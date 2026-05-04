#include "ibpe_scan.h"
#include "ibpe_relcache.h"

#include <access/relscan.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>

typedef struct
{
    ibpe_relcache *state;
} ibpe_scan_opaque;

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys)
{
    elog(NOTICE, "ibpe_rescan called with nkeys=%d, norderbys=%d", nkeys, norderbys);

    ibpe_scan_opaque *scan_state = palloc0(sizeof(ibpe_scan_opaque));
    scan_state->state = ibpe_restore_or_create_cache(indexRelation);

    IndexScanDesc scan = RelationGetIndexScan(indexRelation, nkeys, norderbys);
    scan->opaque = scan_state;

    return scan;
}

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    elog(NOTICE, "ibpe_rescan called with nkeys=%d, norderbys=%d", nkeys, norderbys);

    if (keys && scan->numberOfKeys > 0) {
        memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
}

typedef struct
{
    Relation indexRelation;
    ibpe_relcache *cache;
    BufferAccessStrategy bas;
    // flat array of all pending entries, loaded once per scan
    ibpe_pending_entry *pending;
    int n_pending;
} ibpe_access_index_state;

static int ibpe_access_index(void *user_data, int token, index_entry *data, int num_entries)
{
    ibpe_access_index_state *state = user_data;

    int num_elems = 0;

    if (state->cache->vocab_size > 0 && state->cache->token_sid_map != NULL) {
        if (token < 0 || token >= state->cache->vocab_size)
            elog(ERROR, "ibpe_access_index: token %d out of range", token);
    }

    ibpe_ptr_record ptr = (state->cache->vocab_size > 0 && state->cache->token_sid_map != NULL)
                              ? state->cache->token_sid_map[token]
                              : (ibpe_ptr_record){.token = token, .blkno = InvalidBlockNumber, .offset = -1};

    if (ptr.blkno != InvalidBlockNumber) {
        Buffer buffer = ReadBufferExtended(state->indexRelation,
                                           MAIN_FORKNUM,
                                           ptr.blkno,
                                           RBM_NORMAL,
                                           state->bas);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);

        char const *begin = PageGetContents(page) + ptr.offset;
        char const *end = page + ((PageHeader) page)->pd_upper;

        num_elems = *((int *) begin);
        begin += sizeof(int);

        for (int i = 0; i < num_elems; ++i) {
            if (i >= num_entries)
                break;

            if (begin + sizeof(index_entry) > end) {
                int next_blkno = ibpe_get_opaque(page)->next_blkno;
                if (next_blkno == InvalidBlockNumber)
                    elog(ERROR,
                         "ibpe_access_index: unexpected end of pages when reading #%d out of %d "
                         "entries for token %d",
                         i,
                         num_entries,
                         token);

                UnlockReleaseBuffer(buffer);

                buffer = ReadBufferExtended(state->indexRelation,
                                            MAIN_FORKNUM,
                                            next_blkno,
                                            RBM_NORMAL,
                                            state->bas);
                LockBuffer(buffer, BUFFER_LOCK_SHARE);
                page = BufferGetPage(buffer);

                begin = PageGetContents(page);
                end = page + ((PageHeader) page)->pd_upper;
            }

            if (data) {
                data[i] = *((index_entry *) begin);
                if (data[i].sent_id == 0)
                    elog(NOTICE, "token=%d: data[%d]=%lu", token, i, (sentid_t) data[i].sent_id);
            }

            begin += sizeof(index_entry);
        }

        UnlockReleaseBuffer(buffer);
    }

    // Append any pending entries for this token
    int pending_added = 0;
    for (int p = 0; p < state->n_pending; p++) {
        if (state->pending[p].token != token)
            continue;
        if (data && num_elems + pending_added < num_entries)
            data[num_elems + pending_added] = state->pending[p].entry;
        pending_added++;
    }

    return num_elems + pending_added;
}

