#include "ibpe_build.h"
#include "ibpe_relcache.h"
#include "ibpe_utils.h"

#include <access/generic_xlog.h>
#include <access/reloptions.h>
#include <access/tableam.h>
#include <commands/vacuum.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <utils/builtins.h>
#include <utils/rel.h>

// metapage population
static void ibpe_fill_metapage(Relation indexRelation, Page metaPage)
{
    ibpe_options_data *opts = (ibpe_options_data *) indexRelation->rd_options;
    if (!opts) {
        elog(ERROR,
             "tokenizer path not set. "
             "Please specify `WITH (tokenizer_path = '<path to tokenizer.json>').`");
    }

    char const *tok_path = GET_STRING_RELOPTION(opts, tokenizer_path);
    elog(NOTICE, "Got tokenizer path = %s", tok_path);

    PageInit(metaPage, BLCKSZ, sizeof(ibpe_opaque_data));

    ibpe_opaque_data *opaque = ibpe_get_opaque(metaPage);
    opaque->flags = IBPE_PAGE_META;
    opaque->data_len = sizeof(ibpe_metapage_data);
    opaque->next_blkno = InvalidBlockNumber;
    opaque->ibpe_page_id = IBPE_PAGE_ID;

    ibpe_metapage_data *metadata = (ibpe_metapage_data *) PageGetContents(metaPage);
    memset(metadata, 0, sizeof(ibpe_metapage_data));
    metadata->magickNumber = IBPE_MAGICK_NUMBER;
    strncpy(metadata->tokenizer_path, tok_path, TOKENIZER_PATH_MAXLEN);
    metadata->index_built = false;
    metadata->num_indexed_tokens = 0;

    ((PageHeader) metaPage)->pd_lower += sizeof(ibpe_metapage_data);
    Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);
}

static void ibpe_init_metapage(Relation indexRelation, ForkNumber forknum)
{
    /*
     * Make a new page; since it is first page it should be associated with
     * block number 0.  No need to hold the extension
     * lock because there cannot be concurrent inserters yet.
     */
    Buffer metaBuffer = ReadBufferExtended(indexRelation, forknum, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);
    Assert(BufferGetBlockNumber(metaBuffer) == 0 /* meta page */);

    /* Initialize contents of meta page */
    GenericXLogState *state = GenericXLogStart(indexRelation);
    Page metaPage = GenericXLogRegisterBuffer(state, metaBuffer, GENERIC_XLOG_FULL_IMAGE);

    ibpe_fill_metapage(indexRelation, metaPage);

    GenericXLogFinish(state);
    UnlockReleaseBuffer(metaBuffer);
}

// regular page manipulation
static void ibpe_init_page(Page page, uint16 flags)
{
    PageInit(page, BLCKSZ, sizeof(ibpe_opaque_data));

    ibpe_opaque_data *opaque = ibpe_get_opaque(page);
    opaque->flags = flags;
    opaque->data_len = 0;
    opaque->next_blkno = InvalidBlockNumber;
    opaque->ibpe_page_id = IBPE_PAGE_ID;
}

static Buffer ibpe_new_buffer(Relation indexRelation, BlockNumber *new_blkno)
{
    Buffer buffer;
    BlockNumber blkno;

    /* First, try to get a page from FSM */
    for (;;) {
        blkno = GetFreeIndexPage(indexRelation);

        if (blkno == InvalidBlockNumber) {
            break;
        }

        buffer = ReadBuffer(indexRelation, blkno);

        /*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
        if (ConditionalLockBuffer(buffer)) {
            Page page = BufferGetPage(buffer);

            if (PageIsNew(page)) {
                if (new_blkno) {
                    *new_blkno = blkno;
                }
                return buffer; /* OK to use, if never initialized */
            }

            if (ibpe_is_page_deleted(page)) {
                if (new_blkno) {
                    *new_blkno = blkno;
                }
                return buffer; /* OK to use */
            }

            LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
        }

        /* Can't use it, so release buffer and try again */
        ReleaseBuffer(buffer);
    }

    if (new_blkno) {
        *new_blkno = RelationGetNumberOfBlocks(indexRelation);
    }

    /* Must extend the file */
    buffer = ExtendBufferedRel(BMR_REL(indexRelation), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);

    return buffer;
}

static BlockNumber ibpe_flush_page(Relation indexRelation, Page data)
{
    BlockNumber blkno;
    Buffer buffer = ibpe_new_buffer(indexRelation, &blkno);

    GenericXLogState *state = GenericXLogStart(indexRelation);
    Page page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);

    memcpy(page, data, BLCKSZ);

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);

    return blkno;
}

