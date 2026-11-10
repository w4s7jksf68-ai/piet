// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "options.h"

namespace piet::handlers::utility_detail {


// `unique` and `gunique` are ONE handler, so they share ONE vocabulary. Both
// gunique-only options are refused for both spellings -- `unique, detail`
// prints a per-variable breakdown and `gunique, replace` REPLACES the data in
// memory with the distinct rows, and either accepted-and-ignored would answer a
// different question at rc 0.
const piet::eval::OptVocab& kVocabUnique = piet::eval::registerOptVocab(
    {"unique", "gunique"}, {{"by", "gen", "generate"}, {"detail", "replace"}});

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

// `decode` runs labmask in reverse (label -> a new string variable), which is a
// different command's output, not an option on this one's.
const piet::eval::OptVocab& kVocabLabmask = piet::eval::registerOptVocab(
    {"labmask"}, {{"values", "lblname"}, {"decode"}});

// This build's renvarlab is the NAME-transform half: prefix/suffix/case. The
// package's label-driven renaming (rename each variable to a token of its
// variable LABEL) is a different derivation of the new names and is refused
// rather than approximated.
const piet::eval::OptVocab& kVocabRenvarlab = piet::eval::registerOptVocab(
    {"renvarlab"}, {{"prefix", "suffix", "upper", "lower"},
                    {"label", "postfix", "subst", "presub", "postsub", "trim", "map", "symbol",
                     "display", "test"}});

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
    {"splitvallabels"}, {{"length"}, {"nobreak", "recode"}});

// `strok` tells marksample to KEEP observations whose string variables are
// non-empty rather than marking them out, and `zeroweight` keeps zero-weight
// observations. Both change which rows the marker selects, so neither may be
// accepted-and-ignored -- the plan's scope table calls them "accept-ignore",
// which is precisely the shape defect class 3 forbids.
const piet::eval::OptVocab& kVocabMarksample = piet::eval::registerOptVocab(
    {"marksample"}, {{"novarlist"}, {"strok", "zeroweight"}});

// `save()`/`replace` write the file list OUT to a .dta; this build's filelist
// only builds it in memory.
const piet::eval::OptVocab& kVocabFilelist = piet::eval::registerOptVocab(
    {"filelist"}, {{"directory", "pattern", "norecursive"}, {"save", "replace"}});

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
    {"rangestat"}, {{"interval", "by"}, {"excludeself", "casewise"}});

// `fcond()`/`pcond()` are tsspell's forward/previous-observation conditions:
// they change where a spell BEGINS, which is the one thing this command decides.
const piet::eval::OptVocab& kVocabTsspell = piet::eval::registerOptVocab(
    {"tsspell"}, {{"cond", "spell", "seq", "end", "by"}, {"fcond", "pcond"}});



const eval::OptVocab& kVocabBcal = eval::registerOptVocab(
    {"bcal"}, {{"from", "generate", "personal", "excludemissing", "replace", "purpose",
                "dateformat", "range", "centerdate", "maxgap", "rc0"},
               {},
               // Minimal abbreviations: g/gen for generate().
               {{"g", "generate"}, {"gen", "generate"}}});


}
