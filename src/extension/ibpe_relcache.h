#ifndef IBPE_RELCACHE_H
#define IBPE_RELCACHE_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

#include "ibpe_backend.h"
#include "ibpe_utils.h"

typedef struct
{
    int token;
    BlockNumber blkno;
    int offset;
} ibpe_ptr_record;

// relcache
typedef struct
{
    tokenizer tok;

    int vocab_size;
    ibpe_ptr_record *token_sid_map;
} ibpe_relcache;

void ibpe_store_cache(Relation indexRelation, ibpe_relcache *cur_state);

ibpe_relcache *ibpe_restore_or_create_cache(Relation indexRelation);

void ibpe_relcache_reload_index(ibpe_relcache *cache,
                                Relation indexRelation,
                                ibpe_metapage_data *meta);

#endif // IBPE_RELCACHE_H
