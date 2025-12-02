#ifndef PG_EXTENSION_H
#define PG_EXTENSION_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

typedef struct
{
    int32 vl_len_;      // varlena header
    int tokenizer_path; // string option
} ibpe_options_data;

// opaque is a special area at the end of all index pages
typedef struct
{
    uint16 flags;        // see #defines below
    uint16 ibpe_page_id; // must equal IBPE_PAGE_ID, aligned at the end of the page
} ibpe_opaque_data;

#define IBPE_PAGE_META (1 << 0)
#define IBPE_PAGE_DELETED (1 << 1)

#define IBPE_PAGE_ID (0x1B9E)

// data structure stored in the meta page (page #0 of the index relation)
#define TOKENIZER_PATH_MAXLEN 255
typedef struct
{
    uint32 magickNumber; // must equal IBPE_MAGICK_NUMBER
    char tokenizer_path[TOKENIZER_PATH_MAXLEN + 1];
} ibpe_metapage_data;

#define IBPE_MAGICK_NUMBER (0xFEEDBEEF)

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
