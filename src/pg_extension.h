#ifndef PG_EXTENSION_H
#define PG_EXTENSION_H

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

/* bulk delete */
IndexBulkDeleteResult *ibpe_bulkdelete(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats,
                                       IndexBulkDeleteCallback callback,
                                       void *callback_state);

/* post-VACUUM cleanup */
IndexBulkDeleteResult *ibpe_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

/* estimate cost of an indexscan */
void ibpe_costestimate(struct PlannerInfo *root,
                       struct IndexPath *path,
                       double loop_count,
                       Cost *indexStartupCost,
                       Cost *indexTotalCost,
                       Selectivity *indexSelectivity,
                       double *indexCorrelation,
                       double *indexPages);

/* parse index reloptions */
bytea *ibpe_options(Datum reloptions, bool validate);

/* validate definition of an opclass for this AM */
bool ibpe_validate(Oid opclassoid);

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys);

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);

/* fetch all valid tuples */
int64 ibpe_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/* end index scan */
void ibpe_endscan(IndexScanDesc scan);

#endif // PG_EXTENSION_H
