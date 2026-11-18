// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/column_fetch.h"
#include "piet/translate.h"
// Copyright (C) 2026 M.K. Kim
// Levels command.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/display_hardening.h"  // formatNumberAsDisplayed: levelsof's %21.0g
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "engine/command_support.h"
#include "estimation/estimation_options.h"
#include "piet/handlers/descriptive.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

namespace command_detail {

// levelsof.py's numeric _fmt: integer-valued -> plain integer string, else
// Python's str(float) shortest round-trip repr.
std::string levelsofFmtNum(double v) {
  if (std::isfinite(v) && v == static_cast<double>(static_cast<long long>(v))) {
    return std::to_string(static_cast<long long>(v));
  }
  return eval::pythonFloatRepr(v);
}

}  // namespace

ojson runLevelsof(const std::string& varname, const std::optional<std::string>& localName,
                   bool clean, const std::optional<std::string>& ifExpr, SessionState& session) {
  // ONESHOT-GETCOLS2: `levelsof` reads ONE column's values (plus whatever an `if`
  // names). Materializing the head for that was 14 of the flushes left after the
  // mutation entrance moved. `varname` is passed as alsoInclude because it is the
  // subject, not an expression reference.
  std::string lvlMiniPath;
  std::unique_ptr<Datastore> lvlMini;
  if (auto cols = referencedColumns(session, {ifExpr.has_value() ? &*ifExpr : nullptr}, varname))
    lvlMini = fetchColumns(session, *cols, session.workdir(), &lvlMiniPath);
  if (lvlMini && !lvlMini->has(varname)) lvlMini.reset();  // subject absent → not usable
  Datastore* ds = lvlMini ? lvlMini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  // levelsof.py's run_levelsof(ds, varname, clean) operates on `target_ds` —
  // the CURRENT dataset, or a row-filtered temp copy when `if COND` is
  // present (dispatcher.py's _run_levelsof: strmutate.write_filtered_hbin,
  // keep=True). M3a numeric-only slice: string/factor `if` conditions are
  // OUT of scope (strmutate's row-wise evaluator is not ported this
  // milestone) — a pure-numeric if routes through cobra eval --keepif.
  Datastore* targetDs = ds;
  std::unique_ptr<Datastore> filteredHolder;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    std::string outPath = nextPath(session.workdir(), "levelsof");
    // BUG-381: this filtered copy is READ and abandoned — it never becomes the
    // head, so no supersede ever names it and reclaimSupersededScratch cannot
    // reach it however it is scheduled. That is what the §38 projection ledger
    // is for: register it and the outermost dispatch boundary drains it.
    session.noteProjectionScratch(outPath);
    // ONESHOT-LASTFLUSH: the earlier wave filtered the REAL head here, reasoning
    // that a projection "carries only what we fetched". It carries exactly what
    // this filter needs: the `if` expression's own columns (that is what the fetch
    // was built from) plus `varname`, which is the only column read back below. So
    // filter the projection and leave the head virtual — this was the last 14 of
    // the flushes, and the fetch above was pure overhead on this branch before.
    //
    // The fallback is not decoration. `referencedColumns` leaves identifiers the
    // shadow card does not confirm alone, so a card that is missing a real column
    // yields a projection the filter cannot evaluate. That is an ENGINE error, not
    // a distinguishable one, so it is caught and retried against the head rather
    // than reported — a miss must cost a fallback, never an answer.
    const std::string lvlHead = session.currentHbin().value_or(ds->path());
    bool filtered = false;
    if (lvlMini) {
      std::vector<std::string> tryArgs = translate::injectTsContextForExpr(
          {"eval", "--data", lvlMiniPath, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(tryArgs);
        filtered = true;
      } catch (const CobraError&) {
        cecon::native_unlink(outPath);  // never leave a torn file for the retry to open
      }
    }
    if (!filtered) {
      std::vector<std::string> args = translate::injectTsContextForExpr(
          {"eval", "--data", lvlHead, "--keepif", *ifExpr, "--out", outPath},
          *ifExpr, session);  // BUG-369 follow-up
      try {
        runCobraJson(args);
      } catch (const CobraError& ce) {
        return engineErrorBlock("levelsof: " + cobraErrorStr(ce), ce.code);
      }
    }
    filteredHolder = std::make_unique<Datastore>(outPath);
    targetDs = filteredHolder.get();
  }

  if (!targetDs->has(varname)) {
    // Python: `raise KeyError(f"variable '{varname}' not found in dataset")`,
    // caught as `except KeyError as e: return make_engine_error(str(e))`.
    // KeyError.__str__() wraps the message in an EXTRA layer of double
    // quotes (Python's repr-of-a-single-string-arg convention) — reproduce
    // that exact double-quoting, not just the inner message.
    return engineErrorBlock("\"variable '" + varname + "' not found in dataset\"");
  }

  std::vector<std::string> levelStrings;
  if (targetDs->dtype(varname) == "f64") {
    const double* d = targetDs->col(varname).f64();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(double));
    std::vector<double> uniq;
    for (int64_t i = 0; i < count; ++i) {
      if (!std::isnan(d[i])) uniq.push_back(d[i]);
    }
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    for (double v : uniq) levelStrings.push_back(levelsofFmtNum(v));
    // clean has NO effect on numeric columns (Python only branches on
    // `clean` inside the `else` / factor-dtype arm) — level_strings above
    // is final for f64 regardless of `clean`.
  } else {
    const int32_t* codes = targetDs->col(varname).codes();
    int64_t count = targetDs->col(varname).nbytes / static_cast<int64_t>(sizeof(int32_t));
    std::vector<int32_t> uniqCodes;
    for (int64_t i = 0; i < count; ++i) {
      if (codes[i] >= 0) uniqCodes.push_back(codes[i]);
    }
    std::sort(uniqCodes.begin(), uniqCodes.end());
    uniqCodes.erase(std::unique(uniqCodes.begin(), uniqCodes.end()), uniqCodes.end());
    const auto& levelsList = targetDs->varinfo(varname).levels;
    std::vector<std::string> rawLabels;
    for (int32_t c : uniqCodes) {
      rawLabels.push_back((c >= 0 && static_cast<size_t>(c) < levelsList.size())
                              ? levelsList[static_cast<size_t>(c)]
                              : std::to_string(c));
    }
    std::sort(rawLabels.begin(), rawLabels.end());
    for (const auto& lbl : rawLabels) {
      levelStrings.push_back(clean ? lbl : ("\"" + lbl + "\""));
    }
  }

  std::string joined;
  for (size_t i = 0; i < levelStrings.size(); ++i) {
    if (i) joined += " ";
    joined += levelStrings[i];
  }

  ojson out;
  out["type"] = "levelsof";
  out["varname"] = varname;
  ojson levelsArr = ojson::array();
  for (const auto& s : levelStrings) levelsArr.push_back(s);
  out["levels"] = levelsArr;
  out["text"] = joined;

  // `if local_name:` — ONLY set when local() was explicitly given (no
  // default local-var name).
  if (localName.has_value() && !localName->empty()) {
    session.eval.locals.set(*localName, joined);
  }
  return out;
}

