#include "ibpe_scan.h"
#include "ibpe_relcache.h"

#include <access/relscan.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>

typedef struct
{
    ibpe_relcache *state;
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
    ibpe_scan_opaque *scan_state = scan->opaque;
    ibpe_relcache *cache = scan_state->state;

    ScanKey skey = scan->keyData;

    elog(NOTICE,
         "ibpe_getbitmap called with sk_flags=%u, sk_attno=%d, numberofkeys=%d",
         skey->sk_flags,
         skey->sk_attno,
         scan->numberOfKeys);

    if (skey->sk_flags & SK_ISNULL) {
        // search for NULL - no entries
        return 0;
    }

    if (skey->sk_strategy != IBPE_STRATEGY_REGEX /* check strategy number */
        || skey->sk_subtype != TEXTOID           /* check if skey holds string */
        || skey->sk_attno != 1                   /* first column */
        || scan->numberOfKeys != 1               /* only one key supported */
    ) {
        elog(ERROR, "ibpe_getbitmap: Unsupported scan key");
    }

    // get text from skey
    char const *search_text = text_to_cstring(DatumGetTextPP(skey->sk_argument));

    elog(NOTICE, "ibpe_getbitmap got search text='%s'", search_text);

    BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

    // TODO
    elog(ERROR, "ibpe_getbitmap: Not implemented");
}

/* end index scan */
void ibpe_endscan(IndexScanDesc scan)
{
    elog(NOTICE, "ibpe_endscan called");
}
