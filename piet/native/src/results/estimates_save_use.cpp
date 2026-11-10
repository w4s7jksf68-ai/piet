// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
#include "piet/handlers/estimates_tables.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "stored_estimates_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

namespace {

// ---------------------------------------------------------------------------
// B4-W3b / BUG-470: LastModel <-> JSON, the two halves of `estimates save` and
// `estimates use`.
//
// This pair lives here, in ONE translation unit, on purpose: a writer and a
// reader that drift apart is the [[format-writer-unread-field-trap]] class, and
// the cheapest structural defence is that the only way to add a field is to
// edit both functions inside one screen of each other.
//
// What travels and what does not:
//  * `estHandle` is DELIBERATELY not written. It is contract_A §31's opaque
//    per-PROCESS session-estimation handle ("est:<nonce>:<n>",
//    session_state.h:168-186); the engine's nonce makes a handle from an
//    earlier process resolve nowhere, and restoring one across a save/use would
//    be the single way this could spend model A's handle on model B. Restoring
//    it EMPTY costs one engine refit and cannot be wrong.
//  * `dataPath`/`fitDataPath` are written as they stand. If the file they name
//    is gone by the time the estimates are `use`d, the post-estimation command
//    fails where it always fails — at the engine call, by name — rather than
//    here, silently, against a different dataset.
ojson lastModelToJson(const LastModel& lm) {
  ojson m;
  m["cobra_cmd"] = lm.cobraCmd;
  m["stata_cmd"] = lm.stataCmd;
  m["y"] = lm.y;
  m["x"] = lm.x;
  m["fe"] = lm.fe;
  m["vcov"] = lm.vcov;
  m["cluster"] = lm.cluster;
  if (lm.ivar.has_value()) m["ivar"] = *lm.ivar;
  m["depvar"] = lm.depvar;
  m["data_path"] = lm.dataPath;
  m["fit_data_path"] = lm.fitDataPath;
  m["fit_keepif"] = lm.fitKeepif;
  m["fit_weight"] = lm.fitWeight;
  m["fit_wtype"] = lm.fitWtype;
  // e(wexp) as typed (`= wf `, `= 1/wf`): without it `estimates use` rebuilt it
  // from the weight column (BUG-1215). Written only for a weighted fit.
  if (!lm.fitWexp.empty()) m["fit_wexp"] = lm.fitWexp;
  if (lm.timevar.has_value()) m["timevar"] = *lm.timevar;
  if (lm.gvar.has_value()) m["gvar"] = *lm.gvar;
  if (lm.method.has_value()) m["method"] = *lm.method;
  if (lm.comparison.has_value()) m["comparison"] = *lm.comparison;
  if (lm.pscoretrim.has_value()) m["pscoretrim"] = *lm.pscoretrim;
  if (lm.result.has_value()) m["result"] = *lm.result;
  m["iv_endog"] = lm.ivEndog;
  m["iv_exog"] = lm.ivExog;
  m["iv_instruments"] = lm.ivInstruments;
  return m;
}

std::vector<std::string> jsonStrVec(const ojson& j, const char* key) {
  std::vector<std::string> out;
  if (!j.contains(key) || !j[key].is_array()) return out;
  for (const auto& e : j[key]) {
    if (e.is_string()) out.push_back(e.get<std::string>());
  }
  return out;
}

std::optional<std::string> jsonOptStr(const ojson& j, const char* key) {
  if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
  return j[key].get<std::string>();
}

LastModel lastModelFromJson(const ojson& m) {
  LastModel lm;
  lm.cobraCmd = m.value("cobra_cmd", std::string());
  lm.stataCmd = m.value("stata_cmd", std::string());
  lm.y = m.value("y", std::string());
  lm.x = jsonStrVec(m, "x");
  lm.fe = jsonStrVec(m, "fe");
  lm.vcov = m.value("vcov", std::string("iid"));
  lm.cluster = jsonStrVec(m, "cluster");
  lm.ivar = jsonOptStr(m, "ivar");
  lm.depvar = m.value("depvar", std::string());
  lm.dataPath = m.value("data_path", std::string());
  lm.fitDataPath = m.value("fit_data_path", std::string());
  lm.fitKeepif = m.value("fit_keepif", std::string());
  lm.fitWeight = m.value("fit_weight", std::string());
  lm.fitWtype = m.value("fit_wtype", std::string());
  lm.fitWexp = m.value("fit_wexp", std::string());
  lm.timevar = jsonOptStr(m, "timevar");
  lm.gvar = jsonOptStr(m, "gvar");
  lm.method = jsonOptStr(m, "method");
  lm.comparison = jsonOptStr(m, "comparison");
  lm.pscoretrim = jsonOptStr(m, "pscoretrim");
  if (m.contains("result") && !m["result"].is_null()) lm.result = m["result"];
  lm.ivEndog = jsonStrVec(m, "iv_endog");
  lm.ivExog = jsonStrVec(m, "iv_exog");
  lm.ivInstruments = jsonStrVec(m, "iv_instruments");
  // estHandle stays empty — see lastModelToJson's comment.
  return lm;
}

bool fileExistsEt(const std::string& path) { return cecon::native_exists(path); }

// Stata's `estimates save` appends `.ster` when the name carries no suffix
// (oracle pins_oracle_g.log:284 — `estimates save "b4g_est", replace`
// answers "(note: file b4g_est.ster not found) / file b4g_est.ster saved").
// Only a suffix on the LAST path component counts, so `dir.v2/est` still gets
// one.
std::string withSterSuffix(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (base.find('.') != std::string::npos) return path;
  return path + ".ster";
}

}  // namespace

