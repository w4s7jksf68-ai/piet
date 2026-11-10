// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session fit state. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

void SessionState::attachFitCovariance(const nlohmann::ordered_json& raw) {
    if (!lastEstBlock.has_value() || !lastEstBlock->is_object() || !raw.is_object()) return;
    const auto it = raw.find("e_V");
    if (it == raw.end() || !it->is_object()) return;
    (*lastEstBlock)["e_V"] = *it;
  }

void SessionState::lastModel_reset() {
    lastModel_.reset();
    lastEstBlock.reset();
    eval.lastModelResult.reset();
    // AER-SCOPE1 bundle E: the estadd bag is keyed to e(); when e() is reset
    // the bag has nothing to belong to. The fingerprint check in `runEststo`
    // is the OTHER half of this (it catches a REPLACED e(), which never comes
    // through here), not a substitute for it.
    estAddedClear();
    activeEstimatesName.clear();  // W2 esttabnames: e(_estimates_name) goes with e()
    eOnFitNames.clear();  // W2D-T01 round 3: every caller has emptied the e() stores
    ePostedEsampleSerial = 0;  // a posted e(sample) goes with the posted e()
    eNameContract.reset();  // the published name lists go with e()
  }

void SessionState::setLastModelEsample(LastModel::EsampleState state, unsigned long long serial,
                           std::string installPath, std::string dataPath) {
    if (!lastModel_.has_value()) return;
    lastModel_->esample = state;
    lastModel_->esampleSerial = serial;
    lastModel_->esampleInstallPath = std::move(installPath);
    lastModel_->dataPath = std::move(dataPath);
  }

void SessionState::pinPreInstallHead() {
    if (currentHbin_.has_value() && !currentHbin_->empty()) {
      preInstallPins_.push_back(*currentHbin_);
      preInstallLoadPending_ = true;
    }
  }

void SessionState::releasePreInstallPins() {
    if (preInstallPins_.empty()) return;
    std::vector<std::string> pins;
    pins.swap(preInstallPins_);
    for (const auto& p : pins) {
      if (currentHbin_.has_value() && *currentHbin_ == p) continue;  // the head again
      reclaimSupersededScratch(p);
    }
  }

void SessionState::dropEOnFit() {
    for (const auto& n : eOnFitNames) {
      eval.eStrings.erase(n);
      eval.eScalars.erase(n);
      eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                          [&](const auto& p) { return p.first == n; }),
                           eval.eMatrices.end());
    }
    eOnFitNames.clear();
  }

void SessionState::supersedePostedE() {
    if (!ePostedLive) return;
    eval.eScalars = eval::OrderedFloatDict();
    eval.eStrings.clear();
    eval.eMatrices.clear();
    ePostedLive = false;
    ePostedEsampleSerial = 0;
  }

