#ifndef IBPE_VACUUM_H
#define IBPE_VACUUM_H

#include <postgres.h>
// The include order is important
#include <access/amapi.h>
#include <fmgr.h>

/* bulk delete */
IndexBulkDeleteResult *ibpe_bulkdelete(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats,
                                       IndexBulkDeleteCallback callback,
                                       void *callback_state);

/* post-VACUUM cleanup */
IndexBulkDeleteResult *ibpe_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

#endif // IBPE_VACUUM_H
