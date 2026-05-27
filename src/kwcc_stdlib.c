/* kwcc_stdlib.c — custom mquickjs stdlib for kwcc
 *
 * Patterned after example_stdlib.c: defines CONFIG_KWCC,
 * then #includes mqjs_stdlib.c to generate the full stdlib header.
 *
 * Used ONLY by the host build tool (_build_stdlib), not linked
 * into the final binary. deps/mquickjs/mqjs_stdlib.c is only
 * minimally modified (one #ifdef CONFIG_KWCC block).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mquickjs_build.h"

/* kwcc_ui is registered in CONFIG_KWCC block of mqjs_stdlib.c */

/* ── include the full standard library ───────────────────────── */
#define CONFIG_KWCC
#include "mqjs_stdlib.c"
