#include "ibpe_vacuum.h"
#include "ibpe_backend.h"
#include "ibpe_utils.h"

#include <access/genam.h>
#include <access/generic_xlog.h>
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
    Relation index = info->index;

    if (stats == NULL)
        stats = palloc0_object(IndexBulkDeleteResult);

    /*
     * Load pending chain info from metapage.  Dead entries in the main bulk
     * SID pages are left in place; needs_recheck=true means PostgreSQL rechecks
     * every TID and discards dead heap tuples automatically.  We only compact
     * the pending list, which is small and fully under our control.
     */
    Buffer meta_buf = ReadBufferExtended(index, MAIN_FORKNUM, 0, RBM_NORMAL, info->strategy);
    LockBuffer(meta_buf, BUFFER_LOCK_SHARE);
    ibpe_metapage_data *meta = (ibpe_metapage_data *) PageGetContents(BufferGetPage(meta_buf));
    BlockNumber pending_head = meta->pending_blkno;
    int n_pending_total = meta->n_pending;
    UnlockReleaseBuffer(meta_buf);

    int n_deleted = 0;

    /* Walk the pending chain, zeroing out dead entries in-place. */
    if (pending_head != InvalidBlockNumber && n_pending_total > 0) {
        BlockNumber blkno = pending_head;
        while (blkno != InvalidBlockNumber) {
            vacuum_delay_point(false);

            Buffer buf = ReadBufferExtended(index,
                                            MAIN_FORKNUM,
                                            blkno,
                                            RBM_NORMAL,
                                            info->strategy);
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            Page page = BufferGetPage(buf);
            ibpe_opaque_data *opaque = ibpe_get_opaque(page);

            Assert((opaque->flags & IBPE_PAGE_PENDING) != 0);

            BlockNumber next = opaque->next_blkno;

            /* First pass: check whether this page has any dead entries. */
            bool has_dead = false;
            {
                char *p = PageGetContents(page);
                char *end = p + opaque->data_len;
                while (p + sizeof(ibpe_pending_entry) <= end) {
                    ibpe_pending_entry *ent = (ibpe_pending_entry *) p;
                    if (ent->entry.sent_id != 0) {
                        ItemPointerData tid = ibpe_sentid_to_tid(ent->entry.sent_id);
                        if (callback(&tid, callback_state)) {
                            has_dead = true;
                            break;
                        }
                    }
                    p += sizeof(ibpe_pending_entry);
                }
            }

            if (has_dead) {
                /*
                 * Second pass: zero out dead entries via WAL.  Setting token=-1
                 * prevents the scan from matching this entry (all valid tokens
                 * are >= 0).  Setting sent_id=0 makes ibpe_getbitmap skip it
                 * even if a stale scan loads it.
                 */
                GenericXLogState *xlog_state = GenericXLogStart(index);
                Page xlog_page = GenericXLogRegisterBuffer(xlog_state,
                                                           buf,
                                                           GENERIC_XLOG_FULL_IMAGE);

                char *p = PageGetContents(xlog_page);
                char *end = p + ibpe_get_opaque(xlog_page)->data_len;
                while (p + sizeof(ibpe_pending_entry) <= end) {
                    ibpe_pending_entry *ent = (ibpe_pending_entry *) p;
                    if (ent->entry.sent_id != 0) {
                        ItemPointerData tid = ibpe_sentid_to_tid(ent->entry.sent_id);
                        if (callback(&tid, callback_state)) {
                            ent->token = -1;
                            ent->entry.sent_id = 0;
                            n_deleted++;
                        }
                    }
                    p += sizeof(ibpe_pending_entry);
                }

                GenericXLogFinish(xlog_state);
            }

            UnlockReleaseBuffer(buf);
            blkno = next;
        }
    }

    stats->tuples_removed += n_deleted;

    /*
     * We intentionally leave meta->n_pending unchanged even though some
     * entries were zeroed out.  n_pending is used only as an allocation
     * bound in ibpe_getbitmap; dead entries (token = -1) are silently
     * skipped by the token-matching loop in ibpe_access_index, so keeping
     * the physical count correct avoids a subtle scan truncation bug.
     */

    /* Count pages and SID index tuples for planner statistics. */
    BlockNumber npages = RelationGetNumberOfBlocks(index);
    stats->num_pages = npages;

    for (BlockNumber blkno = 1; blkno < npages; blkno++) {
        vacuum_delay_point(false);

        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, blkno, RBM_NORMAL, info->strategy);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);

        if (PageIsNew(page) || ibpe_is_page_deleted(page)) {
            stats->pages_free++;
        } else {
            ibpe_opaque_data *opaque = ibpe_get_opaque(page);
            if (opaque->flags & IBPE_PAGE_SID)
                stats->num_index_tuples += opaque->data_len / sizeof(index_entry);
        }

        UnlockReleaseBuffer(buf);
    }

    return stats;
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
            ibpe_opaque_data *opaque = ibpe_get_opaque(page);
            if (opaque->flags & IBPE_PAGE_SID)
                stats->num_index_tuples += opaque->data_len / sizeof(index_entry);
        }

        UnlockReleaseBuffer(buffer);
    }

    IndexFreeSpaceMapVacuum(info->index);

    return stats;
}
