// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// encode STRVAR, gen(NEWVAR) [label(LBLNAME)]: string -> labelled numeric codes.
#include "piet/handlers/data_values.h"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "datamgmt_support.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

// ---------------------------------------------------------------------------
// encode STRVAR, gen(NEWVAR) [label(LBLNAME)] — C++ port of encode_op.py's
// run_encode. Folded into M4a (ROADMAP M4a, blocking the debug_all.do depth
// boundary at line 157) even though it predates this campaign's explicit
// item list.
//
// L1/M3 (CSV-shim retirement): this calls cobra's native `encode` verb
// (`--var SRC --gen NEW`), which appends NEW as an f64 column (codes in
// alphabetical level order, 1-based, missing preserved) and byte-preserves
// the source and every other column — no whole-frame CSV dump, no
// re-factorization. NEW lands f64, not factor, exactly the oracle's contract
// (encode's output is a plain numeric column with a value-label ATTACHMENT).
// (History: this used to dump the whole dataset to CSV with the extra
// alphabetically-ranked column and `cobra convert` it back; superseded.)
ojson runEncode(const std::string& strvar, const std::string& newvar,
                const std::optional<std::string>& lblname, SessionState& session,
                const std::optional<std::string>& ifExpr, bool noExtend) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: encode's entry validation reads a NAME, a
  // DTYPE and another NAME — no column data — so the resident shadow card
  // answers all three, and the datastore open (cecon::Dataset::open: mmap +
  // 192-column header parse) is skipped. Same mechanism as runGsort/runOrder
  // above; the dtype half is the S4a treatment (row_evaluation.cpp's factorColumns),
  // where the A12 argument for trusting the card's dtype is written out in
  // full: the card's vocabulary is exactly Datastore::dtype()'s ("factor" |
  // "f64"), it is the BASE type on both sides, and any other spelling is
  // refused rather than guessed. Refusal here means falling into the else
  // branch, which is the untouched prior behavior.
  //
  // SCOPE, STATED HONESTLY. This is NOT where encode's measured ~0.24 s/call
  // goes. That is dominated by mutationResult's card rebuild rescanning all 192
  // columns for n_missing, because "encode" is missing from
  // SessionState::seedNMissForPureAdd's kPureAdd set (session_state.cpp) even
  // though encode is a textbook pure add — a one-token fix in a file this stint
  // does not touch. Removing the wasted open here does not substitute for it.
  //
  // The three facts the checks below need, resolved from the card when it can
  // supply them and from the datastore otherwise. Resolution is separated from
  // checking on purpose: the three rejections must keep firing in their
  // ORIGINAL order (not-found, then not-a-string, then already-defined), since
  // an input that trips two of them must keep returning the same message it
  // always did. Collapsing the lookups into the checks is what would silently
  // reorder them.
  bool haveFacts = false;
  bool srcExists = false;
  std::string srcDtype;
  bool newExists = false;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    const VarInfo* src = cardFind(*card, strvar);
    // An unrecognized dtype spelling is refused rather than guessed (S4a's
    // rule) — haveFacts stays false and the datastore branch re-answers all
    // three, exactly as before this fix.
    if (src == nullptr || src->dtype == "factor" || src->dtype == "f64") {
      srcExists = (src != nullptr);
      if (src != nullptr) srcDtype = src->dtype;
      newExists = (cardFind(*card, newvar) != nullptr);
      haveFacts = true;
    }
  }
  if (!haveFacts) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    srcExists = ds->has(strvar);
    if (srcExists) srcDtype = ds->dtype(strvar);
    newExists = ds->has(newvar);
  }

  // Stata's rc for each refusal (w2ah_data_verbs.log ah_encode_numeric: 107 --
  // main answered rc 7; ah_encode_exist: 110).
  if (!srcExists) {
    return rcErrorBlock("variable " + strvar + " not found", 111);
  }
  if (srcDtype != "factor") {
    return rcErrorBlock(strvar + ": not possible with numeric variable", 107);
  }
  // BUG-1096: noextend reads the label() set as it stands, so it needs one
  // (StataBE 19.5 order: the source checks, then this, then r(110)).
  if (noExtend && (!lblname.has_value() || lblname->empty())) {
    return rcErrorBlock("noextend option not allowed without label() option", 198);
  }
  // BUG-1120: after the source checks, a reserved generate() name is refused by
  // the new-variable rule with _se and the r(table) row names: "_cons invalid
  // varname", "long invalid varname", "str5 invalid variable name", r(198); _skip
  // and _all are legal here (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar, /*resultRows=*/true)) {
    return rcErrorBlock(r->message, r->rc);
  }
  if (newExists) {
    return rcErrorBlock("variable " + newvar + " already defined", 110);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // BUG-1096: under noextend every string the sample holds must already be a
  // text of the label() set (a set that does not exist has none); otherwise
  // nothing is created and the strangers are named, in sorted order, r(459):
  // "Unexpected values encountered:" / "ccc" (StataBE 19.5; "" is missing, never
  // a stranger, and rows outside `if` are not looked at).
  if (noExtend) {
    std::set<std::string> texts;
    if (auto set = session.labelDefs.find(*lblname); set != session.labelDefs.end())
      for (const auto& [code, text] : set->second) texts.insert(text);
    std::optional<Datastore> sampleFile;
    Datastore* sample = nullptr;
    if (haveIf) {
      try {
        InRangeFilterResult fr =
            applyIfAndInFilter(session, *session.currentHbin(), ifExpr, std::nullopt);
        sampleFile.emplace(fr.dataPath);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      } catch (const std::exception& e) {
        return engineErrorBlock(std::string("encode: ") + e.what());
      }
      sample = &*sampleFile;
    } else {
      sample = session.datastore();
    }
    if (sample == nullptr || !sample->has(strvar)) return eval::noDataLoaded();
    const cecon::Column& c = sample->col(strvar);
    std::set<std::string> strangers;
    for (int64_t i = 0; i < sample->nRows(); ++i) {
      const int32_t code = c.codes()[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
      std::string text(c.levels[static_cast<size_t>(code)]);
      if (!text.empty() && texts.count(text) == 0) strangers.insert(std::move(text));
    }
    if (!strangers.empty()) {
      std::string msg = "Unexpected values encountered:";
      for (const auto& s : strangers) msg += "\n" + s;
      return rcErrorBlock(msg, 459);
    }
  }

  // L1/M3 (CSV-shim retirement): cobra's native `encode` produces NEWVAR as
  // f64 with codes in ALPHABETICAL level order (1-based; missing stays
  // missing), byte-preserving the source column and every other column (no
  // whole-frame CSV dump, no re-factorization). Its stdout JSON carries the
  // alphabetical "levels" array, which drives the value-label attachment
  // below — codes and labels thus come from the SAME engine sort and always
  // agree. `--gen` lands f64, not factor: exactly encode's contract.
  //
  // BUG-042 (`encode STRVAR if COND, gen(NEWVAR)`): Stata restricts which
  // observations get a real code — excluded rows come back MISSING on NEWVAR,
  // while the level MAPPING still spans the WHOLE variable (encode's
  // value-label set is dataset-wide, not tied to one invocation's sample). So
  // encode runs on the full data (all rows -> whole-variable levels) into a
  // temp column, then a cobra `cond()` mask nulls the out-of-sample rows —
  // the identical `if`-folding runClonevar's numeric fast path uses.
  std::string encGen = haveIf ? ("__encode_full_" + scratchToken()) : newvar;
  std::string encOut = nextPath(session.workdir(), "encode");
  // *session.currentHbin() rather than ds->path() — same substitution runGsort
  // and runRename make, and the reason `ds` no longer needs to exist here.
  std::vector<std::string> encArgs = {"encode", "--data", *session.currentHbin(), "--out",
                                      encOut,   "--var",  strvar,                 "--gen", encGen};
  // C4-L4: cobra's cmd_encode rides the same §26 acquire_data/commit_frame
  // bridge as setcol (apps/cmds/cmds_data.cpp:693/:698 — read cross-lane, no
  // cobra change), so the SAME resident opt-in routes it through the frame
  // plane: virtual head, vars_delta fold, no whole-Datastore reconstruct —
  // and on a dirty virtual head no EMH materialization at the stateless
  // --data site (the a2 encode window class). The `if` mask's eval below
  // rides the same scope. Escapes as at the destring site; the stateless
  // fallback (plane down ⇒ no virtual head) is byte-identical to before.
  const bool residentEncode = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::optional<SessionState::ResidentEligible> encodeResident;
  if (residentEncode) encodeResident.emplace(session);
  nlohmann::json encJson;
  try {
    encJson = runCobraJson(encArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // Alphabetical levels straight from the engine's own encode output, so the
  // value labels below describe exactly the codes it assigned. A level that is
  // not UTF-8 arrives escaped (BUG-1372; cmd_encode) and is turned back here.
  std::vector<std::string> sortedLevels;
  if (encJson.contains("levels") && encJson.at("levels").is_array()) {
    for (const auto& lv : encJson.at("levels")) {
      sortedLevels.push_back(lv.get<std::string>());
      cecon::hbin_level_unescape(sortedLevels.back());
    }
  }

  if (haveIf) {
    // Mask out-of-sample rows to missing (in-sample rows keep encGen, itself
    // already missing where the source was missing), then drop the temp — the
    // same one-shot `--gen COND-cond() --drop TMP` shape collapse's proxy
    // step used.
    std::string finalOut = nextPath(session.workdir(), "encode");
    // BUG-369 follow-up: the mask cond() embeds the user's if, which can carry
    // a TS operator (`encode s if L.x>0, gen(v)`).
    std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + encGen + ",.)";
    std::vector<std::string> maskArgs = translate::injectTsContextForExpr(
        {"eval", "--data", encOut, "--gen", maskExpr, "--drop", encGen, "--out", finalOut},
        maskExpr, session);
    CobraRunResult cr = runCobra(maskArgs);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(finalOut);
  } else {
    session.load(encOut);
  }

  // The value-label set encode fills and attaches. Its name is label() when
  // given, else the NEW variable's (w2ah_data_verbs.log ah_encode_gen: `: value
  // label e_es' is e_es; main named it after the source, es). When the set
  // already exists its codes are reused and each new string is appended at the
  // set's largest code + 1, in sorted order (ah_encode_label_exist: ordl = {1
  // pear, 5 kiwi} gives pear 1, Pear 6, apple 7, fig 8; ah_encode_label_again
  // reuses intervention's codes). The engine numbered the sorted levels 1..K; a
  // set that does not agree with that numbering is applied by one in-place
  // engine recode of the codes.
  std::string lbl = (lblname.has_value() && !lblname->empty()) ? *lblname : newvar;
  std::map<std::string, std::string> codeMap;  // string-keyed (labelDefs)
  auto existingSet = session.labelDefs.find(lbl);
  if (existingSet != session.labelDefs.end()) codeMap = existingSet->second;
  std::map<std::string, long long> codeOfText;
  long long maxCode = 0;
  for (const auto& [codeStr, text] : codeMap) {
    long long c = 0;
    try {
      c = std::stoll(codeStr);
    } catch (const std::exception&) {
      continue;
    }
    maxCode = std::max(maxCode, c);
    auto ins = codeOfText.emplace(text, c);
    if (!ins.second && c < ins.first->second) ins.first->second = c;
  }
  std::string remapRules;
  for (size_t i = 0; i < sortedLevels.size(); ++i) {
    const long long engineCode = static_cast<long long>(i) + 1;
    long long code = 0;
    auto known = codeOfText.find(sortedLevels[i]);
    if (known != codeOfText.end()) {
      code = known->second;
    } else if (noExtend) {
      // Only rows outside the sample hold this string (checked above), and they
      // are missing: the set is not extended (StataBE 19.5: `label list` after
      // `encode s if x < 4, ... noextend` still has only the set's own codes).
      continue;
    } else {
      code = ++maxCode;
      codeMap[std::to_string(code)] = sortedLevels[i];
    }
    if (code != engineCode)
      remapRules += "(" + std::to_string(engineCode) + "=" + std::to_string(code) + ")";
  }
  if (!remapRules.empty()) {
    std::string remapOut = nextPath(session.workdir(), "encode_codes");
    std::vector<std::string> args = {"recode", "--data", *session.currentHbin(), "--var", newvar,
                                     "--rules", remapRules, "--out", remapOut};
    std::optional<SessionState::ResidentEligible> remapResident;
    if (residentEncode) remapResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(remapOut);
  }
  if (!codeMap.empty() || existingSet != session.labelDefs.end()) session.labelDefs[lbl] = codeMap;
  session.varLblname[newvar] = lbl;
  // Every variable carrying this set sees the appended codes.
  for (const auto& [var, lname] : session.varLblname) {
    if (lname != lbl) continue;
    std::map<int, std::string> numericMap;  // int-keyed (valueLabels)
    for (const auto& [codeStr, text] : codeMap) {
      try {
        numericMap[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    if (numericMap.empty()) session.valueLabels.erase(var);
    else session.valueLabels[var] = numericMap;
  }
  // encode keeps the source's variable label (ah_encode_gen: [Fruit name]).
  {
    auto srcMeta = session.varMeta.find(strvar);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      session.varMeta[newvar].label = srcMeta->second.label;
  }
  // format-display-default-lifecycle: Stata's encode creates a long that
  // displays %8.0g, not long's own %12.0g (w2af_format_lifecycle.log
  // af_encode: `long|%8.0g`, and the same pair after a save/use,
  // af_rt_created). The format is the variable's own, so it is held.
  {
    VarMeta& vm = session.varMeta[newvar];
    vm.stataType = std::string("long");
    vm.format = std::string("%8.0g");
    session.implicitFloatVars.erase(newvar);
  }

  return mutationResult(session, "encode", {newvar});
}

}  // namespace piet::handlers
