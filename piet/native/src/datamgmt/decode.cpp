// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// decode NUMVAR, generate(NEWVAR): labelled numeric codes -> string.
#include "piet/handlers/data_values.h"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "datamgmt_support.h"
#include "string_conversion_internal.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

// BUG-371: `decode NUMVAR, generate(NEWVAR)` — the inverse of `encode`, and the
// asymmetry was the finding: a user who encoded a string could not get it back.
//
// What `encode` actually leaves behind (runEncode above) is an f64 column of
// 1-based codes PLUS a session-side value-label map — not a factor column. So
// decode is not a dtype relabel: it reads session.valueLabels[var], maps each
// row's code to its label, and writes a NEW factor column. A code with no label
// becomes missing, which is what Stata does.
ojson runDecode(const std::string& varname, const std::optional<std::string>& genVar,
                SessionState& session, size_t maxLength) {
  if (!session.hasData()) return eval::noDataLoaded();
  if (!genVar.has_value() || genVar->empty())
    return engineErrorBlock("decode: generate(newvar) is required", "syntax_error");
  const std::string target = trim(*genVar);
  if (!strutil::isValidStataIdentifier(target))
    return engineErrorBlock(
        "decode: invalid variable name `" + target + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(varname))
    return engineErrorBlock("variable " + varname + " not found", "unknown_variable");
  if (ds->has(target))
    return engineErrorBlock("decode: variable " + target + " already defined", "bad_input");
  // Stata's rc for both refusals (w2ah_data_verbs.log ah_decode_string: 108,
  // ah_decode_nolab: 182; main answered rc 1 to each).
  if (ds->dtype(varname) != "f64")
    return rcErrorBlock(varname + ": not possible with string variable", 108);

  // "Not labeled" is about the attachment: a variable carrying a value-label
  // name decodes (an unmapped code to ""), one carrying none is r(182).
  if (session.varLblname.find(varname) == session.varLblname.end())
    return rcErrorBlock(varname + " not labeled", 182);
  // An attached set that is not defined (dropped, or never defined) is
  // r(111) "value label X not found" (w2ao_label_family lb_val_undef, lb_drop
  // `dec=111`).
  {
    const std::string& lbl = session.varLblname.at(varname);
    if (session.labelDefs.find(lbl) == session.labelDefs.end())
      return rcErrorBlock("value label " + lbl + " not found", 111);
  }
  // BUG-1120: only then the generate() name, by encode's rule: "_cons invalid
  // varname", "int invalid varname", r(198) (`decode s, gen(_cons)' with s string
  // is still r(108), `decode id, gen(_rc)' with id unlabeled r(182); StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(target, /*resultRows=*/true))
    return rcErrorBlock(r->message, r->rc);
  static const std::map<int, std::string> kNoLabels;
  auto lblIt = session.valueLabels.find(varname);
  const std::map<int, std::string>& labels =
      lblIt == session.valueLabels.end() ? kNoLabels : lblIt->second;

  const int64_t n = ds->nRows();
  const double* src = ds->col(varname).f64();
  std::vector<std::string> out(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    const double v = src[r];
    if (std::isnan(v) || v != std::floor(v)) continue;  // stays "" == factor-missing
    auto it = labels.find(static_cast<int>(v));
    if (it == labels.end()) continue;  // an unlabeled code decodes to missing, matching Stata
    // BUG-1096: maxlength(#) counts bytes but never cuts inside a character; it
    // keeps the whole characters that fit (StataBE 19.5, labels "héllo" and
    // "日本語です": maxlength(4) gives "hél" and "日", maxlength(2) "h" and "").
    // A cut mid-sequence wrote invalid UTF-8 the snapshot writer then refused.
    std::string text = it->second;
    if (text.size() > maxLength) {
      size_t cut = maxLength;
      while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
      text.resize(cut);
    }
    out[static_cast<size_t>(r)] = std::move(text);
  }

  // Same one-column setcol shape runTostring uses — see its comments for the
  // resident-plane reasoning and the BUG-124 flush guard.
  std::string valuesPath = nextPath(session.workdir(), "decode") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("decode: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) csv << csvEscape(out[static_cast<size_t>(r)]) << "\n";
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("decode: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::string outPath = nextPath(session.workdir(), "decode");
  std::string dataPath = residentSetcol ? *session.currentHbin() : ds->path();
  std::vector<std::string> setArgs = {"setcol",   "--data",   dataPath, "--var",   target,
                                      "--values", valuesPath, "--dtype", "factor", "--new",
                                      "--out",    outPath};
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("decode: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // decode's new variable is a str# as wide as the longest label of the attached
  // set in bytes, used or not, and at most maxlength() (at least 1), with that
  // type's default format, and it keeps the source's variable label
  // (w2ah_data_verbs.log ah_decode_gen: str17 %17s [Region code]; ah_decode_short:
  // str2 %9s). Main recorded no type, so `: type' failed and the format read %9s.
  // StataBE 19.5, set {"ab", "c", unused "a very long unused label"}: str24, and
  // str10 under maxlength(10); "héllo"/"日本語です" under maxlength(2): str2 though
  // the longest kept text is "h". Past 2045 bytes it is strL, as every str# is: a
  // 2048-byte label (used or not) makes strL, and so does maxlength(2046) over a
  // longer one, while maxlength(2045) makes str2045.
  size_t width = 1;
  for (const auto& [code, text] : labels) width = std::max(width, text.size());
  width = std::min(width, std::max<size_t>(maxLength, 1));
  {
    VarMeta& vm = session.varMeta[target];
    vm.stataType = width > 2045 ? std::string("strL") : "str" + std::to_string(width);
    auto srcMeta = session.varMeta.find(varname);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      vm.label = srcMeta->second.label;
  }
  session.implicitFloatVars.erase(target);
  return mutationResult(session, "decode", {target});
}

}  // namespace piet::handlers
