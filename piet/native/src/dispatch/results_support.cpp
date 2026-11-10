// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "results_support.h"
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {


// e(sample) T4 (w2_esample.log:1692 H1 `0|60|60|`, :1725 H2 `0|47|47|`): an ML
// fit installed its model (estimation/panel_estimation.cpp cacheLastModelM4) with no mask;
// esampleInstallMlMask writes it on the head and only the e(sample) fields of
// the live model move (setLastModelEsample) -- a second setLastModel would drop
// the e() keys the handler posted after its own (e(chi2type), e(rules)). A mask
// it cannot vouch for stays the named r(198) of kUnavailable.
// BUG-1213: a store that wrote `_est_NAME` (estimates.ado's esample variable)
// changed the variable list, so the data_mutation block carries the new list to
// the variables panel. Stata prints nothing about the variable; neither does it.
void pushEstStoreMutation(const ojson& stored, ojson& blocks, SessionState& session) {
  if (!stored.is_object() || stored.value("type", std::string()) == "error") return;
  const std::string& name = session.activeEstimatesName;
  const auto it = session.estimatesModels.find(name);
  if (name.empty() || it == session.estimatesModels.end() || !it->second ||
      !it->second->esampleVar)
    return;
  blocks.push_back(handlers::mutationResult(session, "estimates_store", {"_est_" + name}));
}


void installMlEsampleAfterFit(const ojson& block, SessionState& session) {
  if (block.value("type", "") == "estimation" && session.lastModel().has_value() &&
      session.lastModel()->esample == LastModel::EsampleState::kUnavailable &&
      session.lastModel()->result.has_value()) {
    LastModel lm = *session.lastModel();
    ojson raw = *lm.result;
    if (handlers::esampleInstallMlMask(session, lm, raw)) {
      session.setLastModelEsample(lm.esample, lm.esampleSerial, lm.esampleInstallPath,
                                  lm.dataPath);
    }
  }
}


// Replace-or-append a user matrix (tabulate's matcell()/matrow()/matcol()).
// The same three lines are file-local twice already (matrix/variables_to_matrix.cpp
// storeMatrix, matrix/matrix_expression.cpp storeMatrixResult); neither is reachable from
// here. Replace, never append: findMatrix() answers the FIRST match. A scalar
// of the same name is destroyed: scalars and matrices share one namespace ([P]
// matrix define, "Namespace").
void storeUserMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) {
      p.second = std::move(mat);
      return;
    }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}




// `title("A title")` arrives with its quote characters still on the value (the
// tokenizer keeps them, exactly as the file-part handling has to strip them
// too). One matching pair, so a title that is itself quoted survives.
std::string dequoteEstTitle(std::string t) {
  if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                        (t.front() == '\'' && t.back() == '\''))) {
    t = t.substr(1, t.size() - 2);
  }
  return t;
}


// esample T0 (BUG-636): the post-estimation verbs that re-fit the ACTIVE model
// from its data file -- test/testparm/lincom/nlcom (runPostEstimationM4's
// lastModelBaseArgv fallback, their only path) -- replay `fitDataPath` else
// `dataPath` as `--data`. In-session those files are pinned (the live model by
// reclaimSupersededScratch exclusion (7), a stored one by pinStoredModel), but
// `estimates use` reads back the path the .ster recorded at `estimates save`,
// and nothing pins that: a later fit plus a few mutations in the SAME session
// reclaim it, and a .ster saved by another session names that session's
// scratch -- as does the same model after `estimates store` + `estimates
// restore`. The verbs then failed with
// `[data_io] cannot open .../piet_native_XXXX/...hbin` at rc 1. predict already
// refuses this shape by name (postestimation/prediction.cpp, the BUG-518 remainder); this
// is its sibling for the verbs whose ONLY path is that replay. NOT margins:
// `margins, dydx()` after a linear fit answers from the coefficient table
// (buildLinearAmeRawM4) without the file, so a guard here would refuse a
// working form; its replay branches need the check inside runMargins. It fires only
// when the replay can NOT work: no engine handle to answer from, the file is
// not the live head (a dirty resident head is legitimately not on disk) and not
// a virtual save, and nothing exists at the path.
std::optional<ojson> refuseIfFitDataGone(const std::string& verb, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !lmOpt->estHandle.empty()) return std::nullopt;
  const LastModel& lm = *lmOpt;
  const std::string& fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (fitFile.empty() || session.isVirtualSave(fitFile)) return std::nullopt;
  if (session.currentHbin().has_value() && *session.currentHbin() == fitFile) return std::nullopt;
  std::error_code ec;
  if (std::filesystem::exists(cecon::to_native_path(fitFile), ec)) return std::nullopt;
  return ojson(eval::engineError(
      verb + ": the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
          "` was fitted on are no longer available to re-fit from in this version of PIET "
          "(the working frame it was fitted on has been released -- e.g. results loaded back "
          "by `estimates use`). Re-run the estimation command, then " +
          verb + ".",
      "unsupported_option"));
}


std::optional<ojson> readEststoStoreOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, EststoStoreOpts* out) {
  // NOT kVocabEststoStore.known: that slot is the UNION with `estimates`' own
  // vocabulary (b/se/stats/replace), none of which `eststo` takes.
  static const std::unordered_set<std::string> kRead = {"title", "prefix"};
  if (auto err = eval::rejectUnknownOptions(opts, kRead, kVocabEststoStore.unimplemented)) {
    return ojson(*err);
  }
  for (const auto& [k, v] : opts) {
    if (k == "title" && !v.isBool) out->title = dequoteEstTitle(v.strValue);
    if (k == "prefix") {
      // eststo.ado declares Prefix(name): one Stata name, nothing else. A bare
      // `prefix` or a non-name would otherwise mint a key no `esttab` can spell.
      // The one name rule (core/ident.h): `prefix(نموذج)` is a name, `prefix(١m)`
      // is not.
      const std::string p = v.isBool ? std::string() : trimWs(v.strValue);
      if (!strutil::isValidStataIdentifier(p)) {
        return ojson(eval::engineError(
            "eststo: option prefix() takes one name, e.g. `eststo, prefix(m): regress y x`",
            "syntax_error"));
      }
      out->autoPrefix = p;
    }
  }
  return std::nullopt;
}


// The colon that ends an `eststo [name] [, options] :` head: the first one that
// is outside a quoted span (w2_eststo_prefix.log A6 -- `eststo t1, title("a: b"):
// regress y w` is rc 0 and stores t1) AND outside a `...' macro reference, so an
// inline extended function in the name (`eststo m`: word 1 of `l'': ...`) does
// not split the line inside the macro. npos when there is none -- which is what
// keeps the flat `eststo m1, title("a: b")` flat. A colon BEHIND a trailing
// ` // comment` is not one either: tokenize() strips that comment (`\s+//.*$`)
// for a line that reaches it raw, so `eststo m1 // note: keep` is the flat store
// it has always been, not a prefix with the head `m1 // note`.
size_t findEststoPrefixColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int tick = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (quoted[i]) continue;
    const char ch = s[i];
    if (ch == '/' && tick == 0 && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t') &&
        i + 1 < s.size() && s[i + 1] == '/') {
      return std::string::npos;
    }
    if (ch == '`') {
      ++tick;
    } else if (ch == '\'' && tick > 0) {
      --tick;
    } else if (ch == ':' && tick == 0) {
      return i;
    }
  }
  return std::string::npos;
}
}  // namespace piet::dispatch_detail
