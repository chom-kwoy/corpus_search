#ifndef IBPE_RELCACHE_H
#define IBPE_RELCACHE_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

#include "ibpe_backend.h"

// relcache
typedef struct
{
    tokenizer tok;
} ibpe_relcache;

void ibpe_store_cache(Relation indexRelation, ibpe_relcache *cur_state);

ibpe_relcache ibpe_restore_or_create_cache(Relation indexRelation);

#endif // IBPE_RELCACHE_H
