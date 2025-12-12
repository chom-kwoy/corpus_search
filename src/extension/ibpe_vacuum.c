#include "ibpe_vacuum.h"
#include "ibpe_backend.h"
#include "ibpe_utils.h"

#include <access/genam.h>
#include <commands/vacuum.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *ibpe_bulkdelete(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats,
                                       IndexBulkDeleteCallback callback,
                                       void *callback_state)
{
    // TODO
    elog(ERROR, "ibpe_bulkdelete: Not implemented");
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *ibpe_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    elog(NOTICE, "ibpe_vacuumcleanup called.");

    Relation index = info->index;

    if (info->analyze_only)
        return stats;

    if (stats == NULL)
        stats = palloc0_object(IndexBulkDeleteResult);

    /*
	 * Iterate over the pages: insert deleted pages into FSM and collect
	 * statistics.
	 */
    BlockNumber npages = RelationGetNumberOfBlocks(index);
    stats->num_pages = npages;
    stats->pages_free = 0;
    stats->num_index_tuples = 0;

    for (BlockNumber blkno = 1; blkno < npages; blkno++) {
        vacuum_delay_point(false); // give a chance to interrupt

        Buffer buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);

        if (PageIsNew(page) || ibpe_is_page_deleted(page)) {
            RecordFreeIndexPage(index, blkno);
            stats->pages_free++;
        } else {
            stats->num_index_tuples += ibpe_get_opaque(page)->data_len / sizeof(index_entry);
        }

        UnlockReleaseBuffer(buffer);
    }

    IndexFreeSpaceMapVacuum(info->index);

    return stats;
}
