// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/utility.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel: mdesc's r(percent) on no rows

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/handlers/descriptive.h"  // ListLayout / buildListing: missings lists through list
#include "engine/scratch_columns.h"     // the if/in sample
#include "data/missing.h"              // sysmiss and breakdown read the missing code

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

namespace {
// The width a display format asks for (`%5.2f` 5).
bool parseStrictSignedInt(const std::string& t, long long* out) {
  if (t.empty()) return false;
  size_t i = (t[0] == '-' || t[0] == '+') ? 1 : 0;
  if (i >= t.size()) return false;
  for (size_t k = i; k < t.size(); ++k)
    if (!std::isdigit(static_cast<unsigned char>(t[k]))) return false;
  try {
    *out = std::stoll(t);
  } catch (...) {
    return false;
  }
  return true;
}
long long listFormatWidthOf(const std::string& fmt) {
  size_t i = 1;
  if (i < fmt.size() && fmt[i] == '-') ++i;
  long long w = 0;
  while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) w = w * 10 + (fmt[i++] - '0');
  return w;
}
}  // namespace

// ===========================================================================
// missings
// ===========================================================================
//
// FOUR SUB-VERBS, FOUR EXISTING PRIMITIVES. `report` reads the card; `dropvars`
// is runDrop; `dropobs` is runDropIf; `tag` is runEgen's rowmiss. The
// sub-verb is the first positional token, exactly as in missings.ado.
//
// WHY `report`'s OBSERVATION COUNT DOES NOT GO THROUGH rowmiss, which is what
// the plan specifies. `Frame::row_egen` calls `require_f64` on every column
// (src/data/frame.cpp), so rowmiss over a varlist containing a string variable
// THROWS -- and `missings report` with no varlist covers every variable, which
// on any real dataset includes strings. The two ways to use rowmiss here are
// (a) let the report fail on any dataset with a string variable, or (b) run it
// over the numeric subset and print a count that silently ignores missing
// string cells. Neither is acceptable, so the per-OBSERVATION predicate is
// computed here, over the same columns the per-VARIABLE table reports, while
// the per-variable COUNTS still come from the card and are never re-derived.
// `tag` still routes to runEgen: the user asked for that variable by name, and
// an honest engine error on a string varlist beats a second rowmiss.
ojson runMissings(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMissings.known,
                                            kVocabMissings.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return utilErrorRc("subcommand needed; see help on missings", 198);

  // missings.ado's subcommands: report, breakdown and list to one letter, table to
  // three, tag / dropvars / dropobs only in full.
  const std::string typedSub = lowerOf(parsed.varlist[0]);
  std::string sub;
  auto abbr = [&](const std::string& full, size_t minLen) {
    return typedSub.size() >= minLen && typedSub.size() <= full.size() &&
           full.compare(0, typedSub.size(), typedSub) == 0;
  };
  if (abbr("report", 1)) sub = "report";
  else if (abbr("breakdown", 1)) sub = "breakdown";
  else if (abbr("list", 1)) sub = "list";
  else if (abbr("table", 3)) sub = "table";
  else if (typedSub == "tag" || typedSub == "dropvars" || typedSub == "dropobs") sub = typedSub;
  else return utilErrorRc("illegal missings subcommand", 198);
  if (sub == "table") {
    return ojson(eval::unsupportedCommand("missings table", std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }
  const std::vector<std::string> asked(parsed.varlist.begin() + 1, parsed.varlist.end());

  // missings' own options; every other one is list's (report / breakdown / list
  // hand `*` to list).
  bool obsNoobsClash = false;
  bool numericOpt = false, stringOpt = false, sysmiss = false, noheader = false;
  bool observations = false, percent = false, sortOpt = false;
  std::optional<double> minimum;
  std::optional<long long> show;
  std::string pcFormat = "%5.2f";
  std::vector<std::string> identify, sortWords;
  std::vector<std::pair<std::string, eval::OptValue>> listOpts;
  // OBServations takes three letters: a shorter `o` / `ob` is list's `obs` (the
  // positive of noobs), which the noobs report / breakdown hand list contradicts:
  // r(198) "option noobs not allowed" (bank 05912eb289a76a0d `missings report, ob`).
  std::vector<std::string> typedNames;
  try {
    for (const auto& kv : eval::tokenize(parsed.raw).opts) typedNames.push_back(lowerOf(kv.first));
  } catch (const eval::TokenizerError&) {
  }
  bool obsShort = false;
  if (typedNames.size() == parsed.opts.size())
    for (size_t t = 0; t < typedNames.size(); ++t)
      if (parsed.opts[t].first == "observations" && typedNames[t].size() < 3) obsShort = true;
  for (const auto& [k, v] : parsed.opts) {
    if (obsShort && k == "observations") {
      if (sub == "report" || sub == "breakdown") obsNoobsClash = true;
      continue;
    }
    if (k == "numeric") numericOpt = true;
    else if (k == "string") stringOpt = true;
    else if (k == "sysmiss") sysmiss = true;
    else if (k == "noheader") noheader = true;
    else if (k == "observations" && sub == "report") observations = true;
    else if (k == "percent" && sub == "report") percent = true;
    else if (k == "format" && sub == "report" && !v.isBool) pcFormat = strutil::trim(v.strValue);
    else if (k == "sort" && (sub == "report" || sub == "breakdown")) {
      sortOpt = true;
      if (!v.isBool)
        for (const auto& w : splitWs(v.strValue)) sortWords.push_back(lowerOf(w));
    } else if (k == "show" && (sub == "report" || sub == "breakdown")) {
      long long x = 0;
      if (v.isBool || !parseStrictInt(strutil::trim(v.strValue), &x) || x < 1)
        return utilErrorRc("show() invalid -- invalid numlist", 121);
      show = x;
    } else if (k == "minimum" && sub != "tag") {
      double x = 0;
      if (v.isBool) return utilErrorRc("option minimum() incorrectly specified", 198);
      try {
        size_t used = 0;
        x = std::stod(strutil::trim(v.strValue), &used);
      } catch (...) {
        return utilErrorRc("minimum() invalid -- invalid numlist", 121);
      }
      if (x < 0)  // numlist max=1 >=0 (bank 96c5c350a8044a62)
        return utilErrorRc("minimum() invalid -- invalid number, outside of allowed range", 125);
      minimum = x;
    } else if (k == "identify" && (sub == "report" || sub == "list") && !v.isBool) {
      identify = splitWs(v.strValue);
    } else if (k == "generate" || k == "force") {
      continue;  // tag / dropvars / dropobs read these below
    } else {
      listOpts.emplace_back(k, v);
    }
  }
  if (sortOpt && sortWords.empty()) sortWords = {"missing", "descending"};
  bool byMissing = false, byAlpha = false, descending = false;
  for (const auto& w : sortWords) {
    if (std::string("missings").compare(0, w.size(), w) == 0) byMissing = true;
    else if (std::string("alpha").compare(0, w.size(), w) == 0) byAlpha = true;
    else if (std::string("descending").compare(0, w.size(), w) == 0) descending = true;
    else return utilErrorRc("sort() option invalid?", 198);
  }
  if (sortOpt && ((byMissing && byAlpha) || (!byMissing && !byAlpha && descending)))
    return utilErrorRc("sort request invalid", 198);

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("missings: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(*dsp, "missings " + sub, asked, &vars)) return *err;
  std::string vartext = joinWith(vars, " ");
  if (asked.empty()) vartext = "all variables";
  if (numericOpt != stringOpt) {
    std::vector<std::string> kept;
    for (const auto& nm : vars)
      if ((dsp->dtype(nm) == "factor") == stringOpt) kept.push_back(nm);
    if (kept.empty()) return utilErrorRc("no variables specified", 100);  // bank 3178e39ae3a4a657
    vars = kept;
    if (asked.empty()) vartext = std::string("all ") + (numericOpt ? "numeric" : "string") + " variables";
  }
  for (auto& w : identify) {
    std::vector<std::string> one;
    if (auto err = resolveVarlist(*dsp, "missings " + sub, {w}, &one)) return *err;
    w = one.front();
  }

  // The sample: `marksample touse, novarlist` (if / in only).
  std::vector<char> touse(static_cast<size_t>(dsp->nRows()), 1);
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
    std::string qual;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) qual += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) qual += " in " + *parsed.inRange;
    if (!qual.empty() && sub != "dropobs") {
      ScratchColumns scratch(session);
      std::string mark;
      if (auto failed = scratch.add("1", qual, &mark)) return *failed;
      const auto m = scratch.values(mark);
      for (size_t r = 0; r < touse.size(); ++r) touse[r] = std::isnan(m[r]) ? 0 : 1;
    }
    dsp = session.datastore();
    if (dsp == nullptr) return ojson(eval::noDataLoaded());
  }
  Datastore& ds = *dsp;
  const int64_t n = ds.nRows();
  int64_t nUsed = 0;
  for (char t : touse) nUsed += t ? 1 : 0;
  const bool force = hasFlag(parsed.opts, "force");

  if (sub == "report" || sub == "breakdown" || sub == "list") {
    if (nUsed == 0) return utilErrorRc("no observations", 2000);
    // Per variable, the missing count over the sample (sysmiss: a number's `.` only).
    std::vector<ColView> views;
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    auto isMiss = [&](size_t k, int64_t i) {
      if (sysmiss && !views[k].isFactor) {
        const double v = views[k].d[i];
        return std::isnan(v) && cecon::missing_kind(v) == 0;
      }
      return cellMissing(views[k], i);
    };
    std::vector<double> nmissing(static_cast<size_t>(n), NAN);
    std::vector<int64_t> counts(vars.size(), 0);
    for (int64_t i = 0; i < n; ++i) {
      if (!touse[static_cast<size_t>(i)]) continue;
      double c = 0;
      for (size_t k = 0; k < vars.size(); ++k)
        if (isMiss(k, i)) {
          c += 1;
          ++counts[k];
        }
      nmissing[static_cast<size_t>(i)] = c;
    }
    const double minN = minimum.value_or(1.0);
    std::vector<std::string> misslist;
    std::vector<double> misscounts;
    for (size_t k = 0; k < vars.size(); ++k)
      if (static_cast<double>(counts[k]) >= minN) {
        misslist.push_back(vars[k]);
        misscounts.push_back(static_cast<double>(counts[k]));
      }
    int64_t nm = 0;
    for (int64_t i = 0; i < n; ++i)
      if (touse[static_cast<size_t>(i)] && nmissing[static_cast<size_t>(i)] > 0) ++nm;
    const std::string system = sysmiss ? "system " : "";

    ojson blocks = ojson::array();
    std::string head = noheader ? std::string() : "Checking missings in " + vartext + ":\n";
    blocks.push_back(displayBlock(head + std::to_string(nm) + " " +
                                  (nm == 1 ? "observation" : "observations") + " with " + system +
                                  "missing values"));
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(nUsed));
    std::vector<std::string> posted = {"N"};
    auto finish = [&]() {
      eval::declareRPosted(session.eval, "missings", posted);
      return blocks;
    };
    auto setVarlist = [&]() {
      session.eval.rStrings.set("varlist", joinWith(misslist, " "));
      posted.push_back("varlist");
    };

    ListLayout lo;
    if (auto bad = parseListLayout(listOpts, parsed.raw, &lo)) return *bad;
    if (obsNoobsClash && nm > 0 && !observations) {
      blocks.push_back(utilErrorRc("option noobs not allowed", 198));
      return blocks;
    }
    auto pushListing = [&](const std::vector<int64_t>& rows, const std::vector<int64_t>& obs,
                           const std::vector<std::string>& listVars,
                           const std::vector<ListLeadCol>& lead) -> std::optional<ojson> {
      ojson err;
      ojson listing = buildListing(session, ds, rows, obs, listVars, lo, &err, lead);
      if (listing.is_null()) return err;
      listing["omitted"] = 0;
      ojson b;
      b["type"] = "browse";
      b["editable"] = false;
      b["columns"] = ojson::array();
      b["rows"] = ojson::array();
      b["listing"] = std::move(listing);
      blocks.push_back(std::move(b));
      return std::nullopt;
    };

    if (sub == "list") {
      // `list identify|misslist if nmissing >= min & touse, <options>`
      if (nm > 0) {
        std::vector<std::string> showVars = identify;
        for (const auto& v : misslist)
          if (std::find(showVars.begin(), showVars.end(), v) == showVars.end()) showVars.push_back(v);
        std::vector<int64_t> rows, obs;
        for (int64_t i = 0; i < n; ++i)
          if (touse[static_cast<size_t>(i)] && nmissing[static_cast<size_t>(i)] >= minN) {
            rows.push_back(i);
            obs.push_back(i + 1);
          }
        if (!showVars.empty())
          if (auto bad = pushListing(rows, obs, showVars, {})) return *bad;
      }
      setVarlist();
      return finish();
    }
    if (nm == 0) {
      if (sub == "breakdown") setVarlist();
      return finish();  // report exits before its r(varlist)
    }
    if (sub == "report" && observations) {
      // `list identify nmissing pcmissing if nmissing >= min, abbrev(9) subvarname` --
      // nmissing is missing outside the sample, which is >= any number.
      lo.subvarname = true;
      if (lo.abbreviate < 0) lo.abbreviate = 9;
      std::vector<int64_t> rows, obs;
      ListLeadCol nmCol{"# " + system + "missing", {}, 9};
      ListLeadCol pcCol{"% " + system + "missing", {}, listFormatWidthOf(pcFormat)};
      nmCol.trailing = pcCol.trailing = true;
      pcCol.format = pcFormat;
      for (int64_t i = 0; i < n; ++i) {
        const double c = nmissing[static_cast<size_t>(i)];
        if (!(std::isnan(c) || c >= minN)) continue;
        rows.push_back(i);
        obs.push_back(i + 1);
        nmCol.values.push_back(c);
        pcCol.values.push_back(std::isnan(c) ? NAN : 100.0 * c / static_cast<double>(vars.size()));
      }
      std::vector<ListLeadCol> lead = {nmCol};
      if (percent) lead.push_back(pcCol);
      if (auto bad = pushListing(rows, obs, identify, lead)) return *bad;
      return finish();
    }
    // report / breakdown: one line per variable in misslist.
    std::vector<size_t> order(misslist.size());
    for (size_t k = 0; k < order.size(); ++k) order[k] = k;
    if (sortOpt) {
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (byMissing) return descending ? misscounts[a] > misscounts[b] : misscounts[a] < misscounts[b];
        return descending ? misslist[a] > misslist[b] : misslist[a] < misslist[b];
      });
      if (show.has_value() && static_cast<size_t>(*show) < order.size()) order.resize(static_cast<size_t>(*show));
    }
    ListLeadCol which{" ", {}, 0};
    size_t whichW = 0;
    for (const auto& v : misslist) whichW = std::max(whichW, v.size());
    which.fmtw = static_cast<long long>(whichW);
    ListLeadCol all{"# missing", {}, 9};
    for (size_t k : order) {
      which.texts.push_back(misslist[k]);
      all.values.push_back(misscounts[k]);
    }
    std::vector<ListLeadCol> lead = {which, all};
    if (sub == "report" && percent) {
      ListLeadCol pc{"% missing", {}, listFormatWidthOf(pcFormat)};
      pc.format = pcFormat;
      for (size_t k : order) pc.values.push_back(100.0 * misscounts[k] / static_cast<double>(nUsed));
      lead.push_back(pc);
    }
    if (sub == "breakdown") {
      // a column per missing code found among the numeric variables (`.` alone with
      // sysmiss), and `empty` for the string ones.
      std::set<int> kinds;
      bool anyString = false, anyNumeric = false;
      for (const auto& v : misslist) {
        const ColView cv = viewOf(ds, v);
        if (cv.isFactor) {
          anyString = true;
          continue;
        }
        anyNumeric = true;
        for (int64_t i = 0; i < n; ++i)
          if (touse[static_cast<size_t>(i)] && std::isnan(cv.d[i]))
            kinds.insert(sysmiss ? 0 : cecon::missing_kind(cv.d[i]));
      }
      if (anyString) {
        ListLeadCol empty{"empty", {}, 9};
        for (size_t k : order) {
          const ColView cv = viewOf(ds, misslist[k]);
          empty.values.push_back(cv.isFactor ? misscounts[k] : NAN);
        }
        lead.push_back(empty);
      }
      if (anyNumeric)
        for (int kind : kinds) {
          ListLeadCol kc{cecon::missing_text(kind), {}, 9};
          for (size_t k : order) {
            const ColView cv = viewOf(ds, misslist[k]);
            if (cv.isFactor) {
              kc.values.push_back(NAN);
              continue;
            }
            double c = 0;
            for (int64_t i = 0; i < n; ++i)
              if (touse[static_cast<size_t>(i)] && std::isnan(cv.d[i]) &&
                  cecon::missing_kind(cv.d[i]) == kind)
                c += 1;
            kc.values.push_back(c);
          }
          lead.push_back(kc);
        }
    }
    lo.subvarname = true;
    lo.noobs = true;
    if (lo.abbreviate < 0) lo.abbreviate = 9;
    std::vector<int64_t> rows(order.size()), obs(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
      rows[k] = 0;
      obs[k] = static_cast<int64_t>(k + 1);
    }
    if (auto bad = pushListing(rows, obs, {}, lead)) return *bad;
    setVarlist();
    return finish();
  }

  // missings: dropvars/dropobs on a dataset changed since it was
  // last saved need `force` -- `force option required with changed dataset`,
  // exit 4 (w2d_uncovered.log missings_dropvars_ctl / missings_dropobs_ctl
  // `4|4|6|`). That is the ado's only use of the option.
  if ((sub == "dropvars" || sub == "dropobs") && !force && session.datasetChanged)
    return utilErrorRc("force option required with changed dataset", 4);

  if (sub == "dropvars") {
    // missings: a variable qualifies when it is missing in every
    // observation used (`r(N) == `N''); :440-447 drop them and return
    // r(varlist) = the dropped names (missings_dropvars_force `0|2|6|b s|`).
    if (n == 0) return utilErrorRc("no observations", 2000);
    std::vector<std::string> doomed;
    for (const auto& nm : vars) {
      if (missingCountOf(session, ds, nm) == n) doomed.push_back(nm);
    }
    ojson out = silent();
    if (!doomed.empty()) {
      out = runDrop(doomed, session);
      if (firstError(out) != nullptr) return out;
    }
    session.eval.rValues.set("N", static_cast<double>(n));
    session.eval.rStrings.set("varlist", joinWith(doomed, " "));
    return out;
  }

  if (sub == "dropobs") {
    // missings: drop the observations missing on EVERY variable in
    // scope (`nmissing' == `nvars'`), strings included (missing() of "" is 1),
    // within the qualifier; r(n_dropped) is their count (:453). The row test is
    // the report's own per-cell predicate, written as a hidden column so the
    // drop itself goes through the ordinary `drop if` path with the user's
    // if/in ANDed in.
    std::vector<ColView> views;
    views.reserve(vars.size());
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      bool all = !views.empty();
      for (const auto& v : views) {
        if (!cellMissing(v, i)) {
          all = false;
          break;
        }
      }
      cells.push_back(numCell(all ? 1.0 : 0.0));
    }
    const std::string tag = "__missings_dropobs_tag";
    std::string tagOut;
    if (auto err = writeColumn(session, *session.currentHbin(), tag, cells, "f64", true, &tagOut))
      return *err;
    session.load(tagOut);
    std::string cond = tag + " == 1";
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty())
      cond = "(" + *parsed.ifExpr + ") & (" + cond + ")";
    Datastore* before = session.datastore();
    const int64_t n0 = before != nullptr ? before->nRows() : n;
    ojson out = runDropIf(cond, session, parsed.inRange);
    if (firstError(out) != nullptr) {
      runDrop({tag}, session);
      return out;
    }
    Datastore* after = session.datastore();
    const int64_t n1 = after != nullptr ? after->nRows() : n0;
    ojson dropTag = runDrop({tag}, session);
    if (firstError(dropTag) != nullptr) return dropTag;
    session.eval.rValues.set("N", static_cast<double>(n));
    session.eval.rValues.set("n_dropped", static_cast<double>(n0 - n1));
    return out;
  }

  // tag
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"generate", "gen"});
  if (!gen.has_value())
    return utilSyntax("missings tag: requires generate(newvar) -- the tag has nowhere to go "
                      "otherwise.");
  ojson guard = newVarGuard(session, "missings tag", *gen);
  if (!guard.is_null()) return guard;

  // rowmiss ONLY when every variable in scope is numeric. `Frame::row_egen`
  // calls `require_f64`, so rowmiss over a varlist containing a string variable
  // THROWS -- and `missings tag` with no varlist covers every variable, which on
  // any real dataset includes strings. Measured: the registry pangram's
  // `missings tag, generate(...)` on kitchensink_panel (which has the factor
  // column `grp`) died with "column 'grp' is a factor, not f64".
  //
  // The fallback is NOT a second definition of missingness: it is the SAME
  // per-row predicate `report` computes a dozen lines above, over the same
  // ColViews, so the count `report` prints and the column `tag` writes can
  // never disagree. Restricting rowmiss to the numeric subset instead would
  // have tagged rows on a narrower test than the user asked for -- a silently
  // wrong column, which is the one outcome worth more code to avoid.
  bool allNumeric = true;
  for (const auto& nm : vars) {
    if (ds.dtype(nm) == "factor") {
      allNumeric = false;
      break;
    }
  }
  // missings.ado's tag is rclass: r(N) is the sample size (bank 3e85c85701931651).
  auto postTagN = [&](ojson out) {
    if (firstError(out) == nullptr) {
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("N", static_cast<double>(nUsed));
      eval::declareRPosted(session.eval, "missings", {"N"});
    }
    return out;
  };
  if (nUsed == 0) return utilErrorRc("no observations", 2000);
  if (allNumeric) {
    return postTagN(runEgen(*gen + " = rowmiss(" + joinWith(vars, " ") + ")", std::string(),
                            parsed.ifExpr, {}, session, parsed.inRange));
  }

  std::vector<ColView> tagViews;
  tagViews.reserve(vars.size());
  for (const auto& nm : vars) tagViews.push_back(viewOf(ds, nm));
  std::vector<std::string> tagCells;
  tagCells.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    double c = 0.0;
    for (const auto& v : tagViews) {
      if (cellMissing(v, i)) c += 1.0;
    }
    tagCells.push_back(touse[static_cast<size_t>(i)] ? numCell(c) : std::string());
  }
  std::string tagOut;
  if (auto err = writeColumn(session, *session.currentHbin(), *gen, tagCells, "f64", true, &tagOut))
    return *err;
  session.load(tagOut);
  return postTagN(mutationResult(session, "gen", {*gen}));
}

