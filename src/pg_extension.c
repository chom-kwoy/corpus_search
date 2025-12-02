#include "pg_extension.h"

#include "corpus_search.h"

#include <string.h>

#include <access/generic_xlog.h>
#include <access/reloptions.h>
#include <access/tableam.h>
#include <commands/vacuum.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/rel.h>

PG_MODULE_MAGIC_EXT(.name = "ibpe", .version = PG_VERSION);

// index options
static relopt_kind ibpe_relopt_kind;
static relopt_parse_elt ibpe_relopt_tab[1];

void _PG_init(void)
{
    elog(NOTICE, "_PG_init called");

    ibpe_relopt_kind = add_reloption_kind();

    // path to tokenizer
    add_string_reloption(ibpe_relopt_kind,
                         "tokenizer_path",
                         "Path to tokenizer.json",
                         "", // default value
                         NULL,
                         AccessExclusiveLock);
    ibpe_relopt_tab[0].optname = "tokenizer_path";
    ibpe_relopt_tab[0].opttype = RELOPT_TYPE_STRING;
    ibpe_relopt_tab[0].offset = offsetof(ibpe_options_data, tokenizer_path);
}

PG_FUNCTION_INFO_V1(ibpe_handler);
Datum ibpe_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    // Total number of strategies (operators) by which we can traverse/search this AM.
    // We support only one strategy: ~ (regex match operator).
    amroutine->amstrategies = 1;

    // Total number of support functions that this AM uses
    // We don't need any support functions.
    amroutine->amsupport = 0;

    // opclass options support function number or 0
    // We don't have an options support function.
    amroutine->amoptsprocnum = 0;

    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = false;
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = false;
    amroutine->amconsistentordering = false;
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = true;
    amroutine->amoptionalkey = true;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
    amroutine->amcanbuildparallel = false;
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL
                                         | VACUUM_OPTION_PARALLEL_CLEANUP;
    amroutine->amkeytype = InvalidOid;

    amroutine->ambuild = ibpe_build;
    amroutine->ambuildempty = ibpe_buildempty;
    amroutine->aminsert = ibpe_insert;
    amroutine->aminsertcleanup = NULL;
    amroutine->ambulkdelete = ibpe_bulkdelete;
    amroutine->amvacuumcleanup = ibpe_vacuumcleanup;
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = ibpe_costestimate;
    amroutine->amgettreeheight = NULL;
    amroutine->amoptions = ibpe_options;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = ibpe_validate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = ibpe_beginscan;
    amroutine->amrescan = ibpe_rescan;
    amroutine->amgettuple = NULL;
    amroutine->amgetbitmap = ibpe_getbitmap;
    amroutine->amendscan = ibpe_endscan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;
    amroutine->amtranslatestrategy = NULL;
    amroutine->amtranslatecmptype = NULL;

    PG_RETURN_POINTER(amroutine);
}

static void ibpe_fill_metapage(Relation indexRelation, Page metaPage)
{
    ibpe_options_data *opts = (ibpe_options_data *) indexRelation->rd_options;
    if (!opts) {
        elog(ERROR,
             "tokenizer path not set. "
             "Please specify `WITH (tokenizer_path = '<path to tokenizer.json>').`");
    }

    char const *tok_path = GET_STRING_RELOPTION(opts, tokenizer_path);
    elog(NOTICE, "Got tokenizer path = %s", tok_path);

    PageInit(metaPage, BLCKSZ, sizeof(ibpe_opaque_data));

    ibpe_opaque_data *opaque = (ibpe_opaque_data *) PageGetSpecialPointer(metaPage);
    opaque->flags = IBPE_PAGE_META;
    opaque->ibpe_page_id = IBPE_PAGE_ID;

    ibpe_metapage_data *metadata = (ibpe_metapage_data *) PageGetContents(metaPage);
    memset(metadata, 0, sizeof(ibpe_metapage_data));
    metadata->magickNumber = IBPE_MAGICK_NUMBER;
    strncpy(metadata->tokenizer_path, tok_path, TOKENIZER_PATH_MAXLEN);

    ((PageHeader) metaPage)->pd_lower += sizeof(ibpe_metapage_data);
    Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);
}