static bool ibpe_add_record_to_page(Page page, char *record, int rec_size, uint16 *out_offset)
{
    /* We shouldn't be pointed to an invalid page */
    Assert(!PageIsNew(page) && !ibpe_is_page_deleted(page));

    /* Does new record fit on the page? */
    if (ibpe_page_get_free_space(page) < rec_size) {
        return false;
    }

    /* Copy new tuple to the end of page */
    ibpe_opaque_data *opaque = ibpe_get_opaque(page);
    char *mem = PageGetContents(page) + opaque->data_len;
    memcpy(mem, record, rec_size);

    /* Store offset to the new data */
    if (out_offset) {
        *out_offset = opaque->data_len;
    }

    /* Adjust maxoff and pd_lower */
    opaque->data_len += rec_size;
    ((PageHeader) page)->pd_lower = (mem + rec_size) - page;

    /* Assert we didn't overrun available space */
    Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);

    return true;
}

static bool ibpe_push_record(Relation indexRelation,
                             Page page,
                             uint16 page_flags,
                             BlockNumber *prev_blkno, // 0 if no previous block exists
                             char *record,
                             int rec_size,
                             uint16 *out_offset)
{
    /* Try to add next item to cached page */
    bool success = ibpe_add_record_to_page(page, record, rec_size, out_offset);

    if (!success) {
        /* Cached page is full, flush it out and make a new one */
        BlockNumber blkno = ibpe_flush_page(indexRelation, page);

        // link previous block (if exists) to the flushed one
        if (*prev_blkno != InvalidBlockNumber) {
            int prev_buf = ReadBuffer(indexRelation, *prev_blkno);
            LockBuffer(prev_buf, BUFFER_LOCK_EXCLUSIVE);

            GenericXLogState *state = GenericXLogStart(indexRelation);
            Page page = GenericXLogRegisterBuffer(state, prev_buf, GENERIC_XLOG_FULL_IMAGE);

            ibpe_get_opaque(page)->next_blkno = blkno;

            GenericXLogFinish(state);
            UnlockReleaseBuffer(prev_buf);
        }

        // update previous block number
        *prev_blkno = blkno;

        ibpe_init_page(page, page_flags);

        if (!ibpe_add_record_to_page(page, record, rec_size, out_offset)) {
            /* We shouldn't be here since we're inserting to the empty page */
            elog(ERROR, "could not add new bloom tuple to empty page");
        }

        return true;
    }

    return false;
}

typedef struct
{
    int token;
    uint16 offset;
} ibpe_token_and_offset;

// build state management
typedef struct
{
    Relation indexRelation;
    int64 indtuples; // total number of tuples indexed
    int num_indexed_tokens;
    tokenizer tok;

    // interface to the C++ backend
    index_builder builder;

    // List of SID records that need to be linked
    // when the SID page is flushed the next time
    int n_records_to_link;
    ibpe_token_and_offset *records_to_link;

    // Currently building pointer page
    BlockNumber ptr_page_prevno;
    PGAlignedBlock ptr_page;

    // Currently building sentence id page
    BlockNumber sid_page_prevno;
    PGAlignedBlock sid_page;
} ibpe_build_state;

#define IBPE_MAX_RECORDS_TO_LINK 4096

/*
 * Per-tuple callback for table_index_build_scan.
 */
static void ibpe_build_callback(Relation indexRelation,
                                ItemPointer tid,
                                Datum *values,
                                bool *isnull,
                                bool tupleIsAlive,
                                void *state)
{
    ibpe_build_state *build_state = (ibpe_build_state *) state;

    if (isnull[0]) {
        return; // skip NULLs
    }

    char *string = TextDatumGetCString(values[0]);

    int tokens[2048] = {};
    int n_tokens = tokenizer_tokenize(build_state->tok, string, tokens, lengthof(tokens));
    if (n_tokens > lengthof(tokens)) {
        elog(ERROR, "String exceeds %zu tokens", lengthof(tokens));
    }

    if (build_state->indtuples < 5) {
        elog(NOTICE,
             "tid=%d/%d/%d, text=%s, isnull=%d, toks=[%d, %d, %d, ...]",
             tid->ip_blkid.bi_hi,
             tid->ip_blkid.bi_lo,
             tid->ip_posid,
             string,
             isnull[0],
             tokens[0],
             tokens[1],
             tokens[2]);
    }

    int sent_id = (tid->ip_blkid.bi_lo << 4) | (tid->ip_blkid.bi_lo << 2) | tid->ip_posid;

    index_builder_add_sentence(build_state->builder, sent_id, tokens, n_tokens);

    build_state->indtuples++;
}

static void ibpe_flush_records_to_link(ibpe_build_state *state)
{
    if (state->n_records_to_link == 0) {
        return; // nothing to flush
    }
    for (int i = 0; i < state->n_records_to_link; ++i) {
        ibpe_ptr_record ptr_record;
        ptr_record.token = state->records_to_link[i].token;
        ptr_record.blkno = state->sid_page_prevno;
        ptr_record.offset = state->records_to_link[i].offset;

        elog(NOTICE,
             "Established link: token %d -> (blkno=%d, offset=%d)",
             ptr_record.token,
             ptr_record.blkno,
             ptr_record.offset);

        ibpe_push_record(state->indexRelation,
                         state->ptr_page.data,
                         IBPE_PAGE_PTR,
                         &state->ptr_page_prevno,
                         (char *) &ptr_record,
                         sizeof(ibpe_ptr_record),
                         NULL);
    }

    // clear buffer
    state->n_records_to_link = 0;
}