// ---------------------------------------------------------------------------
// levelsof / glevelsof: the counts and the level text each command's own ado
// publishes (vs-stata bank 2026-11-10).
//
// levelsof (base/l/levelsof.ado, levelsof_wrk): the sample is the if/in rows;
// without `missing` a row whose value is missing (`.`-`.z`, or "" for a string)
// is dropped, and r(N) counts the rows KEPT (auto rep78: 69 of 74). r(r) is the
// number of distinct levels; missing values of different kinds are different
// levels. No row left: r(N) = r(r) = 0 and no r(levels). Numeric levels are
// %21.0g when every level is a whole number, else the text `local x = value`
// stores; string levels are compound-quoted unless `clean`.
//
// glevelsof (gtools' glevelsof.ado + its plugin): the same row rule over a
// varlist (a row with ANY missing value is dropped), published as r(N), r(J),
// r(minJ), r(maxJ) (the smallest and largest level's row count), r(levels),
// r(sep), r(colsep). A numeric level is printed %.16g, a string one `"..."'
// unless `clean`; with several variables each level is its columns joined by
// colseparate() (default " | ") and compound-quoted. For one numeric variable
// the ado then turns " 0." into " ." and "-0." into "-.".
// ---------------------------------------------------------------------------

}  // namespace piet::handlers

