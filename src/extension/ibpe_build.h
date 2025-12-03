#ifndef IBPE_BUILD_H
#define IBPE_BUILD_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

/* build new index */
IndexBuildResult *ibpe_build(Relation heapRelation,
                             Relation indexRelation,
                             struct IndexInfo *indexInfo);

/* build empty index */
void ibpe_buildempty(Relation indexRelation);

/* insert this tuple */
bool ibpe_insert(Relation indexRelation,
                 Datum *values,
                 bool *isnull,
                 ItemPointer heap_tid,
                 Relation heapRelation,
                 IndexUniqueCheck checkUnique,
                 bool indexUnchanged,
                 struct IndexInfo *indexInfo);

#endif // IBPE_BUILD_H