static void ibpe_index_builder_iterate(void *user_data,
                                       int token,
                                       struct index_entry const *p_sentids,
                                       int n_sentids)
{
    ibpe_build_state *state = user_data;

    elog(NOTICE,
         "token %d -> Array[%d]{(sid=%d,pos=%d), ...}",
         token,
         n_sentids,
         p_sentids[0].sent_id,
         p_sentids[0].pos);

    uint16 offset;

    // push array size first
    if (ibpe_push_record(state->indexRelation,
                         state->sid_page.data,
                         IBPE_PAGE_SID,
                         &state->sid_page_prevno,
                         (char *) &n_sentids,
                         sizeof(int),
                         &offset)) {
        ibpe_flush_records_to_link(state);
    }
    state->records_to_link[state->n_records_to_link++] = (ibpe_token_and_offset){token, offset};

    // push sid elements
    for (int i = 0; i < n_sentids; ++i) {
        if (ibpe_push_record(state->indexRelation,
                             state->sid_page.data,
                             IBPE_PAGE_SID,
                             &state->sid_page_prevno,
                             (char *) &p_sentids[i],
                             sizeof(struct index_entry),
                             NULL)) {
            ibpe_flush_records_to_link(state);
        }
    }
}

/* build new index */
IndexBuildResult *ibpe_build(Relation heapRelation, Relation indexRelation, IndexInfo *indexInfo)
{
    elog(NOTICE, "ibpe_build called");

    if (RelationGetNumberOfBlocks(indexRelation) != 0)
        elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(indexRelation));

    ibpe_init_metapage(indexRelation, MAIN_FORKNUM);

    ibpe_relcache *cache = ibpe_restore_or_create_cache(indexRelation);

    // initialize build state
    ibpe_build_state build_state;
    build_state.indexRelation = indexRelation;
    build_state.indtuples = 0;
    build_state.num_indexed_tokens = 0;
    build_state.tok = cache->tok;

    build_state.builder = create_index_builder();
    if (!build_state.builder) {
        elog(ERROR, "Cannot allocate index builder");
    }

    build_state.n_records_to_link = 0;
    build_state.records_to_link = palloc0(sizeof(build_state.records_to_link[0])
                                          * IBPE_MAX_RECORDS_TO_LINK);

    // Insert blank starter pages
    ibpe_init_page(build_state.ptr_page.data, IBPE_PAGE_PTR);
    build_state.ptr_page_prevno = ibpe_flush_page(indexRelation, build_state.ptr_page.data);
    Assert(build_state.ptr_page_prevno == 1);

    ibpe_init_page(build_state.sid_page.data, IBPE_PAGE_SID);
    build_state.sid_page_prevno = ibpe_flush_page(indexRelation, build_state.sid_page.data);
    Assert(build_state.sid_page_prevno == 2);

    // scan the heap (table to be indexed)
    double reltuples = table_index_build_scan(heapRelation,
                                              indexRelation,
                                              indexInfo,
                                              true,
                                              true,
                                              ibpe_build_callback,
                                              &build_state,
                                              NULL);
    index_builder_finalize(build_state.builder);

    // Populate index using result from builder
    index_builder_iterate(build_state.builder, ibpe_index_builder_iterate, &build_state);

    // flush remaining pages
    if (build_state.n_records_to_link > 0) {
        build_state.sid_page_prevno = ibpe_flush_page(indexRelation, build_state.sid_page.data);
        ibpe_flush_records_to_link(&build_state);
    }
    ibpe_flush_page(indexRelation, build_state.ptr_page.data);

    // free memory
    destroy_index_builder(build_state.builder);

    // Update metapage
    Buffer buffer = ReadBuffer(indexRelation, 0 /* metapage */);
    LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

    GenericXLogState *state = GenericXLogStart(indexRelation);
    Page metaPage = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);

    ibpe_metapage_data *metadata = (ibpe_metapage_data *) PageGetContents(metaPage);
    metadata->index_built = true;
    metadata->num_indexed_tokens = build_state.num_indexed_tokens;

    // add built index data to relcache
    ibpe_relcache_reload_index(cache, indexRelation, metadata);

    GenericXLogFinish(state);
    UnlockReleaseBuffer(buffer);

    // return results
    IndexBuildResult *result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
    result->heap_tuples = reltuples;
    result->index_tuples = build_state.indtuples;

    return result;
}

/* build empty index */
void ibpe_buildempty(Relation indexRelation)
{
    // TODO
    elog(ERROR, "ibpe_buildempty: Not implemented");
}

/* insert this tuple */
bool ibpe_insert(Relation indexRelation,
                 Datum *values,
                 bool *isnull,
                 ItemPointer heap_tid,
                 Relation heapRelation,
                 IndexUniqueCheck checkUnique,
                 bool indexUnchanged,
                 IndexInfo *indexInfo)
{
    // TODO
    elog(ERROR, "ibpe_insert: Not implemented");
    return false;
}