namespace piet::eval {
std::string stataMacroRealText(double d);  // macro_expressions.cpp: `local x = value`
}

namespace piet::handlers {
using namespace command_detail;

namespace {

ojson levelsRcError(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

struct LevelCell {
  bool isStr = false;
  double d = 0.0;
  std::string s;
};

bool levelCellMissing(const LevelCell& c) {
  return c.isStr ? c.s.empty() : std::isnan(c.d);
}

int levelCellCmp(const LevelCell& a, const LevelCell& b) {
  if (a.isStr) return a.s < b.s ? -1 : (b.s < a.s ? 1 : 0);
  return cecon::stata_cmp(a.d, b.d);
}

struct LevelKeyLess {
  bool operator()(const std::vector<LevelCell>& a, const std::vector<LevelCell>& b) const {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const int c = levelCellCmp(a[i], b[i]);
      if (c != 0) return c < 0;
    }
    return false;
  }
};

std::string levelMissingText(double v) {
  const int k = cecon::missing_kind(v);
  return k >= 1 && k <= 26 ? std::string(".") + static_cast<char>('a' + k - 1) : std::string(".");
}

std::string trimSpaces(const std::string& s) {
  const size_t b = s.find_first_not_of(' ');
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(' ');
  return s.substr(b, e - b + 1);
}

// levelsof's integer spelling: strofreal(v, "%21.0g").
std::string levelsofIntText(double v) {
  if (std::isnan(v)) return levelMissingText(v);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return trimSpaces(formatNumberAsDisplayed("%21.0g", buf));
}

// gtools' default numfmt, %.16g.
std::string glevelsofNumText(double v) {
  if (std::isnan(v)) return levelMissingText(v);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.16g", v == 0.0 ? 0.0 : v);
  return buf;
}

std::string compoundQuote(const std::string& s) { return "`\"" + s + "\"'"; }

std::string replaceAllOf(std::string s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

}  // namespace

ojson runLevelsofCommand(const LevelsofRequest& req, SessionState& session) {
  const std::string cmdName = req.gtools ? "glevelsof" : "levelsof";
  if (req.vars.empty()) return levelsRcError("varlist required", 100);
  if (!req.gtools && req.vars.size() > 1) return levelsRcError("too many variables specified", 103);

  // The projection first (one fetch of the columns read), the head as fallback:
  // the same shape the single-variable form has used since ONESHOT-GETCOLS2.
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  {
    const std::string* ifp = req.ifExpr.has_value() ? &*req.ifExpr : nullptr;
    if (auto cols = referencedColumns(session, {ifp}, req.vars[0])) {
      std::vector<std::string> want = *cols;
      for (const auto& v : req.vars)
        if (std::find(want.begin(), want.end(), v) == want.end()) want.push_back(v);
      mini = fetchColumns(session, want, session.workdir(), &miniPath);
    }
    if (mini) {
      for (const auto& v : req.vars)
        if (!mini->has(v)) {
          mini.reset();
          break;
        }
    }
  }
  Datastore* ds = mini ? mini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  for (const auto& v : req.vars)
    if (!ds->has(v)) return levelsRcError("variable " + v + " not found", 111);
  if (req.gtools && ds->nRows() < 1) return levelsRcError("no observations", 2000);

  Datastore* t = ds;
  std::unique_ptr<Datastore> filteredHolder;
  if ((req.ifExpr.has_value() && !req.ifExpr->empty()) || req.inRange.has_value()) {
    const std::string headPath = session.currentHbin().value_or(ds->path());
    std::optional<std::string> ifx;
    if (req.ifExpr.has_value() && !req.ifExpr->empty()) ifx = req.ifExpr;
    InRangeFilterResult fr;
    bool done = false;
    if (mini) {
      try {
        fr = applyIfAndInFilter(session, miniPath, ifx, req.inRange);
        done = true;
      } catch (const CobraError&) {
        done = false;  // a projection the filter cannot read: retry on the head
      }
    }
    if (!done) {
      try {
        fr = applyIfAndInFilter(session, headPath, ifx, req.inRange);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cmdName + ": " + cobraErrorStr(ce), ce.code);
      }
    }
    if (!fr.ok) return inRangeRejectError(fr, req.inRange.value_or(""), cmdName);
    if (fr.dataPath != miniPath && fr.dataPath != headPath) session.noteProjectionScratch(fr.dataPath);
    filteredHolder = std::make_unique<Datastore>(fr.dataPath);
    t = filteredHolder.get();
  }