/* fetch all valid tuples */
int64 ibpe_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    ibpe_scan_opaque *scan_state = scan->opaque;
    ibpe_relcache *cache = scan_state->state;

    ScanKey skey = scan->keyData;

    elog(NOTICE,
         "ibpe_getbitmap called with sk_flags=%u, sk_attno=%d, numberofkeys=%d",
         skey->sk_flags,
         skey->sk_attno,
         scan->numberOfKeys);

    if (skey->sk_flags & SK_ISNULL) {
        // search for NULL - no entries
        return 0;
    }

    if (skey->sk_strategy != IBPE_STRATEGY_REGEX /* check strategy number */
        || skey->sk_subtype != TEXTOID           /* check if skey holds string */
        || skey->sk_attno != 1                   /* first column */
        || scan->numberOfKeys != 1               /* only one key supported */
    ) {
        elog(ERROR, "ibpe_getbitmap: Unsupported scan key");
    }

    // get text from skey
    char const *search_term = text_to_cstring(DatumGetTextPP(skey->sk_argument));
    elog(NOTICE, "ibpe_getbitmap got search text='%s'", search_term);

    // run the actual search
    BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

    // Load all pending entries into a flat array for this scan
    ibpe_pending_entry *pending_arr = NULL;
    int n_pending = 0;

    Buffer meta_buf = ReadBuffer(scan->indexRelation, 0 /* metapage */);
    LockBuffer(meta_buf, BUFFER_LOCK_SHARE);
    ibpe_metapage_data *meta = (ibpe_metapage_data *) PageGetContents(BufferGetPage(meta_buf));
    BlockNumber pending_blkno = meta->pending_blkno;
    int pending_total = meta->n_pending;
    UnlockReleaseBuffer(meta_buf);

    if (pending_blkno != InvalidBlockNumber && pending_total > 0) {
        pending_arr = palloc(pending_total * sizeof(ibpe_pending_entry));

        BlockNumber blkno = pending_blkno;
        while (blkno != InvalidBlockNumber) {
            Buffer buf = ReadBufferExtended(scan->indexRelation,
                                            MAIN_FORKNUM,
                                            blkno,
                                            RBM_NORMAL,
                                            bas);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buf);
            ibpe_opaque_data *opaque = ibpe_get_opaque(page);

            Assert((opaque->flags & IBPE_PAGE_PENDING) != 0);

            char *p = PageGetContents(page);
            char *end = p + opaque->data_len;
            while (p + sizeof(ibpe_pending_entry) <= end) {
                if (n_pending < pending_total)
                    pending_arr[n_pending++] = *((ibpe_pending_entry *) p);
                p += sizeof(ibpe_pending_entry);
            }

            blkno = opaque->next_blkno;
            UnlockReleaseBuffer(buf);
        }
        elog(NOTICE, "ibpe_getbitmap: loaded %d pending entries", n_pending);
    }

    ibpe_access_index_state access_state = {
        .indexRelation = scan->indexRelation,
        .cache = cache,
        .bas = bas,
        .pending = pending_arr,
        .n_pending = n_pending,
    };

    index_accessor_cb callback = {
        .user_data = &access_state,
        .func = ibpe_access_index,
    };
    search_result results = search_corpus(cache->tok, callback, search_term);
    if (!results.candidates) {
        elog(WARNING, "Search failed. Returning 0 results");

        FreeAccessStrategy(bas);
        return 0;
    }

    FreeAccessStrategy(bas);

    sentid_t const *data = sentid_vec_get_data(results.candidates);
    int size = sentid_vec_get_size(results.candidates);

    elog(NOTICE, "ibpe_getbitmap: Found %d results", size);

    // fill tbm with results
    for (int i = 0; i < size; ++i) {
        if (data[i] != 0) { // FIXME: search returns invalid sent_id=0 sometimes
            ItemPointerData tid = ibpe_sentid_to_tid(data[i]);
            tbm_add_tuples(tbm, &tid, 1, results.needs_recheck);
        }
    }

    destroy_sentid_vec(results.candidates);

    return size;
}

/* end index scan */
void ibpe_endscan(IndexScanDesc scan)
{
    elog(NOTICE, "ibpe_endscan called");
}
