#ifndef IBPE_SCAN_H
#define IBPE_SCAN_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys);

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);

/* fetch all valid tuples */
int64 ibpe_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/* end index scan */
void ibpe_endscan(IndexScanDesc scan);

#endif // IBPE_SCAN_H
