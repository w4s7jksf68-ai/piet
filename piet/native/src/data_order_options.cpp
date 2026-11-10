// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_mutation_internal.h"
namespace piet::handlers::data_mutation_detail {

const piet::eval::OptVocab& kVocabOrder = piet::eval::registerOptVocab(
    {"order"}, {{"first", "last", "before",
                                                                   "after", "alphabetic"},
     {"sequential"}});

const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j"},
     {}});

// 2026-09-10: `generate`, `update` and `replace` REMOVED from this row's unimplemented set.
// They are implemented — `handlers_data_verbs2.cpp`'s kVocabMerge (the one `runMerge` actually
// reads) carries all three in `known`, and this build answers Stata's cells exactly:
// 74 non-missing after update, `_merge==4` on the 5 filled rows, 69 matched, `mflag` created and
// `_merge` absent (tests/eval/golden/aer_b4/b5_riders_pins.log).
//
// This registration is DEAD CODE — there is no runMerge in this translation unit — but
// `registerOptVocab` UNIONS same-named registrations across translation units, so its
// unimplemented set repolluted the merged vocabulary even though `known` wins at runtime. The
// live surface was therefore correct while the CENSUS, which reads the vocabulary and not the
// code, counted all 83 corpus uses of `merge, update`/`merge, gen()` as gaps. A vocabulary row is
// not documentation: something reads it, and here that something was the instrument measuring us.
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate",
                                                                   "keep", "assert"},
     {"nolabel", "nonotes", "noreport", "force", "sorted"}});
}
