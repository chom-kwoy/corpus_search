#include "pg_extension.h"

#include <string.h>

#include <access/generic_xlog.h>
#include <access/reloptions.h>
#include <access/tableam.h>
#include <commands/vacuum.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/rel.h>

PG_MODULE_MAGIC_EXT(.name = "ibpe", .version = PG_VERSION);

static relopt_kind ibpe_relopt_kind;
static relopt_parse_elt ibpe_relopt_tab[1];

typedef struct
{
    int32 vl_len_;      // varlena header
    int tokenizer_path; // string option
} ibpe_options_st;

typedef struct
{
} ibpe_opaque_data;

typedef struct
{
    uint32 magickNumber;
    ibpe_options_st opts;
} ibpe_metapage_data;

#define IBPE_MAGICK_NUMBER (0xFEEDBEEF)

void _PG_init(void)
{
    elog(NOTICE, "_PG_init called");

    ibpe_relopt_kind = add_reloption_kind();

    // path to tokenizer
    add_string_reloption(ibpe_relopt_kind,
                         "tokenizer_path",
                         "Path to tokenizer.json",
                         "NONE", // default value
                         NULL,
                         AccessExclusiveLock);
    ibpe_relopt_tab[0].optname = "tokenizer_path";
    ibpe_relopt_tab[0].opttype = RELOPT_TYPE_STRING;
    ibpe_relopt_tab[0].offset = offsetof(ibpe_options_st, tokenizer_path);
}

PG_FUNCTION_INFO_V1(ibpe_handler);
Datum ibpe_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    // Total number of strategies (operators) by which we can traverse/search this AM.
    amroutine->amstrategies = 1;

    // total number of support functions that this AM uses
    amroutine->amsupport = 0; // FIXME?

    // opclass options support function number or 0
    amroutine->amoptsprocnum = 0; // FIXME?

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

static ibpe_options_st *make_default_ibpe_options(void)
{
    ibpe_options_st *opts = (ibpe_options_st *) palloc0(sizeof(ibpe_options_st));

    // TODO: set default tokenizer_path

    SET_VARSIZE(opts, sizeof(ibpe_options_st));
    return opts;
}

static void ibpe_fill_metapage(Relation indexRelation, Page metaPage)
{
    ibpe_options_st *opts = (ibpe_options_st *) indexRelation->rd_options;
    if (!opts) {
        opts = make_default_ibpe_options();
    }

    PageInit(metaPage, BLCKSZ, sizeof(ibpe_opaque_data));

    ibpe_opaque_data *opaque = (ibpe_opaque_data *) PageGetSpecialPointer(metaPage);
    // TODO: set things in opaque

    ibpe_metapage_data *metadata = (ibpe_metapage_data *) PageGetContents(metaPage);
    memset(metadata, 0, sizeof(ibpe_metapage_data));
    metadata->magickNumber = IBPE_MAGICK_NUMBER;
    metadata->opts = *opts;

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
    Assert(BufferGetBlockNumber(metaBuffer) == 0);

    /* Initialize contents of meta page */
    GenericXLogState *state = GenericXLogStart(indexRelation);
    Page metaPage = GenericXLogRegisterBuffer(state, metaBuffer, GENERIC_XLOG_FULL_IMAGE);

    ibpe_fill_metapage(indexRelation, metaPage);

    GenericXLogFinish(state);

    UnlockReleaseBuffer(metaBuffer);
}

typedef struct
{
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

    char *string = TextDatumGetCString(values[0]);
    elog(NOTICE,
         "tid=%d/%d/%d, text=%s",
         tid->ip_blkid.bi_hi,
         tid->ip_blkid.bi_lo,
         tid->ip_posid,
         string);
    // TODO
}

/* build new index */
IndexBuildResult *ibpe_build(Relation heapRelation,
                             Relation indexRelation,
                             struct IndexInfo *indexInfo)
{
    elog(NOTICE, "ibpe_build called");

    if (RelationGetNumberOfBlocks(indexRelation) != 0)
        elog(ERROR, "index \"%s\" already contains data", RelationGetRelationName(indexRelation));

    ibpe_init_metapage(indexRelation, MAIN_FORKNUM);

    ibpe_build_state build_state;
    int ncols = indexRelation->rd_att->natts;

    elog(NOTICE, "ncols = %d", ncols);

    // scan the heap (table to be indexed)
    double reltuples = table_index_build_scan(heapRelation,
                                              indexRelation,
                                              indexInfo,
                                              true,
                                              true,
                                              ibpe_build_callback,
                                              &build_state,
                                              NULL);

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
                 struct IndexInfo *indexInfo)
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
    ibpe_options_st *rdopts = build_reloptions(reloptions,
                                               validate,
                                               ibpe_relopt_kind,
                                               sizeof(ibpe_options_st),
                                               ibpe_relopt_tab,
                                               lengthof(ibpe_relopt_tab));
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
