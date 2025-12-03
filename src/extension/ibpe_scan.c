#include "ibpe_scan.h"
#include "ibpe_relcache.h"

#include <access/relscan.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <storage/bufmgr.h>

typedef struct
{
    ibpe_relcache state;
} ibpe_scan_opaque;

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys)
{
    elog(NOTICE, "ibpe_rescan called with nkeys=%d, norderbys=%d", nkeys, norderbys);

    ibpe_scan_opaque *scan_state = palloc0(sizeof(ibpe_scan_opaque));
    scan_state->state = ibpe_restore_or_create_cache(indexRelation);

    IndexScanDesc scan = RelationGetIndexScan(indexRelation, nkeys, norderbys);
    scan->opaque = scan_state;

    return scan;
}

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    elog(NOTICE, "ibpe_rescan called with nkeys=%d, norderbys=%d", nkeys, norderbys);

    if (keys && scan->numberOfKeys > 0) {
        memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
}

/* fetch all valid tuples */
int64 ibpe_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    // TODO
    elog(ERROR, "ibpe_getbitmap: Not implemented");
}

/* end index scan */
void ibpe_endscan(IndexScanDesc scan)
{
    elog(NOTICE, "ibpe_endscan called");
}
