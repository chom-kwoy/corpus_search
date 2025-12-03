#ifndef IBPE_UTILS_H
#define IBPE_UTILS_H

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
typedef struct __attribute__((packed))
{
    uint16 flags;    // see #defines below
    uint16 data_len; // length of the data, in bytes
    BlockNumber next_blkno;
    uint16 ibpe_page_id; // must equal IBPE_PAGE_ID, aligned at the end of the page
} ibpe_opaque_data;

// page flags
#define IBPE_PAGE_META (1 << 0)
#define IBPE_PAGE_DELETED (1 << 1)
#define IBPE_PAGE_PTR (1 << 2) // containing pageid and offset for each token
#define IBPE_PAGE_SID (1 << 3) // containing sentence ids

#define IBPE_PAGE_ID (0x1B9E)

// data structure stored in the meta page (page #0 of the index relation)
#define TOKENIZER_PATH_MAXLEN 255
typedef struct
{
    uint32 magickNumber; // must equal IBPE_MAGICK_NUMBER
    char tokenizer_path[TOKENIZER_PATH_MAXLEN + 1];
} ibpe_metapage_data;

#define IBPE_MAGICK_NUMBER (0xFEEDBEEF)

// page related utils
ibpe_opaque_data *ibpe_get_opaque(Page page);

bool ibpe_is_page_deleted(Page page);

int ibpe_page_get_free_space(Page page);

// Callback routines

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

#endif // IBPE_UTILS_H
