// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_io_internal.h"
namespace piet::handlers::data_io_detail {



// R2: re-binding of the option vocabularies this TU consumes. Registering the
// same token twice UNIONS the sets (documented, deliberate — errors.h), and a
// set unioned with itself is itself, so the single-registry law (BUG-233)
// holds with three TUs exactly as it did with one.
const piet::eval::OptVocab& kVocabTsfill = piet::eval::registerOptVocab(
    {"tsfill"}, {{"full"},
     {}});
}
