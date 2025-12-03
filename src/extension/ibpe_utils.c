#include "ibpe_utils.h"
#include "ibpe_build.h"

#include <string.h>

#include <access/generic_xlog.h>
#include <access/reloptions.h>
#include <access/tableam.h>
#include <commands/vacuum.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
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

ibpe_opaque_data *ibpe_get_opaque(Page page)
{
    return (ibpe_opaque_data *) PageGetSpecialPointer(page);
}

bool ibpe_is_page_deleted(Page page)
{
    return (ibpe_get_opaque(page)->flags & IBPE_PAGE_DELETED) != 0;
}

int ibpe_page_get_free_space(Page page)
{
    int space = BLCKSZ;
    space -= MAXALIGN(SizeOfPageHeaderData);
    space -= ibpe_get_opaque(page)->data_len;
    space -= MAXALIGN(sizeof(ibpe_opaque_data));
    return space;
}

/* bulk delete */
IndexBulkDeleteResult *ibpe_bulkdelete(IndexVacuumInfo *info,
                                       IndexBulkDeleteResult *stats,
                                       IndexBulkDeleteCallback callback,
                                       void *callback_state)
{
    // TODO
    elog(ERROR, "ibpe_bulkdelete: Not implemented");
}

/* post-VACUUM cleanup */
IndexBulkDeleteResult *ibpe_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    // TODO
    elog(ERROR, "ibpe_vacuumcleanup: Not implemented");
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
    elog(ERROR, "ibpe_costestimate: Not implemented");
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
    elog(ERROR, "ibpe_validate: Not implemented");
}

/* prepare for index scan */
IndexScanDesc ibpe_beginscan(Relation indexRelation, int nkeys, int norderbys)
{
    // TODO
    elog(ERROR, "ibpe_beginscan: Not implemented");
}

/* (re)start index scan */
void ibpe_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    // TODO
    elog(ERROR, "ibpe_rescan: Not implemented");
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
    // TODO
    elog(ERROR, "ibpe_endscan: Not implemented");
}
