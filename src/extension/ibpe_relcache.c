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

static ibpe_relcache *ibpe_fill_cache(Relation indexRelation, ibpe_metapage_data *meta)
{
    // allocate memory for amcache
    ibpe_relcache *state_mem = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));

    // initialize tokenizer
    elog(NOTICE, "Loading tokenizer from '%s'", meta->tokenizer_path);
    char errmsg[256] = {};
    state_mem->tok = create_tokenizer(meta->tokenizer_path, errmsg, 256);
    if (!state_mem->tok) {
        elog(ERROR, "Cannot load tokenizer: %s", errmsg);
    }

    // free tokenizer when cache is freed
    MemoryContextCallback *cb = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                   sizeof(MemoryContextCallback));
    cb->func = ibpe_free_relcache_callback;
    cb->arg = state_mem;
    MemoryContextRegisterResetCallback(indexRelation->rd_indexcxt, cb);

    // Load index if already built
    if (meta->index_built) {
        elog(NOTICE, "Loading index from disk");

        BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

        Buffer ptr_page_buf = ReadBufferExtended(indexRelation, MAIN_FORKNUM, 1, RBM_NORMAL, bas);
        LockBuffer(ptr_page_buf, BUFFER_LOCK_SHARE);

        Page page = BufferGetPage(ptr_page_buf);

        for (;;) {
            Assert(ibpe_get_opaque(page)->ibpe_page_id == IBPE_PAGE_ID);
            Assert((ibpe_get_opaque(page)->flags & IBPE_PAGE_PTR) != 0);

            // TODO: read contents from page

            BlockNumber next_blkno = ibpe_get_opaque(page)->next_blkno;
            if (next_blkno == InvalidBlockNumber) {
                break;
            }

            UnlockReleaseBuffer(ptr_page_buf);

            elog(NOTICE, "Reading page #%d", next_blkno);

            ptr_page_buf = ReadBufferExtended(indexRelation,
                                              MAIN_FORKNUM,
                                              next_blkno,
                                              RBM_NORMAL,
                                              bas);
            LockBuffer(ptr_page_buf, BUFFER_LOCK_SHARE);

            page = BufferGetPage(ptr_page_buf);
        }

        UnlockReleaseBuffer(ptr_page_buf);
    }

    return state_mem;
}

ibpe_relcache ibpe_restore_or_create_cache(Relation indexRelation)
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
        state_mem = ibpe_fill_cache(indexRelation, meta);

        UnlockReleaseBuffer(buffer);

        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }
    return *state_mem;
}
