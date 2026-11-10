// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_combine_internal.h"
namespace piet::handlers::data_combine_detail {



// R2: re-binding of the option vocabularies this TU consumes. Registering the
// same token twice UNIONS the sets (documented, deliberate — errors.h), and a
// set unioned with itself is itself, so the single-registry law (BUG-233)
// holds with three TUs exactly as it did with one.
// reshape.ado's syntax line: I() J() ATwl() String favor(); `string` is read
// by runReshape (s/str/string, and j(t, string)).
const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j", "string"},
     {"atwl", "favor"}});

// B4-W3b: `generate`, `update` and `replace` graduate from UNIMPLEMENTED to
// KNOWN — the engine grew all three on main 3a5d80e2 ("merge --update and the
// egen riders, against an oracle captured first") and this is the PIET side of
// that landing. The rest stay unimplemented and keep saying so.
// merge's syntax line: force, sorted, noreport and nonotes are read by
// runMerge (nonotes: merge appends no using notes in this build either way).
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate", "keep", "assert", "generate", "update", "replace",
                 "force", "sorted", "noreport", "nonotes", "nolabel"},
     {}});
}