  // The rows, as cells; a row with a missing value leaves unless `missing`.
  const int64_t n = t->nRows();
  std::vector<const cecon::Column*> cols;
  std::vector<bool> isStr;
  for (const auto& v : req.vars) {
    cols.push_back(&t->col(v));
    isStr.push_back(t->col(v).dtype == cecon::DType::Factor);
  }
  std::map<std::vector<LevelCell>, int64_t, LevelKeyLess> groups;
  int64_t kept = 0;
  // One variable (every levelsof): tally the rows by a flat key first -- the
  // string code, or the value's bits with -0 as 0 and one pattern per missing
  // kind (the classes stata_cmp makes equal) -- so the ordered map sees each
  // distinct level once instead of once per row.
  auto addLevel = [&](LevelCell cell, int64_t count) {
    if (levelCellMissing(cell) && !req.missing) return;
    kept += count;
    groups[std::vector<LevelCell>{std::move(cell)}] += count;
  };
  if (req.vars.size() == 1 && isStr[0]) {
    const int32_t* codes = cols[0]->codes();
    const auto& lv = cols[0]->levels;
    std::vector<int64_t> perCode(lv.size() + 1, 0);  // [0] = no level (blank)
    for (int64_t i = 0; i < n; ++i) {
      const int32_t code = codes[i];
      ++perCode[(code >= 0 && static_cast<size_t>(code) < lv.size()) ? static_cast<size_t>(code) + 1 : 0];
    }
    for (size_t k = 0; k < perCode.size(); ++k) {
      if (perCode[k] == 0) continue;
      LevelCell cell;
      cell.isStr = true;
      if (k > 0) cell.s = lv[k - 1];
      addLevel(std::move(cell), perCode[k]);
    }
  } else if (req.vars.size() == 1) {
    const double* v = cols[0]->f64();
    std::unordered_map<uint64_t, std::pair<double, int64_t>> tally;
    for (int64_t i = 0; i < n; ++i) {
      const double d = v[i] == 0.0 ? 0.0 : v[i];
      uint64_t bits;
      if (std::isnan(d)) bits = 0x7ff8000000000000ull + static_cast<uint64_t>(cecon::missing_kind(d));
      else std::memcpy(&bits, &d, sizeof bits);
      auto it = tally.find(bits);
      if (it == tally.end()) tally.emplace(bits, std::make_pair(d, int64_t{1}));
      else ++it->second.second;
    }
    for (const auto& [bits, rep] : tally) {
      LevelCell cell;
      cell.d = rep.first;
      addLevel(std::move(cell), rep.second);
    }
  }
  for (int64_t i = 0; req.vars.size() > 1 && i < n; ++i) {
    std::vector<LevelCell> key(req.vars.size());
    bool anyMissing = false;
    for (size_t j = 0; j < req.vars.size(); ++j) {
      LevelCell& c = key[j];
      c.isStr = isStr[j];
      if (c.isStr) {
        const int32_t code = cols[j]->codes()[i];
        const auto& lv = cols[j]->levels;
        c.s = (code >= 0 && static_cast<size_t>(code) < lv.size()) ? lv[static_cast<size_t>(code)]
                                                                   : std::string();
      } else {
        c.d = cols[j]->f64()[i];
      }
      if (levelCellMissing(c)) anyMissing = true;
    }
    if (anyMissing && !req.missing) continue;
    ++kept;
    ++groups[key];
  }

