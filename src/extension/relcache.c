#include "pg_extension.h"

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

void ibpe_free_relcache(void *arg)
{
    elog(NOTICE, "freeing rd_amcache");

    ibpe_relcache *state_mem = arg;

    destroy_tokenizer(state_mem->tok);
}

ibpe_relcache ibpe_restore_or_create_cache(Relation indexRelation)
{
    ibpe_relcache *state_mem;
    if (!indexRelation->rd_amcache) {
        // allocate memory for amcache
        state_mem = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));

        // restore state from metapage
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

        UnlockReleaseBuffer(buffer);

        // initialize tokenizer
        elog(NOTICE, "Loading tokenizer from '%s'", meta->tokenizer_path);
        char errmsg[256] = {};
        state_mem->tok = create_tokenizer(meta->tokenizer_path, errmsg, 256);
        if (!state_mem->tok) {
            elog(ERROR, "Cannot load tokenizer: %s", errmsg);
        }

        // free tokenizer before cache is freed
        MemoryContextCallback *cb = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                       sizeof(MemoryContextCallback));
        cb->func = ibpe_free_relcache;
        cb->arg = state_mem;
        MemoryContextRegisterResetCallback(indexRelation->rd_indexcxt, cb);

        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }
    return *state_mem;
}