static void ibpe_init_metapage(Relation indexRelation, ForkNumber forknum)
{
    /*
	 * Make a new page; since it is first page it should be associated with
	 * block number 0.  No need to hold the extension
	 * lock because there cannot be concurrent inserters yet.
	 */
    Buffer metaBuffer = ReadBufferExtended(indexRelation, forknum, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);
    Assert(BufferGetBlockNumber(metaBuffer) == 0 /* meta page */);

    /* Initialize contents of meta page */
    GenericXLogState *state = GenericXLogStart(indexRelation);
    Page metaPage = GenericXLogRegisterBuffer(state, metaBuffer, GENERIC_XLOG_FULL_IMAGE);

    ibpe_fill_metapage(indexRelation, metaPage);

    GenericXLogFinish(state);

    UnlockReleaseBuffer(metaBuffer);
}

typedef struct
{
    tokenizer tok;
} ibpe_relcache;

static void ibpe_store_cache(Relation indexRelation, ibpe_relcache *cur_state)
{
    ibpe_relcache *state_mem;
    if (!indexRelation->rd_amcache) {
        // allocate memory for amcache
        state_mem = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));
        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }

    // store current state into rd_amcache
    *state_mem = *cur_state;
}

static void ibpe_free_relcache(void *arg)
{
    elog(NOTICE, "freeing rd_amcache");

    ibpe_relcache *state_mem = arg;

    destroy_tokenizer(state_mem->tok);
}

static ibpe_relcache ibpe_restore_or_create_cache(Relation indexRelation)
{
    ibpe_relcache *state_mem;
    if (!indexRelation->rd_amcache) {
        // allocate memory for amcache
        state_mem = MemoryContextAlloc(indexRelation->rd_indexcxt, sizeof(ibpe_relcache));

        // restore state from metapage
        Buffer buffer = ReadBuffer(indexRelation, 0 /* meta page */);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);

        Page page = BufferGetPage(buffer);

        if (((ibpe_opaque_data *) PageGetSpecialPointer(page))->ibpe_page_id != IBPE_PAGE_ID) {
            elog(ERROR, "Relation is not an ibpe index: page id does not match.");
        }

        ibpe_metapage_data *meta = (ibpe_metapage_data *) PageGetContents(page);
        if (meta->magickNumber != IBPE_MAGICK_NUMBER) {
            elog(ERROR, "Relation is not an ibpe index: invalid magick number.");
        }

        UnlockReleaseBuffer(buffer);

        // initialize tokenizer
        elog(NOTICE, "Loading tokenizer from '%s'", meta->tokenizer_path);
        char errmsg[256] = {};
        state_mem->tok = create_tokenizer(meta->tokenizer_path, errmsg, 256);
        if (!state_mem->tok) {
            elog(ERROR, "Cannot load tokenizer: %s", errmsg);
        }

        // free tokenizer before cache is freed
        MemoryContextCallback *cb = MemoryContextAlloc(indexRelation->rd_indexcxt,
                                                       sizeof(MemoryContextCallback));
        cb->func = ibpe_free_relcache;
        cb->arg = state_mem;
        MemoryContextRegisterResetCallback(indexRelation->rd_indexcxt, cb);

        indexRelation->rd_amcache = state_mem;
    } else {
        state_mem = indexRelation->rd_amcache;
    }
    return *state_mem;
}

typedef struct
{
    index_builder builder;
    tokenizer tok;

    int64 indtuples; /* total number of tuples indexed */
} ibpe_build_state;

/*
 * Per-tuple callback for table_index_build_scan.
 */
static void ibpe_build_callback(Relation indexRelation,
                                ItemPointer tid,
                                Datum *values,
                                bool *isnull,
                                bool tupleIsAlive,
                                void *state)
{
    ibpe_build_state *build_state = (ibpe_build_state *) state;

    if (isnull[0]) {
        return; // skip NULLs
    }

    char *string = TextDatumGetCString(values[0]);

    int tokens[2048] = {};
    int n_tokens = tokenizer_tokenize(build_state->tok, string, tokens, lengthof(tokens));
    if (n_tokens > lengthof(tokens)) {
        elog(ERROR, "String exceeds %zu tokens", lengthof(tokens));
    }

    if (build_state->indtuples < 5) {
        elog(NOTICE,
             "tid=%d/%d/%d, text=%s, isnull=%d, toks=[%d, %d, %d, ...]",
             tid->ip_blkid.bi_hi,
             tid->ip_blkid.bi_lo,
             tid->ip_posid,
             string,
             isnull[0],
             tokens[0],
             tokens[1],
             tokens[2]);
    }

    // int sent_id = /* TODO */;

    // index_builder_add_sentence(build_state->builder, sent_id, );

    build_state->indtuples++;
}