void SessionState::setLastModel(LastModel m) {
    if (m.fitWexp.empty() && !m.fitWeight.empty()) m.fitWexp = commandWexp;
    supersedePostedE();  // BUG-733: a posted e() ends here (see above)
    dropEOnFit();        // round 3: so do keys written on top of the previous fit
    ++lastModelInstalls;
    // eb-order T-P3 part 0: a NEW e() (another fit serial, or none live) owns
    // no estimation block and no e(b)/e(V) until it writes its own. Every fit
    // that has an e(b) writes its block AFTER this call (csdid, oaxaca and
    // teffects included) and posts its e(b)/e(V) after it, so a fit that has
    // none (pca, rdrobust, rddensity) leaves no block and the previous fit's
    // can never answer for it. A copy of the live model keeps both.
    const bool sameFit = lastModel_.has_value() && lastModel_->fitSerial == m.fitSerial;
    if (!sameFit) {
      // A new fit replaces e() whole, so what `estadd` attached to the previous
      // e() goes with it -- also when the new fit repeats the same specification,
      // which the bag's fingerprint (depvar|estimator|N|k) cannot tell apart
      // (w2ak_estout_family: `estadd r(mean)` at do:191, then `regress y x1 x2`
      // at :221 left e(mean) live). estadd mirrored the additions into the e()
      // stores (mirrorIntoEStores), so they leave those too. A re-install of the
      // SAME fit (test, lincom) keeps them.
      dropEstAddedMirrors();
      estAddedClear();
      lastEstBlock.reset();
      eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                          [](const auto& p) { return p.first == "b" || p.first == "V"; }),
                           eval.eMatrices.end());
    }
    // W2 esttabnames: a new e() is not a stored one. `estimates restore` sets
    // the name again AFTER calling here (estimates.ado Store/Restore keep the
    // hidden e(_estimates_name) on the results, so a fresh fit has none).
    // W2AK: a re-install of the SAME fit (a post-estimation command such as
    // `test` copying the live model back) keeps it -- Stata's e() is untouched
    // there, so a later `estadd` still reaches the stored copy (w2ak ek_d01).
    if (!lastModel_.has_value() || lastModel_->fitSerial != m.fitSerial) {
      activeEstimatesName.clear();
    }
    lastModel_ = std::move(m);
    // BUG-518 (W2 predictframe, 2026-09-19): the frame a model was FIT on must
    // exist ON DISK for as long as the model is live. Every fit handler records
    // `lm.dataPath = *session.currentHbin()`, and after a resident `generate`
    // that path is a dirty VIRTUAL head -- the fit read it straight from the
    // worker's frame and nothing ever wrote it. The next resident mutation then
    // advances the worker's frame in place, and `test`, `lincom`, `predict`
    // (which replay `--data`/`--fit-data lm.dataPath`) all fail with
    // `[data_io] cannot open .../bysort_gen_*.hbin` (measured on the frozen main
    // binary: `regress y x2 t` / `generate foo = 1` / `test x2` -> rc 1) where
    // Stata is rc 0 (w2_smalls.log section C's shape with the `generate` moved
    // in front of the first predict; BUG-518.do). reclaimSupersededScratch's
    // exclusion (7) already keeps the file ALIVE; this is the other half, the
    // one that makes it EXIST. A no-op unless the head is that dirty virtual
    // frame (a fit on a clean head, a `dataOverride` slice, an `estimates
    // restore` of an older path all fall through), so it costs exactly one
    // snapshot per estimation on a dirty head -- the flush a later `predict` /
    // `sort` / `save` would have paid as "stateless-data" anyway, moved
    // forward to the moment the model starts naming the frame. Tagged for the
    // PIET_ROUTELOG census so the added flushes can be counted by site.
    if (residentDirty_ && !lastModel_->dataPath.empty() &&
        lastModel_->dataPath == residentToken_) {
      ensureMaterializedHead("lastmodel-pin");
    }
    eval.lastModelResult = lastModel_->result;
    // BUG-458: e(cmd)/e(depvar)/e(vce) are PUBLISHED into the e-macro store
    // here, the one point every estimation handler already passes through, so
    // the delimited form `e(cmd)', the bare form e(cmd) (BUG-456) and `ereturn
    // list` (which now reads the store) cannot disagree. Before this, only
    // pca/oaxaca/estadd wrote the store and `e(cmd)' was EMPTY after regress
    // while `ereturn list` printed "feols" (internal notes bug456-20260908 E0/E1).
    // An empty depvar (pca) ERASES the macro rather than publishing "".
    eval.setEString("cmd", lastModel_->stataCmd.empty() ? lastModel_->cobraCmd
                                                        : lastModel_->stataCmd);
    eval.setEString("depvar", lastModel_->depvar);
    // W2 estimopts (BUG-536/537/538), the e() macros a table writer reads
    // after a robust/clustered/weighted fit, published in STATA's spellings.
    // lm.vcov keeps the engine's ("hc1"/"cluster"/"twoway"/"multiway") because the
    // replay machinery keys on it; only the macro store translates.
    // w2_pweight.log PSTR records: R2 `robust|Robust||pweight|= w`,
    // R4/X1 `cluster|Robust|id|...`, R1 `...|aweight|= w`. Anything else
    // (iid and every non-linear spelling) passes through unchanged in e(vce).
    //
    // e(clustvar) is published for EVERY fit: the cluster names under a
    // cluster VCE, erased (setEString("") erases) otherwise. Nothing else in
    // the tree writes it, and no fit clusters under a non-engine spelling
    // (nbreg records `--vce cluster` as "cluster"), so a later robust nbreg,
    // pca, oaxaca or teffects cannot inherit a clustered fit's variable.
    //
    // e(vcetype): the engine's spellings answer here ("Robust" / erased).
    // Of the non-engine spellings, only teffects (stataCmd "teffects",
    // lm.vcov "robust") publishes its own -- `AI robust`,
    // aer_estim2_oracle.log INFO|nn|e(vcetype) -- BEFORE calling here, so it
    // is left untouched. Every other one (nbreg's engine `--vce robust`,
    // oaxaca "robust", pca "none") is ERASED: none of them has a captured
    // e(vcetype) arm, and erased is what this store answered for them before
    // e(vcetype) was published at all, where keeping the previous fit's
    // `Robust` would be a stale macro nobody computed for this fit.
    {
      const std::string& v = lastModel_->vcov;
      const bool robustLike = (v == "hc1");
      const bool clusterLike = (v == "cluster" || v == "twoway" || v == "multiway");
      const bool engineSpelling = v.empty() || v == "iid" || robustLike || clusterLike;
      eval.setEString("vce", lastModel_->eVce.has_value()
                                 ? *lastModel_->eVce
                                 : robustLike ? std::string("robust")
                                              : clusterLike ? std::string("cluster") : v);
      if (lastModel_->eVce.has_value() && *lastModel_->eVce == "linearized") {
        // a `svy:` fit (cacheLastModelM4): StataBE 19.5 `svy: regress` e(vce)
        // linearized, e(vcetype) Linearized (BUG-1513)
        eval.setEString("vcetype", "Linearized");
      } else if (engineSpelling) {
        eval.setEString("vcetype", (robustLike || clusterLike) ? "Robust" : "");
      } else if (lastModel_->stataCmd != "teffects") {
        eval.setEString("vcetype", "");
      }
      std::string clustvar;
      if (clusterLike) {
        for (const auto& c : lastModel_->cluster) {
          if (!clustvar.empty()) clustvar += ' ';
          clustvar += c;
        }
        // cobra's two-way and multiway spellings are one comma-joined "A,B[,C...]"
        // token; Stata's e(clustvar) is "A B [C...]".
        for (auto& ch : clustvar) {
          if (ch == ',') ch = ' ';
        }
      }
      eval.setEString("clustvar", clustvar);
      const std::string& wt = lastModel_->fitWtype;
      const std::string wtype = wt == "aw"   ? "aweight"
                                : wt == "fw" ? "fweight"
                                : wt == "iw" ? "iweight"
                                : wt == "pw" ? "pweight"
                                             : std::string();
      eval.setEString("wtype", wtype);
      // The ivreg2-family ados store "=" + the expression with no blank after
      // the equals sign (StataBE 19.5, BUG-1530: ivreg2/xtivreg2/ivreghdfe
      // `[aw=wf]` and `[aw = wf]` -> "=wf", `[aw=wf ]` -> "=wf "), where regress,
      // reghdfe, ivregress and areg store "= wf". A first stage stored by
      // savefirst (cobraCmd feols) is left as it was.
      const std::string& sc = lastModel_->stataCmd;
      const bool ivreg2Family =
          (sc == "ivreg2" || sc == "xtivreg2" || sc == "ivreghdfe") && lastModel_->cobraCmd != "feols";
      eval.setEString("wexp", wtype.empty() ? std::string()
                                             : (ivreg2Family ? "=" : "= ") +
                                                   (lastModel_->fitWexp.empty()
                                                        ? lastModel_->fitWeight
                                                        : lastModel_->fitWexp));
      // reghdfe's e(resid) names the residual column of ONE fit (BUG-539);
      // runRegress re-posts it after this call when the option was given. A
      // re-install of the SAME fit (setEstHandleM4, reached by lincom/test)
      // keeps it (w2n_followups.log:673/:682/:741 D02/D03/D06: e(resid) is
      // still `_reghdfe_resid` / `myres` after `quietly lincom x1`).
      if (!sameFit) eval.setEString("resid", "");
    }
  }

}