// ===========================================================================
// B4-W3b / BUG-470: estimates save / estimates use
//
// Stata runs all four links at rc 0 (oracle pins_oracle_g.log:283-298):
//   estimates save "b4g_est", replace -> 0
//   estimates use  "b4g_est"          -> 0
//   e(cmd)==regress, e(depvar)==price   <- the RESTORE puts e() back
//   predict double pxb, xb            -> 0, N 74
// The third line is the one that decides whether this is real: `predict` in a
// session that has run ANY regression answers rc 0 whether or not the restore
// happened, off the still-live model. So `use` restores LastModel wholesale —
// which is what predict/test/lincom/margins all read — and not just a display.

ojson runEstimatesSave(const std::string& fileArg, bool replace, SessionState& session) {
  // BUG-1248: StataBE 19.5 `estimates save a` with no estimates, after `ereturn
  // clear` as well: "last estimates not found" r(301).
  if (!session.lastEstBlock.has_value() && !session.lastModel().has_value()) {
    return ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                  std::nullopt, std::nullopt, "r(301)"));
  }
  std::string fp = withSterSuffix(resolveUserPath(session, fileArg));
  if (fileExistsEt(fp) && !replace) {
    return engineErrorBlockEt("estimates save: file already exists: " + fp +
                              ". Use `, replace` to overwrite.");
  }

  ojson doc;
  doc["_magic"] = "PIETEST1";
  // The e() display block AND the replayable model. Both, because they answer
  // different questions: `est_block` is what `esttab`/`estimates table` render
  // from, `last_model` is what `predict` re-fits from. Writing only the first
  // is exactly the partial `use` BUG-470's reopening condition names.
  doc["est_block"] = session.lastEstBlock.has_value() ? *session.lastEstBlock : ojson(nullptr);
  doc["last_model"] = session.lastModel().has_value() ? lastModelToJson(*session.lastModel())
                                                      : ojson(nullptr);
  doc["added"] = session.estAdded;
  // eb-order T-P3: a posting with no fitted model behind it (estpost) has no
  // LastModel to republish its e() on `use`, so its e() stores travel in the
  // file -- e(cmd)/e(subcmd), e(N), e(mean)/e(sd)/... (w2v_estpost_ematrix.log
  // ep14s_ecmd `estpost|summarize`, ep14s_emean_12). A fit's stores come back
  // from its LastModel, as before.
  if (!session.lastModel().has_value()) {
    ojson strs = ojson::object(), scs = ojson::object(), mats = ojson::object();
    for (const auto& [k, v] : session.eval.eStrings.items()) strs[k] = v;
    for (const auto& [k, v] : session.eval.eScalars.items())
      scs[k] = std::isnan(v) ? ojson(nullptr) : ojson(v);
    for (const auto& [k, m] : session.eval.eMatrices) {
      ojson data = ojson::array();
      for (const auto& row : m.data) {
        ojson r = ojson::array();
        for (double v : row) r.push_back(std::isnan(v) ? ojson(nullptr) : ojson(v));
        data.push_back(r);
      }
      mats[k] = {{"rownames", m.rownames}, {"colnames", m.colnames}, {"data", data}};
    }
    doc["e_posted"] = {{"strings", strs}, {"scalars", scs}, {"matrices", mats}};
  }

  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) return engineErrorBlockEt("estimates save: could not write file: " + fp);
  std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!f) return engineErrorBlockEt("estimates save: could not write file: " + fp);

  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = "file " + fp + " saved";
  return out;
}