/* build new index */
IndexBuildResult *ibpe_build(Relation heapRelation, Relation indexRelation, IndexInfo *indexInfo)
{
    elog(NOTICE, "ibpe_build called");

    if (RelationGetNumberOfBlocks(indexRelation) != 0)
        elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(indexRelation));

    ibpe_init_metapage(indexRelation, MAIN_FORKNUM);

    ibpe_relcache cur_state = ibpe_restore_or_create_cache(indexRelation);

    ibpe_build_state build_state;
    build_state.builder = create_index_builder();
    if (!build_state.builder) {
        elog(ERROR, "Cannot allocate index builder");
    }
    build_state.tok = cur_state.tok;
    build_state.indtuples = 0;

    // scan the heap (table to be indexed)
    double reltuples = table_index_build_scan(heapRelation,
                                              indexRelation,
                                              indexInfo,
                                              true,
                                              true,
                                              ibpe_build_callback,
                                              &build_state,
                                              NULL);

    // TODO: populate index using result from builder

    destroy_index_builder(build_state.builder);

    IndexBuildResult *result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
    result->heap_tuples = reltuples;
    result->index_tuples = build_state.indtuples;

    return result;
}

/* build empty index */
void ibpe_buildempty(Relation indexRelation)
{
    // TODO
}

/* insert this tuple */
bool ibpe_insert(Relation indexRelation,
                 Datum *values,
                 bool *isnull,
                 ItemPointer heap_tid,
                 Relation heapRelation,
                 IndexUniqueCheck checkUnique,
                 bool indexUnchanged,
                 IndexInfo *indexInfo)
{
    // TODO
}

/* bulk delete */
IndexBulkDeleteResult *ibpe_bulkdelete(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats,
                                       IndexBulkDeleteCallback callback,
                                       void *callback_state)
{
    // TODO
}

/* post-VACUUM cleanup */
IndexBulkDeleteResult *ibpe_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    // TODO
}

/* estimate cost of an indexscan */
void ibpe_costestimate(struct PlannerInfo *root,
                       struct IndexPath *path,
                       double loop_count,
                       Cost *indexStartupCost,
                       Cost *indexTotalCost,
                       Selectivity *indexSelectivity,
                       double *indexCorrelation,
                       double *indexPages)
{
    // TODO
}

/* parse index reloptions */
bytea *ibpe_options(Datum reloptions, bool validate)
{
    elog(NOTICE, "ibpe_options called with validate = %d", validate);
    ibpe_options_data *rdopts = build_reloptions(reloptions,
                                                 validate,
                                                 ibpe_relopt_kind,
                                                 sizeof(ibpe_options_data),
                                                 ibpe_relopt_tab,
                                                 lengthof(ibpe_relopt_tab));

    char const *tok_path = GET_STRING_RELOPTION(rdopts, tokenizer_path);
    elog(NOTICE, "Tokenizer path = %s", tok_path);

    if (validate) {
        if (strcmp(tok_path, "") == 0) {
            elog(ERROR,
                 "tokenizer path not set. "
                 "Please specify `WITH (tokenizer_path = '<path to tokenizer.json>').`");
        }
        if (strlen(tok_path) > TOKENIZER_PATH_MAXLEN) {
            elog(ERROR, "Tokenizer path too long");
        }
    }

    return (bytea *) rdopts;
}

/* validate definition of an opclass for this AM */
bool ibpe_validate(Oid opclassoid)
{
    // TODO
}

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys)
{
    // TODO
}

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    // TODO
}

/* fetch all valid tuples */
int64 ibpe_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    // TODO
}

/* end index scan */
void ibpe_endscan(IndexScanDesc scan)
{
    // TODO
}