  const std::string sep = req.separate.value_or(" ");
  const std::string colsep = req.colseparate.value_or(" | ");
  ojson out;
  out["type"] = "levelsof";
  out["varname"] = req.vars[0];
  ojson levelsArr = ojson::array();

  if (!req.gtools) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(kept));
    session.eval.rValues.set("r", static_cast<double>(groups.size()));
    std::string joined;
    if (!groups.empty()) {
      std::vector<std::string> texts;
      if (isStr[0]) {
        for (const auto& kv : groups) texts.push_back(kv.first[0].s);
        if (req.clean) {
          for (size_t i = 0; i < texts.size(); ++i) joined += (i ? sep : "") + texts[i];
        } else {
          joined = "`\"";
          for (size_t i = 0; i < texts.size(); ++i)
            joined += (i ? "\"'" + sep + "`\"" : std::string()) + texts[i];
          joined += "\"'";
        }
      } else {
        bool isint = true;
        for (const auto& kv : groups) {
          const double v = kv.first[0].d;
          if (!std::isnan(v) && v != std::floor(v)) isint = false;
        }
        for (const auto& kv : groups) {
          const double v = kv.first[0].d;
          texts.push_back(isint ? levelsofIntText(v)
                                : (std::isnan(v) ? levelMissingText(v) : eval::stataMacroRealText(v)));
        }
        for (size_t i = 0; i < texts.size(); ++i) joined += (i ? sep : "") + texts[i];
      }
      for (const auto& s : texts) levelsArr.push_back(s);
      session.eval.rStrings.set("levels", joined);
      eval::declareRPosted(session.eval, "levelsof", {"N", "r", "levels"});
    } else {
      eval::declareRPosted(session.eval, "levelsof", {"N", "r"});
    }
    if (req.localName.has_value() && !req.localName->empty())
      session.eval.locals.set(*req.localName, joined);
    out["levels"] = levelsArr;
    out["text"] = joined;
    return out;
  }

  // glevelsof
  if (groups.empty()) {
    ojson b;
    b["type"] = "display";
    b["text"] = "(no observations)";
    return b;
  }
  std::vector<std::string> rows;
  int64_t minJ = std::numeric_limits<int64_t>::max(), maxJ = 0;
  for (const auto& kv : groups) {
    minJ = std::min(minJ, kv.second);
    maxJ = std::max(maxJ, kv.second);
    std::string row;
    for (size_t j = 0; j < kv.first.size(); ++j) {
      const LevelCell& c = kv.first[j];
      std::string cell = c.isStr ? (req.clean ? c.s : compoundQuote(c.s)) : glevelsofNumText(c.d);
      row += (j ? colsep : std::string()) + cell;
    }
    rows.push_back(req.vars.size() > 1 ? compoundQuote(row) : row);
  }
  std::string joined;
  for (size_t i = 0; i < rows.size(); ++i) joined += (i ? sep : "") + rows[i];
  if (req.vars.size() == 1 && !isStr[0]) {
    joined = replaceAllOf(joined, " 0.", " .");
    joined = replaceAllOf(joined, "-0.", "-.");
  }
  // The block's `levels` lists the levels themselves, as levelsof's does: a
  // string level unquoted (its compound quotes belong to the r(levels) / local
  // text above). A multi-variable row has no plainer form than the quoted row.
  size_t gi = 0;
  for (const auto& kv : groups) {
    const bool bareStr = req.vars.size() == 1 && kv.first[0].isStr;
    levelsArr.push_back(bareStr ? kv.first[0].s : rows[gi]);
    ++gi;
  }
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("levels", joined);
  session.eval.rStrings.set("sep", sep);
  session.eval.rStrings.set("colsep", colsep);
  session.eval.rValues.set("N", static_cast<double>(kept));
  session.eval.rValues.set("J", static_cast<double>(groups.size()));
  session.eval.rValues.set("minJ", static_cast<double>(minJ));
  session.eval.rValues.set("maxJ", static_cast<double>(maxJ));
  eval::declareRPosted(session.eval, "glevelsof",
                       {"N", "J", "minJ", "maxJ", "levels", "sep", "colsep"});
  if (req.localName.has_value() && !req.localName->empty())
    session.eval.locals.set(*req.localName, joined);
  out["levels"] = levelsArr;
  out["text"] = req.silent ? std::string() : joined;
  if (req.silent) out["silent"] = true;
  return out;
}