ojson runEstimatesUse(const std::string& fileArg, SessionState& session) {
  std::string fp = withSterSuffix(resolveUserPath(session, fileArg));
  std::ifstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) {
    return engineErrorBlockEt("estimates use: file " + fp + " not found");
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  ojson doc;
  try {
    doc = ojson::parse(ss.str());
  } catch (...) {
    doc = ojson(nullptr);
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETEST1") {
    // A REAL Stata `.ster` lands here, and this is the message it must get: the
    // refusal is about the FILE FORMAT, which is a named limit (PIET cannot
    // read Stata's binary estimation format), and not about the command, which
    // works. BUG-470 draws that line explicitly.
    return engineErrorBlockEt(
        "estimates use: " + fp +
        " is not a PIET estimates file. This version of PIET reads only the estimates it "
        "wrote itself (`estimates save`); Stata's own binary .ster format is not supported, "
        "so refusing rather than restoring a model this file does not describe.");
  }

  // Order matters: setLastModel() republishes e(cmd)/e(depvar)/e(vce) and
  // re-points eval.lastModelResult, so it must run before anything reads e().
  if (doc.contains("last_model") && doc["last_model"].is_object()) {
    session.setLastModel(lastModelFromJson(doc["last_model"]));
    // e(sample) T3: results read from a file carry no estimation sample; Stata
    // answers 0 for every row (w2_esample.log:1531 G6 `0|0|0|28|`), and
    // `estimates esample:` may then set one without `replace` (:1542 G7).
    const LastModel& used = *session.lastModel();
    session.setLastModelEsample(LastModel::EsampleState::kNone, 0, std::string(), used.dataPath);
  } else {
    // eb-order T-P3: a file with no fitted model behind it (an estpost
    // posting) replaces the whole e() -- the previous fit's LastModel, its
    // stores and its posted matrices go, so neither `_b[]` nor predict/test
    // can answer from that fit (SPEC C: the stale `last_model` after estpost).
    session.lastModel_reset();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.ePostedLive = doc.contains("est_block") && doc["est_block"].is_object();
    if (doc.contains("e_posted") && doc["e_posted"].is_object()) {
      const ojson& ep = doc["e_posted"];
      if (ep.contains("strings") && ep["strings"].is_object())
        for (const auto& [k, v] : ep["strings"].items())
          if (v.is_string()) session.eval.setEString(k, v.get<std::string>());
      if (ep.contains("scalars") && ep["scalars"].is_object())
        for (const auto& [k, v] : ep["scalars"].items())
          session.eval.setEScalar(k, v.is_number() ? v.get<double>()
                                                   : std::numeric_limits<double>::quiet_NaN());
      if (ep.contains("matrices") && ep["matrices"].is_object()) {
        for (const auto& [k, mj] : ep["matrices"].items()) {
          eval::EvalMatrix em;
          if (mj.contains("rownames") && mj["rownames"].is_array())
            em.rownames = mj["rownames"].get<std::vector<std::string>>();
          if (mj.contains("colnames") && mj["colnames"].is_array())
            em.colnames = mj["colnames"].get<std::vector<std::string>>();
          if (mj.contains("data") && mj["data"].is_array()) {
            for (const auto& row : mj["data"]) {
              std::vector<double> r;
              if (row.is_array())
                for (const auto& c : row)
                  r.push_back(c.is_number() ? c.get<double>()
                                            : std::numeric_limits<double>::quiet_NaN());
              em.data.push_back(std::move(r));
            }
          }
          session.eval.eMatrices.emplace_back(k, std::move(em));
        }
      }
    }
  }
  if (doc.contains("est_block") && doc["est_block"].is_object()) {
    session.lastEstBlock = doc["est_block"];
  } else {
    session.lastEstBlock.reset();
  }
  // The estadd bag belongs to the estimate, so it travels with it — and the
  // fingerprint is recomputed from the RESTORED block rather than carried, so a
  // bag can never be re-attached to an estimate it did not come from.
  session.estAddedClear();
  if (doc.contains("added") && doc["added"].is_object() && session.lastEstBlock.has_value()) {
    session.estAdded = doc["added"];
    session.estAddedFp = SessionState::estAddFingerprint(*session.lastEstBlock);
  }

  std::string depvar = session.lastModel().has_value() ? session.lastModel()->depvar : std::string();
  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = "(estimates restored from " + fp + (depvar.empty() ? "" : ": " + depvar) + ")";
  return out;
}

}  // namespace piet::handlers