// ===========================================================================
// mdesc
// ===========================================================================
//
// The same card counts `missings report` reads, in mdesc's own three-column
// layout. Nothing is scanned: every number below is either the card's or
// arithmetic on it.
ojson runMdesc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMdesc.known,
                                            kVocabMdesc.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // mdesc.ado: `marksample touse, novarlist` -- the if/in sample is the total
  // of every row, and each variable's missing count is taken inside it
  // (vs-stata bank: `mdesc rep78 if foreign==1` -> miss 1, total 22).
  std::vector<std::string> wantCols = parsed.varlist;
  ReadScope scope(session, parsed, wantCols, "mdesc", true);
  if (scope.failed())
    return scope.error().is_null() ? utilError("mdesc: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "mdesc", parsed.varlist, &vars)) return *err;
  const int64_t n = ds.nRows();

  // mdesc.ado (1.x): abbreviate(#) default 12, capped at 32; any / all / none count
  // the observations with a missing value in any / every / none of the variables.
  long long ab = 12;
  bool any = false, all = false, none = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "abbreviate") {
      if (v.isBool || !parseStrictSignedInt(strutil::trim(v.strValue), &ab))
        return utilErrorRc("option abbreviate() incorrectly specified", 198);
    } else if (k == "any") {
      any = true;
    } else if (k == "all") {
      all = true;
    } else if (k == "none") {
      none = true;
    }
  }
  if (static_cast<int>(any) + static_cast<int>(all) + static_cast<int>(none) > 1)
    return utilErrorRc("specify only the any option or the all option or the none", 198);
  std::string note;
  if ((any || all) && vars.size() == 1) {
    note = std::string("since only 1 variable was specified, the ") + (any ? "any" : "all") +
           " option will be ignored\n";
    any = all = false;
  }
  if (ab > 32) ab = 32;
  int c1 = 17, c2 = 16, c3 = 47;
  if (ab > 16) {
    c1 = static_cast<int>(ab) + 2;
    c2 = static_cast<int>(ab) + 1;
    c3 = static_cast<int>(ab) + 30;
  }
  // Stata's abbrev(): no name is cut below five characters.
  auto abbrevTo = [&](const std::string& name) {
    const size_t w = static_cast<size_t>(std::max<long long>(ab, 5));
    if (name.size() <= w) return name;
    return name.substr(0, w - 2) + "~" + name.substr(name.size() - 1);
  };
  auto gc = [](double v, int w) {  // %w.0gc
    std::string t = intText(static_cast<int64_t>(v));
    return rightAlign(t, static_cast<size_t>(w));
  };
  auto f82 = [](double v) {
    if (std::isnan(v) || v >= eval::kStataMissingSentinel) return rightAlign(".", 8);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%8.2f", v);
    return std::string(buf);
  };
  const std::string rule = std::string(static_cast<size_t>(c2), '-') + "+" +
                           std::string(static_cast<size_t>(c3), '-');
  auto col = [&](const std::string& left) {
    return left.size() >= static_cast<size_t>(c1 - 1) ? left + " "
                                                      : left + std::string(static_cast<size_t>(c1 - 1) - left.size(), ' ');
  };
  std::ostringstream oss;
  oss << note << "\n";
  if (none) {
    oss << col("            ") << "|      None                        Percent              \n";
    oss << col("    Variable") << "|     Missing          Total     Not Missing\n";
  } else {
    oss << col("    Variable") << "|     Missing          Total     Percent Missing\n";
  }
  oss << rule << "\n";
  auto label = [&](const std::string& text) {
    return rightAlign(abbrevTo(text), static_cast<size_t>(c1 - 2)) + " ";
  };
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted = {"miss", "total", "percent"};
  auto pct = [&](int64_t miss) {
    return n > 0 ? 100.0 * static_cast<double>(miss) / static_cast<double>(n)
                 : eval::kStataMissingSentinel;
  };
  if (!any && !all && !none) {
    int64_t lastMiss = 0;
    std::string missVars, notMissVars;
    for (const auto& nm : vars) {
      const ColView v = viewOf(ds, nm);
      int64_t miss = 0;
      for (int64_t i = 0; i < n; ++i) miss += cellMissing(v, i) ? 1 : 0;
      oss << label(nm) << "| " << gc(static_cast<double>(miss), 11) << "    " << gc(static_cast<double>(n), 11)
          << "       " << f82(pct(miss)) << "\n";
      lastMiss = miss;
      if (miss > 0) missVars += (missVars.empty() ? "" : " ") + nm;
      else notMissVars += (notMissVars.empty() ? "" : " ") + nm;
    }
    oss << rule;
    // r() describes the LAST variable; the two lists are mdesc's `return local`s
    // (an empty one is not set).
    session.eval.rValues.set("miss", static_cast<double>(lastMiss));
    session.eval.rValues.set("total", static_cast<double>(n));
    session.eval.rValues.set("percent", pct(lastMiss));
    if (!missVars.empty()) {
      session.eval.rStrings.set("miss_vars", missVars);
      posted.push_back("miss_vars");
    }
    if (!notMissVars.empty()) {
      session.eval.rStrings.set("notmiss_vars", notMissVars);
      posted.push_back("notmiss_vars");
    }
  } else {
    std::vector<ColView> views;
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    int64_t count = 0;
    for (int64_t i = 0; i < n; ++i) {
      size_t m = 0;
      for (const auto& v : views) m += cellMissing(v, i) ? 1 : 0;
      if ((any && m > 0) || (all && m == views.size()) || (none && m == 0)) ++count;
    }
    oss << label(any ? "any vars" : all ? "all vars" : "no vars") << "|    "
        << gc(static_cast<double>(count), 8) << "    " << gc(static_cast<double>(n), 8) << "       "
        << f82(pct(count)) << "\n"
        << rule;
    session.eval.rValues.set("miss", static_cast<double>(count));
    session.eval.rValues.set("total", static_cast<double>(n));
    session.eval.rValues.set("percent", pct(count));
  }
  eval::declareRPosted(session.eval, "mdesc", posted);
  return displayBlock(oss.str());
}


}  // namespace piet::handlers
