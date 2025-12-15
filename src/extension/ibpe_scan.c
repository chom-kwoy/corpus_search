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

typedef struct
{
    Relation indexRelation;
    ibpe_relcache *cache;
    BufferAccessStrategy bas;
} ibpe_access_index_state;

static int ibpe_access_index(void *user_data, int token, index_entry *data, int num_entries)
{
    ibpe_access_index_state *state = user_data;

    if (token < 0 || token >= state->cache->vocab_size) {
        elog(ERROR, "ibpe_access_index: token %d out of range", token);
    }

    ibpe_ptr_record ptr = state->cache->token_sid_map[token];

    // no entries for this token
    if (ptr.blkno == InvalidBlockNumber) {
        return 0;
    }

    Buffer buffer = ReadBufferExtended(state->indexRelation,
                                       MAIN_FORKNUM,
                                       ptr.blkno,
                                       RBM_NORMAL,
                                       state->bas);
    LockBuffer(buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(buffer);

    char const *begin = PageGetContents(page) + ptr.offset;
    char const *end = page + ((PageHeader) page)->pd_upper;

    int num_elems = *((int *) begin);
    begin += sizeof(int);

    for (int i = 0; i < num_elems; ++i) {
        if (i >= num_entries) {
            break;
        }

        if (begin + sizeof(index_entry) > end) {
            // follow pointer to next page blkno
            int next_blkno = ibpe_get_opaque(page)->next_blkno;
            if (next_blkno == InvalidBlockNumber) {
                elog(ERROR,
                     "ibpe_access_index: unexpected end of pages when reading #%d out of %d "
                     "entries for token %d",
                     i,
                     num_entries,
                     token);
            }

            UnlockReleaseBuffer(buffer);

            // open next page
            buffer = ReadBufferExtended(state->indexRelation,
                                        MAIN_FORKNUM,
                                        next_blkno,
                                        RBM_NORMAL,
                                        state->bas);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buffer);

            begin = PageGetContents(page);
            end = page + ((PageHeader) page)->pd_upper;
        }

        if (data) {
            data[i] = *((index_entry *) begin);
            elog(NOTICE, "token=%d: data[%d]=%lu", token, i, data[i].sent_id);
        }

        begin += sizeof(index_entry);
    }

    UnlockReleaseBuffer(buffer);

    return num_elems;
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
    char const *search_term = text_to_cstring(DatumGetTextPP(skey->sk_argument));
    elog(NOTICE, "ibpe_getbitmap got search text='%s'", search_term);

    // run the actual search
    BufferAccessStrategy bas = GetAccessStrategy(BAS_BULKREAD);

    ibpe_access_index_state access_state = {
        .indexRelation = scan->indexRelation,
        .cache = cache,
        .bas = bas,
    };

    index_accessor_cb callback = {
        .user_data = &access_state,
        .func = ibpe_access_index,
    };
    sentid_vec results = search_corpus(cache->tok, callback, search_term);
    if (!results) {
        elog(WARNING, "Search failed. Returning 0 results");

        FreeAccessStrategy(bas);
        return 0;
    }

    FreeAccessStrategy(bas);

    sentid_t const *data = sentid_vec_get_data(results);
    int size = sentid_vec_get_size(results);

    elog(NOTICE, "ibpe_getbitmap: Found %d results", size);

    for (int i = 0; i < size; ++i) {
        ItemPointerData tid;

        tid.ip_blkid.bi_hi = (data[i] >> 32) & 0xFFFF;
        tid.ip_blkid.bi_lo = (data[i] >> 16) & 0xFFFF;
        tid.ip_posid = data[i] & 0xFFFF;

        elog(NOTICE,
             "bi_hi=%d, bi_lo=%d, posid=%d",
             tid.ip_blkid.bi_hi,
             tid.ip_blkid.bi_lo,
             tid.ip_posid);
    }

    // fill tbm with results
    for (int i = 0; i < size; ++i) {
        ItemPointerData tid;

        tid.ip_blkid.bi_hi = (data[i] >> 32) & 0xFFFF;
        tid.ip_blkid.bi_lo = (data[i] >> 16) & 0xFFFF;
        tid.ip_posid = data[i] & 0xFFFF;

        tbm_add_tuples(tbm, &tid, 1, true);
    }

    destroy_sentid_vec(results);

    return size;
}

/* end index scan */
void ibpe_endscan(IndexScanDesc scan)
{
    elog(NOTICE, "ibpe_endscan called");
}
