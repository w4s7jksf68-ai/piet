// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vendor/nlohmann/json.hpp"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_options.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is the plan's "corpus options (implemented)" column verbatim;
// `unimplemented` is its "refused options" column. The split is the whole
// point: a name in NEITHER set reads as "unknown option", which is the wrong
// diagnosis for a real Stata option this build simply does not do, and a name
// wrongly placed in `known` is accepted and silently ignored, which is the
// defect the apparatus exists to prevent.
namespace piet::handlers::dataset_shape_detail {

// `p()` is split's own abbreviation for `parse()`; both are exact members, so
// the prefix resolver never has to choose between them.
const piet::eval::OptVocab& kVocabSplit = piet::eval::registerOptVocab(
    {"split"}, {{"parse", "p", "gen", "generate", "limit", "destring",
                 // split.ado: `notrim` keeps the padding; force, float, ignore()
                 // and percent are destring's, handed to it (each needs destring).
                 "notrim", "ignore", "force", "float", "percent"},
                // `g` is split.ado's Generate() minimum (aer_w2 w2b_dataio
                // split_p_g `p("-") g(Job)` rc 0); it prefixes both
                // registered spellings, so it is declared.
                {},
                {{"g", "generate"}, {"ge", "generate"}}});

// `promote` asks for the narrowest storage type that holds the transposed
// values. It IS read below (it produces a note), because this build's frame is
// f64 throughout and there is nothing to promote -- saying so is honest, where
// silently accepting it would claim a storage decision that never happened.
// `format` / `format(%fmt)` (xpose.ado `Format(string) Format1`, abbreviated to
// `f`) set the display format of every new variable: format() the one given,
// bare `format` the widest numeric format among the source variables.
const piet::eval::OptVocab& kVocabXpose = piet::eval::registerOptVocab(
    {"xpose"}, {{"clear", "varname", "promote", "format"}, {}, {{"f", "format"}}});

const piet::eval::OptVocab& kVocabRecast =
    piet::eval::registerOptVocab({"recast"}, {{"force"}, {}});

const piet::eval::OptVocab& kVocabInsobs =
    piet::eval::registerOptVocab({"insobs"}, {{"before", "after"}, {}});

const piet::eval::OptVocab& kVocabStack = piet::eval::registerOptVocab(
    {"stack"}, {{"into", "clear", "wide", "group"}, {}});

const piet::eval::OptVocab& kVocabSeparate = piet::eval::registerOptVocab(
    {"separate"}, {{"by", "shortlabel", "veryshortlabel", "sequential", "missing", "indicator",
                    "generate"},
                   {},
                   {{"g", "generate"}, {"i", "indicator"}}});

const piet::eval::OptVocab& kVocabMvencode =
    piet::eval::registerOptVocab({"mvencode"}, {{"mv", "override"}, {}});

const piet::eval::OptVocab& kVocabSample =
    piet::eval::registerOptVocab({"sample"}, {{"count", "by"}, {}});

const piet::eval::OptVocab& kVocabSplitsample = piet::eval::registerOptVocab(
    {"splitsample"}, {{"generate", "nsplit", "rseed", "show", "split", "values", "cluster",
                       "strok", "percent"},
                      {"strata", "balance", "rround"},
                      {{"value", "values"}, {"cl", "cluster"}}});

// `double` asks for double storage, which is what this build's frame already
// is, so it is honoured by construction rather than ignored. `cstorage()`
// declares an alternative corr()/cov() matrix layout and `forcepsd` silently
// repairs a non-PD matrix -- both would change the numbers drawn.
const piet::eval::OptVocab& kVocabDrawnorm = piet::eval::registerOptVocab(
    {"drawnorm"}, {{"n", "means", "sds", "corr", "cov", "seed", "double", "clear", "cstorage",
                    "forcepsd"},
                   {}});

// `automatic` and `label` build value labels from the text met in a numeric
// variable. The labelling is not implemented: runInput refuses a line that
// would need it by name, and reads every other line as it would without them.
const piet::eval::OptVocab& kVocabInput =
    piet::eval::registerOptVocab({"input"}, {{"automatic", "label"}, {}});

// `notes` has no option tail at all -- everything after the colon is NOTE TEXT,
// including any comma in it, so this handler reads `parsed.raw` and never
// `parsed.opts`, and never runs the reject net (it would reject the note's own
// words). The registration exists so the vocabulary inventory is complete and
// `canonicalizeForCommand` has an answer for the token.
const piet::eval::OptVocab& kVocabNotes =
    piet::eval::registerOptVocab({"notes", "note"}, {{}, {}});

const piet::eval::OptVocab& kVocabCompare =
    piet::eval::registerOptVocab({"compare"}, {{}, {}});

const piet::eval::OptVocab& kVocabCumul = piet::eval::registerOptVocab(
    {"cumul"}, {{"gen", "generate", "equal", "freq", "by"},
                {},
                {{"g", "generate"}, {"ge", "generate"}, {"f", "freq"}}});

const piet::eval::OptVocab& kVocabBsample = piet::eval::registerOptVocab(
    {"bsample"}, {{"cluster", "idcluster", "strata", "weight"}, {}});

// `altdef` selects Stata's ALTERNATIVE percentile definition, the interpolating
// rule of [D] pctile "Methods and formulas" (pctile.cpp stataAltdefPercentile).
// pctile.ado's syntax line has no percentiles(): `pctile pc = x, p(10 50)` is
// "option p() not allowed" r(198) (StataBE 19.5); only _pctile takes it.
const piet::eval::OptVocab& kVocabPctile = piet::eval::registerOptVocab(
    {"pctile"}, {{"nquantiles", "nq", "genp", "altdef"}, {},
                 // pctile's syntax: Nquantiles(), ALTdef, Genp() (bank afd2f5ddbc6615f9 `g(pp)`).
                 {{"n", "nquantiles"}, {"g", "genp"}}});

const piet::eval::OptVocab& kVocabPctileR = piet::eval::registerOptVocab(
    {"_pctile"}, {{"nquantiles", "nq", "percentiles", "p", "altdef"}, {}});

namespace {

// The registrations above are consumed by the reject nets below; naming the two
// that no net reads keeps -Wunused-variable honest about them (`notes` and
// `compare` have no option tail to reject).
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {&kVocabNotes, &kVocabCompare};

}  // namespace

}  // namespace piet::handlers::dataset_shape_detail
