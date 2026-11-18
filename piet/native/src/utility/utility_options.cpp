// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "utility_options.h"

namespace piet::handlers::utility_detail {


// `unique` and `gunique` are ONE handler, so they share ONE vocabulary. Both
// gunique-only options are refused for both spellings -- `unique, detail`
// prints a per-variable breakdown and `gunique, replace` REPLACES the data in
// memory with the distinct rows, and either accepted-and-ignored would answer a
// different question at rc 0.
const piet::eval::OptVocab& kVocabUnique = piet::eval::registerOptVocab(
    {"unique", "gunique"}, {{"by", "gen", "generate", "missing"}, {"detail", "replace"}});

// `abbrev()` shortens the displayed names, `min()`/`max()` filter the rows this
// build prints in full. None is read; each changes what the table SAYS.
const piet::eval::OptVocab& kVocabDistinct = piet::eval::registerOptVocab(
    {"distinct"}, {{"joint", "missing"}, {"abbrev", "min", "max"}});

// `force` IS read: it is missings' own safety catch (see runMissings), not a
// no-op. `generate()` names the column `missings tag` creates.
const piet::eval::OptVocab& kVocabMissings = piet::eval::registerOptVocab(
    {"missings"},
    {{"generate", "force"}, {"minimum", "observations", "percent", "sort"}});

// `any`/`all`/`none` select WHICH variables mdesc reports; `abbreviate()`
// shortens the names. All four would change the table.
const piet::eval::OptVocab& kVocabMdesc = piet::eval::registerOptVocab(
    {"mdesc"}, {{}, {"abbreviate", "any", "all", "none"}});

// `decode` takes the text from the value label of the values variable.
const piet::eval::OptVocab& kVocabLabmask = piet::eval::registerOptVocab(
    {"labmask"}, {{"values", "lblname", "decode"}, {}});

// ---- AER scope 2, bundle B2 (2026-09-07) ----------------------------------
// strdist's own `syntax` line is
//   syntax anything [if] [in], [Generate(name) MAXdist(integer -1)]
// and both options are READ here. There is nothing to put in `unimplemented`:
// the package has exactly these two.
const piet::eval::OptVocab& kVocabStrdist = piet::eval::registerOptVocab(
    {"strdist"}, {{"generate", "maxdist"}, {}});

// `nobreak` suppresses the chopping this command exists to do; `recode()`
// renumbers the codes r(relabel) carries.
const piet::eval::OptVocab& kVocabSplitvallabels = piet::eval::registerOptVocab(
    {"splitvallabels"}, {{"length", "nobreak", "recode", "vals", "local"}, {}});

// `strok` tells marksample to KEEP observations whose string variables are
// non-empty rather than marking them all out, and `zeroweight` keeps
// zero-weight observations. Both change which rows the marker selects, and
// runMarksample (sample.cpp) implements both ([P] mark rules 1 and 6).
const piet::eval::OptVocab& kVocabMarksample = piet::eval::registerOptVocab(
    {"marksample"}, {{"novarlist", "strok", "zeroweight"}, {}});

// `save(file)` writes the file list OUT to a .dta (with `replace` over an existing
// file) and leaves the data in memory as they were; `list` and `maxdeep()` are
// not implemented.
const piet::eval::OptVocab& kVocabFilelist = piet::eval::registerOptVocab(
    {"filelist"}, {{"directory", "pattern", "norecursive", "recursive", "save", "replace"},
                   {"list", "maxdeep"}});

// `by` arrives from the dispatcher's by-prefix branch as an ordinary option.
// `carryalong()` drags other variables along with each filled value,
// `dynamic_condition()` makes the fill conditional, `cfindic()` records which
// values were filled, `strict` errors instead of filling -- each changes what
// lands in the data.
const piet::eval::OptVocab& kVocabCarryforward = piet::eval::registerOptVocab(
    {"carryforward"}, {{"gen", "generate", "replace", "by"},
                       {"carryalong", "strict", "dynamic_condition", "cfindic"}});

// `excludeself` drops the row itself from its own window and `casewise` drops
// rows missing on ANY of the requested variables -- both change which
// observations enter every statistic.
const piet::eval::OptVocab& kVocabRangestat = piet::eval::registerOptVocab(
    {"rangestat"}, {{"interval", "by", "excludeself", "casewise", "describe", "local", "nocheck"},
                    {},
                    {{"d", "describe"}}});

// `fcond()` and `pcond()` set where a spell BEGINS and where its run continues; each is
// read as tsspell.ado reads it. `replace` drops existing _spell/_seq/_end first.
const piet::eval::OptVocab& kVocabTsspell = piet::eval::registerOptVocab(
    {"tsspell"}, {{"cond", "fcond", "pcond", "spell", "seq", "end", "by", "replace"}, {}});



const eval::OptVocab& kVocabBcal = eval::registerOptVocab(
    {"bcal"}, {{"from", "generate", "personal", "excludemissing", "replace", "purpose",
                "dateformat", "range", "centerdate", "maxgap", "rc0"},
               {},
               // Minimal abbreviations: g/gen for generate().
               {{"g", "generate"}, {"gen", "generate"}}});


}
