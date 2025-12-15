#include "ibpe_utils.h"
#include "ibpe_backend.h"
#include "ibpe_build.h"
#include "ibpe_scan.h"
#include "ibpe_vacuum.h"

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
#include <utils/selfuncs.h>

PG_MODULE_MAGIC;

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
#if PG_MAJORVERSION_NUM >= 18
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = false;
    amroutine->amconsistentordering = false;
#endif
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
#if PG_MAJORVERSION_NUM >= 18
    amroutine->amgettreeheight = NULL;
    amroutine->amtranslatestrategy = NULL;
    amroutine->amtranslatecmptype = NULL;
#endif
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

    PG_RETURN_POINTER(amroutine);
}

// page related utils
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

// index options
static relopt_kind ibpe_relopt_kind;
static relopt_parse_elt ibpe_relopt_tab[2];

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

    // normalize mappings
    add_string_reloption(ibpe_relopt_kind,
                         "normalize_mappings",
                         "Normalize mappings in JSON format",
                         "{}",
                         NULL,
                         AccessExclusiveLock);
    ibpe_relopt_tab[1].optname = "normalize_mappings";
    ibpe_relopt_tab[1].opttype = RELOPT_TYPE_STRING;
    ibpe_relopt_tab[1].offset = offsetof(ibpe_options_data, normalize_mappings);
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
    char const *mappings = GET_STRING_RELOPTION(rdopts, normalize_mappings);
    elog(NOTICE, "Tokenizer path = %s, normalize mappings = %s", tok_path, mappings);

    if (validate) {
        if (strcmp(tok_path, "") == 0) {
            elog(ERROR,
                 "tokenizer path not set. "
                 "Please specify `WITH (tokenizer_path = '<path to tokenizer.json>').`");
        }
        if (strlen(tok_path) > TOKENIZER_PATH_MAXLEN) {
            elog(ERROR, "Tokenizer path too long");
        }
        if (parse_normalize_mappings(mappings, NULL, 0) < 0) {
            elog(ERROR, "Malformed JSON in normalize_mappings");
        }
    }

    return (bytea *) rdopts;
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
    IndexOptInfo *index = path->indexinfo;
    GenericCosts costs = {0};

    /* We have to visit all index tuples anyway */
    costs.numIndexTuples = index->tuples;

    elog(NOTICE, "ibpe_costestimate called with tuples=%lf", index->tuples);

    /* Use generic estimate */
    genericcostestimate(root, path, loop_count, &costs);

    costs.indexTotalCost = 0.1;
    costs.indexSelectivity = 0.01;

    elog(NOTICE,
         "indexStartupCost=%lf, indexTotalCost=%lf, indexSelectivity=%lf, "
         "indexCorrelation=%lf, numIndexPages=%lf",
         costs.indexStartupCost,
         costs.indexTotalCost,
         costs.indexSelectivity,
         costs.indexCorrelation,
         costs.numIndexPages);

    *indexStartupCost = costs.indexStartupCost;
    *indexTotalCost = costs.indexTotalCost;
    *indexSelectivity = costs.indexSelectivity;
    *indexCorrelation = costs.indexCorrelation;
    *indexPages = costs.numIndexPages;
}

/* validate definition of an opclass for this AM */
bool ibpe_validate(Oid opclassoid)
{
    elog(WARNING, "ibpe_validate: Not implemented. always returning true..");
    return true;
}
