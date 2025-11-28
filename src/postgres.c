#include <postgres.h>
// The include order is important
#include <fmgr.h>

PG_MODULE_MAGIC_EXT(.name = "ibpe", .version = PG_VERSION);
