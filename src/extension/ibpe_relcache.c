#include "ibpe_relcache.h"

#include "ibpe_utils.h"

#include <access/generic_xlog.h>
#include <access/reloptions.h>
#include <access/tableam.h>
#include <commands/vacuum.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <utils/builtins.h>
#include <utils/rel.h>

void ibpe_store_cache(Relation indexRelation, ibpe_relcache *cur_state)
{
    ibpe_relcache *state_mem;
    if (!indexRelation->rd_amcache) {
        // allocate memory for amcache
        state_mem = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));
        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }

    // store current state into rd_amcache
    *state_mem = *cur_state;
}

static void ibpe_free_relcache_callback(void *arg)
{
    elog(NOTICE, "freeing rd_amcache");

    ibpe_relcache *state_mem = arg;

    destroy_tokenizer(state_mem->tok);
}

void ibpe_relcache_reload_index(ibpe_relcache *cache,
                                Relation indexRelation,
                                ibpe_metapage_data *meta)
{
    elog(NOTICE, "Loading index from disk");
    if (!meta->index_built) {
        elog(NOTICE, "Index not built yet. Exiting");
        return;
    }

    elog(NOTICE, "metadata: %d tokens found in index", meta->num_indexed_tokens);

    cache->vocab_size = tokenizer_get_vocab_size(cache->tok);
    cache->token_sid_map = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                              sizeof(ibpe_ptr_record) * cache->vocab_size);

    BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

    int token_recs_added = 0;
    int tok_id = 0;

    BlockNumber blkno = 1; // start of ptr pages
    for (;;) {
        elog(NOTICE, "Reading page #%d / %d", blkno, RelationGetNumberOfBlocks(indexRelation));

        Buffer ptr_page_buf = ReadBufferExtended(indexRelation, MAIN_FORKNUM, blkno, RBM_NORMAL, bas);
        LockBuffer(ptr_page_buf, BUFFER_LOCK_SHARE);

        Page page = BufferGetPage(ptr_page_buf);

        ibpe_opaque_data *opaque = ibpe_get_opaque(page);
        Assert(opaque->ibpe_page_id == IBPE_PAGE_ID);
        Assert((opaque->flags & IBPE_PAGE_PTR) != 0);

        // Read contents from page
        char *p = PageGetContents(page);
        while (p < PageGetContents(page) + opaque->data_len) {
            ibpe_ptr_record *rec = (ibpe_ptr_record *) p;

            if (token_recs_added < 5) {
                elog(NOTICE,
                     "Read mapping: token %d -> (blkno=%d, offset=%d)",
                     rec->token,
                     rec->blkno,
                     rec->offset);
            }

            for (; tok_id < rec->token; tok_id++) {
                // fill missing tokens with invalid pointers
                cache->token_sid_map[tok_id] = (ibpe_ptr_record){
                    .token = tok_id,
                    .blkno = InvalidBlockNumber,
                    .offset = -1,
                };
            };

            cache->token_sid_map[tok_id++] = (ibpe_ptr_record){
                .token = rec->token,
                .blkno = rec->blkno,
                .offset = rec->offset,
            };
            token_recs_added += 1;

            p += sizeof(ibpe_ptr_record);
        }

        // follow pointer to next page
        elog(NOTICE, "Got next blkno = %u", opaque->next_blkno);

        if (opaque->next_blkno == InvalidBlockNumber) {
            UnlockReleaseBuffer(ptr_page_buf);
            break;
        }

        blkno = opaque->next_blkno;

        UnlockReleaseBuffer(ptr_page_buf);
    }

    for (; tok_id < cache->vocab_size; tok_id++) {
        // fill missing tokens with invalid pointers
        cache->token_sid_map[tok_id] = (ibpe_ptr_record){
            .token = tok_id,
            .blkno = InvalidBlockNumber,
            .offset = -1,
        };
    }

    FreeAccessStrategy(bas);

    elog(NOTICE, "Reading End. Added %d tokens", token_recs_added);

    Assert(token_recs_added == meta->num_indexed_tokens);
}

static ibpe_relcache *ibpe_relcache_fill(Relation indexRelation, ibpe_metapage_data *meta)
{
    // allocate memory for amcache
    ibpe_relcache *cache = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));

    // initialize tokenizer
    elog(NOTICE,
         "Loading tokenizer from '%s' with %d mappings",
         meta->tokenizer_path,
         meta->n_normalize_mappings);

    char errmsg[256] = {};
    cache->tok = create_tokenizer(meta->tokenizer_path,
                                  meta->normalize_mappings,
                                  meta->n_normalize_mappings,
                                  errmsg,
                                  lengthof(errmsg));
    if (!cache->tok) {
        elog(ERROR, "Cannot load tokenizer: %s", errmsg);
    }

    cache->n_normalize_mappings = meta->n_normalize_mappings;
    cache->normalize_mappings = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                   sizeof(char[2]) * meta->n_normalize_mappings);
    memcpy(cache->normalize_mappings,
           meta->normalize_mappings,
           sizeof(char[2]) * meta->n_normalize_mappings);

    // free tokenizer when cache is freed
    MemoryContextCallback *cb = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                   sizeof(MemoryContextCallback));
    cb->func = ibpe_free_relcache_callback;
    cb->arg = cache;
    MemoryContextRegisterResetCallback(indexRelation->rd_indexcxt, cb);

    // Load index if already built
    ibpe_relcache_reload_index(cache, indexRelation, meta);

    return cache;
}

ibpe_relcache *ibpe_restore_or_create_cache(Relation indexRelation)
{
    ibpe_relcache *state_mem;
    if (!indexRelation->rd_amcache) {
        // read meta page
        Buffer buffer = ReadBuffer(indexRelation, 0 /* meta page */);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        Page page = BufferGetPage(buffer);

        if (ibpe_get_opaque(page)->ibpe_page_id != IBPE_PAGE_ID) {
            elog(ERROR, "Relation is not an ibpe index: page id does not match.");
        }

        ibpe_metapage_data *meta = (ibpe_metapage_data *) PageGetContents(page);
        if (meta->magickNumber != IBPE_MAGICK_NUMBER) {
            elog(ERROR, "Relation is not an ibpe index: invalid magick number.");
        }

        // restore state from metapage
        state_mem = ibpe_relcache_fill(indexRelation, meta);

        UnlockReleaseBuffer(buffer);

        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }
    return state_mem;
}