// ---------------------------------------------------------------------------
// M3b: clear / set obs / cd / pwd / noop (set|version|compress) — foundational
// commands the corpus's data-construction idiom (`clear` -> `set obs N` ->
// `gen ...`) depends on. C++ port of dispatcher.py's clear/set_obs/cd/pwd
// handlers + session.py's clear_data.

// BS-044 (BETA campaign C4): `clear <subcmd>` scoping. Before this fix
// EVERY subcommand fell through to the same bare-data-wipe path (probe
// bs044: `clear results` answered "No dataset is loaded"), which is wrong
// for every subcommand except a genuinely bare `clear`/`clear all`.
//
// Ground truth here is Stata's own clear.ado (StataNow 19.5, dumped via
// `program list` while live-probing this item — not a guess):
//   ""            -> `drop _all` + `label drop _all` ONLY. Scalars,
//                     matrices, e()/r() results and est-store all SURVIVE
//                     a bare `clear` (live-probed: sca1/matrix m1/ereturn
//                     list all still present after a bare `clear`).
//   "all"/"*"     -> everything (data, labels, matrices, scalars, e()/r(),
//                     programs, panelvar/timevar).
//   "results"/    -> ONE shared branch in the real ado: `return clear` +
//   "matrix"         `ereturn clear` + `sreturn clear` + `_return drop
//                     _all` (r()/e()/s() results only — NOT scalars, NOT
//                     matrices). `matrix` ADDITIONALLY does `matrix drop
//                     _all` + `_est drop _all` (user matrices + the eststo
//                     store) — oracle-pinned (`clear results` keeps N=336
//                     + scalar list) plus this ado reading for the r()/e()
//                     split and the matrix/results relationship.
//   "programs"    -> `program drop _all` (session.programs — real store).
//   "mata"        -> empties the M0 Mata store (session.eval.mataMatrices).
//   "ado"/"rngstream"/"rngstreams"/"frames"/"collect" -> real Stata
//                     subcommands with genuinely NO backing store in this
//                     build (no frames/collect engine, no rngstream
//                     state, no ado-vs-interactive program provenance) —
//                     honest scoped no-ops with a note, replacing the old
//                     "wipe the whole dataset" mistake, not a silent
//                     no-message pass-through.
//   anything else -> real Stata hard-rejects ("`1' not allowed", r(198));
//                     matched here rather than silently no-opping OR
//                     wiping data for a typo'd subcommand.
//
// Deviation from this batch's task-brief shorthand (noted for review): the
// brief grouped "matrix" with the no-store no-op list, but PIET already
// carries a real matrix store (eval.matrices/eval.eMatrices, populated by
// the `matrix define` handler) and a real est-store (session.estimates) —
// clearing them costs no new infrastructure and matches verified Stata
// truth, so `clear matrix` is implemented as a genuine clear here, not a
// no-op. See this task's return report for the full callout.

}  // namespace piet::handlers
