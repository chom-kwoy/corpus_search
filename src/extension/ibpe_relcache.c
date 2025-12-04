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

    cache->num_indexed_tokens = meta->num_indexed_tokens;
    cache->token_sid_map = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                              sizeof(ibpe_ptr_record) * meta->num_indexed_tokens);

    BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

    Buffer ptr_page_buf = ReadBufferExtended(indexRelation, MAIN_FORKNUM, 1, RBM_NORMAL, bas);
    LockBuffer(ptr_page_buf, BUFFER_LOCK_SHARE);

    Page page = BufferGetPage(ptr_page_buf);

    int tokens_added = 0;
    for (;;) {
        ibpe_opaque_data *opaque = ibpe_get_opaque(page);
        Assert(opaque->ibpe_page_id == IBPE_PAGE_ID);
        Assert((opaque->flags & IBPE_PAGE_PTR) != 0);

        // Read contents from page
        char *p = PageGetContents(page);
        while (p < PageGetContents(page) + opaque->data_len) {
            ibpe_ptr_record *rec = (ibpe_ptr_record *) p;

            elog(NOTICE,
                 "Read: token %d -> (blkno=%d, offset=%d)",
                 rec->token,
                 rec->blkno,
                 rec->offset);

            cache->token_sid_map[tokens_added++] = (ibpe_ptr_record){
                rec->token,
                rec->blkno,
                rec->offset,
            };

            p += sizeof(ibpe_ptr_record);
        }

        BlockNumber next_blkno = opaque->next_blkno;
        if (next_blkno == InvalidBlockNumber) {
            break;
        }

        UnlockReleaseBuffer(ptr_page_buf);

        elog(NOTICE, "Reading page #%d", next_blkno);

        ptr_page_buf = ReadBufferExtended(indexRelation, MAIN_FORKNUM, next_blkno, RBM_NORMAL, bas);
        LockBuffer(ptr_page_buf, BUFFER_LOCK_SHARE);

        page = BufferGetPage(ptr_page_buf);
    }

    UnlockReleaseBuffer(ptr_page_buf);
}

static ibpe_relcache *ibpe_relcache_fill(Relation indexRelation, ibpe_metapage_data *meta)
{
    // allocate memory for amcache
    ibpe_relcache *cache = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));

    // initialize tokenizer
    elog(NOTICE, "Loading tokenizer from '%s'", meta->tokenizer_path);
    char errmsg[256] = {};
    cache->tok = create_tokenizer(meta->tokenizer_path, errmsg, 256);
    if (!cache->tok) {
        elog(ERROR, "Cannot load tokenizer: %s", errmsg);
    }

    // free tokenizer when cache is freed
    MemoryContextCallback *cb = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                   sizeof(MemoryContextCallback));
    cb->func = ibpe_free_relcache_callback;
    cb->arg = cache;
    MemoryContextRegisterResetCallback(indexRelation->rd_indexcxt, cb);

    // Load index if already built
    if (meta->index_built) {
        ibpe_relcache_reload_index(cache, indexRelation, meta);
    }

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
