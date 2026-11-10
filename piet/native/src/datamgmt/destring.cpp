// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// destring: string -> numeric conversion (Stata real() reader, ignore/force/
// percent/dpcomma, storage-type choice).
#include "piet/handlers/data_values.h"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "datamgmt_support.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule
#include "string_conversion_internal.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;
using string_conversion_detail::destringNoteBlock;
using string_conversion_detail::placeGeneratedAfterSource;

namespace {
using storage::stataDefaultFormat;
}  // namespace

// ---------------------------------------------------------------------------
// destring VAR, {replace | gen(NEWVAR)} [ignore("chars")] [force] [float]
// [percent] [dpcomma] -- see handlers.h. Behaviour per [D] destring (and the
// StataBE 19.5 measurements).

namespace {

// Reads a number the way Stata's real() does: an optional sign, digits with an
// optional decimal point, and an optional exponent introduced by e/E/d/D, or the
// %21x hexadecimal form. Overflow, and any magnitude not below 2^1023 (beyond
// Stata's largest double), is unreadable; underflow reads as 0.
std::optional<double> destringReadNumber(const std::string& text) {
  if (text.empty()) return std::nullopt;
  size_t pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = text[pos] == '-';
    ++pos;
  }
  auto isDec = [](char c) { return c >= '0' && c <= '9'; };
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  constexpr double kLimit = 8.98846567431158e307;  // 2^1023
  auto finish = [&](double magnitude) -> std::optional<double> {
    if (!std::isfinite(magnitude) || magnitude >= kLimit) return std::nullopt;
    if (magnitude == 0.0) return 0.0;  // -0 reads as 0
    return negative ? -magnitude : magnitude;
  };

  // Hexadecimal form, recognised by its x/X marker: `1.` (or `0.` with all-zero
  // digits for zero, as %21x prints it), one to thirteen hexadecimal digits, the
  // marker, a required sign and one to three hexadecimal exponent digits; the
  // exponent runs from -3ff (which is zero) to +3fe. StataBE,
  // oracle/cr_destring_probe3 and cr_destring_probe4.
  const size_t xAt = text.find_first_of("xX", pos);
  if (xAt != std::string::npos) {
    if (xAt < pos + 3 || xAt > pos + 15 || text[pos + 1] != '.') return std::nullopt;
    const char lead = text[pos];
    if (lead != '0' && lead != '1') return std::nullopt;
    double fraction = 0.0, scale = 1.0;
    for (size_t i = pos + 2; i < xAt; ++i) {
      const int h = hexVal(text[i]);
      if (h < 0) return std::nullopt;
      scale /= 16.0;
      fraction += h * scale;
    }
    size_t e = xAt + 1;
    if (e >= text.size() || (text[e] != '+' && text[e] != '-')) return std::nullopt;
    const bool expNeg = text[e] == '-';
    ++e;
    if (e >= text.size() || text.size() - e > 3) return std::nullopt;
    int expo = 0;
    for (; e < text.size(); ++e) {
      const int h = hexVal(text[e]);
      if (h < 0) return std::nullopt;
      expo = expo * 16 + h;
    }
    if (expNeg) expo = -expo;
    if (expo > 0x3fe || expo < -0x3ff) return std::nullopt;
    if (lead == '0') {
      if (fraction != 0.0 || expo != -0x3ff) return std::nullopt;
      return 0.0;
    }
    if (expo == -0x3ff) return 0.0;
    return finish(std::ldexp(1.0 + fraction, expo));
  }

  // Decimal form; validated here, converted by strtod in the C locale.
  std::string cleaned;
  cleaned.reserve(text.size() - pos);
  bool mantDigit = false, sawPoint = false;
  size_t i = pos;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (isDec(c)) {
      mantDigit = true;
    } else if (c == '.' && !sawPoint) {
      sawPoint = true;
    } else {
      break;
    }
    cleaned.push_back(c);
  }
  if (!mantDigit) return std::nullopt;
  if (i < text.size()) {
    const char c = text[i];
    if (c != 'e' && c != 'E' && c != 'd' && c != 'D') return std::nullopt;
    cleaned.push_back('e');
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) cleaned.push_back(text[i++]);
    bool expDigit = false;
    for (; i < text.size(); ++i) {
      if (!isDec(text[i])) return std::nullopt;
      expDigit = true;
      cleaned.push_back(text[i]);
    }
    if (!expDigit) return std::nullopt;
  }
  char* end = nullptr;
  const double magnitude = std::strtod(cleaned.c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  return finish(magnitude);  // overflow gives inf (refused); underflow gives 0
}

// `.`, `.a`-`.z` (and the empty string) are missing values, never unreadable.
bool destringIsMissingToken(const std::string& t) {
  if (t.empty() || t == ".") return true;
  return t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z';
}

// dpcomma: a period anywhere makes the value unreadable; the first comma is
// the decimal point, unless the value is a lone comma or some comma is
// directly followed by a lowercase letter (then nothing is converted and the
// value stays unreadable).
std::optional<std::string> destringApplyDpcomma(const std::string& t) {
  if (t.find('.') != std::string::npos) return std::nullopt;
  if (t == ",") return t;
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i] == ',' && t[i + 1] >= 'a' && t[i + 1] <= 'z') return t;
  }
  std::string out = t;
  const size_t comma = out.find(',');
  if (comma != std::string::npos) out[comma] = '.';
  return out;
}

enum class DestringRead { Number, Missing, Unreadable };

// One observation's text -> number / missing / unreadable, after ignore(),
// trimming, the missing tokens and dpcomma ([D] destring, Options and
// Description). `sawPercent` is set when a `%` was removed.
DestringRead destringReadCell(const std::string& raw, const std::array<bool, 256>& ignoreSet,
                              bool anyIgnore, bool dpcomma, double& value, bool& sawPercent) {
  std::string t;
  if (anyIgnore) {
    t.reserve(raw.size());
    for (char c : raw) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (ignoreSet[u]) {
        if (c == '%') sawPercent = true;
        continue;
      }
      t.push_back(c);
    }
  } else {
    t = raw;
  }
  size_t a = 0, b = t.size();
  while (a < b && (t[a] == ' ' || t[a] == '\t')) ++a;
  while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t')) --b;
  t = t.substr(a, b - a);
  if (destringIsMissingToken(t)) return DestringRead::Missing;
  if (dpcomma) {
    std::optional<std::string> conv = destringApplyDpcomma(t);
    if (!conv.has_value()) return DestringRead::Unreadable;
    t = std::move(*conv);
  }
  std::optional<double> v = destringReadNumber(t);
  if (!v.has_value()) return DestringRead::Unreadable;
  value = *v;
  return DestringRead::Number;
}

// float(): the nearest single; nullopt from 2^127 up, where Stata's float range
// ends (StataBE: under `float`, 1.7e38 reads, while 1.8e38 and 2e38% are
// unreadable, or missing under `force`).
std::optional<double> toSingle(double v) {
  if (std::fabs(v) >= 0x1p127) return std::nullopt;
  const double f = static_cast<double>(static_cast<float>(v));
  if (std::fabs(f) >= 0x1p127) return std::nullopt;
  return f;
}

// [D] destring creates double (float under `float`) and then compresses;
// [D] compress demotes to the smallest integer type that holds every value
// (ranges per [D] Data types), never double->float and never float->long.
std::string destringStorageType(const std::vector<double>& vals, bool asFloat) {
  const std::string created = asFloat ? "float" : "double";
  if (vals.empty()) return created;  // an empty variable is not compressed
  bool any = false, integral = true;
  double lo = 0, hi = 0;
  for (double v : vals) {
    if (std::isnan(v)) continue;
    if (v != std::trunc(v)) integral = false;
    if (!any) {
      lo = hi = v;
      any = true;
    } else {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  if (!any) return "byte";
  if (!integral) return created;
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (asFloat) return "float";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return "double";
}

}  // namespace

ojson runDestring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ignoreChars, bool force,
                  SessionState& session, const DestringOpts& opts) {
  if (!session.hasData()) return eval::noDataLoaded();

  // Names and types come from the resident card when one is current (it is
  // authoritative for absence as well); the datastore is opened only when
  // there is no card, or when the row strings are not cached.
  Datastore* ds = nullptr;
  auto openDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto dtypeOf = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card)
        if (vi.name == name) return vi.dtype;
      return std::nullopt;
    }
    Datastore* d = openDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };

  const std::optional<std::string> srcType = dtypeOf(varname);
  if (!srcType.has_value())
    return engineErrorBlock("destring: variable '" + varname + "' not found");
  if (doReplace == genVar.has_value())
    return engineErrorBlock("destring: specify exactly one of `replace` or `generate(newvar)`",
                            "syntax_error");

  const std::string target = doReplace ? varname : trim(*genVar);
  if (!doReplace) {
    const bool reserved = strutil::isReservedVarName(target);
    if (reserved || target == "_skip" || target == "_all")
      return rcErrorBlock("generate() contains existing variable(s) and/or illegal variable name(s)",
                          reserved ? 198 : 101);
    if (dtypeOf(target).has_value())
      return engineErrorBlock("destring: variable '" + target + "' already defined");
    if (!strutil::isValidStataIdentifier(target))
      return engineErrorBlock("destring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
  }
  const std::string op = doReplace ? "replace" : "generate";
  if (*srcType == "f64") return destringNoteBlock(varname + " already numeric; no " + op);

  // The effective ignore set: ignore()'s bytes, plus `%` under percent.
  std::array<bool, 256> ignoreSet{};
  bool anyIgnore = false;
  if (ignoreChars.has_value()) {
    for (char c : *ignoreChars) {
      ignoreSet[static_cast<unsigned char>(c)] = true;
      anyIgnore = true;
    }
  }
  if (opts.percent) {
    ignoreSet[static_cast<unsigned char>('%')] = true;
    anyIgnore = true;
  }

  // The source's row strings: the cached committed column when it is current,
  // else the datastore's codes and level table.
  int64_t n = 0;
  const std::vector<std::string>* cachedStrs = nullptr;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* levels = nullptr;
  if (const auto* cc = session.residentComputedCol(varname); cc != nullptr && cc->dtype == "factor") {
    n = cc->nRows;
    cachedStrs = &cc->strs;
  } else {
    Datastore* d = openDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    const cecon::Column& col = d->col(varname);
    codes = col.codes();
    levels = &col.levels;
  }
  static const std::string kEmpty;
  auto rowText = [&](int64_t r) -> const std::string& {
    if (cachedStrs != nullptr) return (*cachedStrs)[static_cast<size_t>(r)];
    const int32_t code = codes[r];
    if (code < 0 || static_cast<size_t>(code) >= levels->size()) return kEmpty;
    return (*levels)[static_cast<size_t>(code)];
  };

  const double kMissing = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> values(static_cast<size_t>(n), kMissing);
  bool sawPercent = false;
  for (int64_t r = 0; r < n; ++r) {
    double v = 0;
    DestringRead got =
        destringReadCell(rowText(r), ignoreSet, anyIgnore, opts.dpcomma, v, sawPercent);
    if (got == DestringRead::Number && opts.asFloat) {
      const std::optional<double> single = toSingle(v);
      if (single.has_value()) v = *single;
      else got = DestringRead::Unreadable;
    }
    if (got == DestringRead::Number) {
      values[static_cast<size_t>(r)] = v;
    } else if (got == DestringRead::Unreadable && !force) {
      return destringNoteBlock(varname +
                               (anyIgnore ? ": contains characters not specified in ignore(); no "
                                          : ": contains nonnumeric characters; no ") +
                               op);
    }
  }
  if (opts.percent && sawPercent) {
    for (double& v : values) {
      if (std::isnan(v)) continue;
      v = v / 100.0;
      if (opts.asFloat) v = toSingle(v).value_or(kMissing);
    }
  }
  const std::string storage = destringStorageType(values, opts.asFloat);

  // The engine's single-column write: one line per row, shortest round-trip
  // text, an empty line for missing.
  std::string payloadPath = nextPath(session.workdir(), "destring") + ".csv";
  {
    std::ofstream payload(cecon::to_native_path(payloadPath), std::ios::binary);
    if (!payload) return engineErrorBlock("destring: could not open scratch CSV for write");
    for (double v : values) {
      if (!std::isnan(v)) payload << eval::pythonFloatRepr(v);
      payload << '\n';
    }
    payload.flush();
    if (!payload) {
      cecon::native_unlink(payloadPath);
      return engineErrorBlock("destring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(cecon::to_native_path(payloadPath), ec);
    counters::addIpcBytes(ec ? 0 : static_cast<int64_t>(bytes), /*setcolShaped=*/true);
  }
  const bool resident =
      std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr && !residentReadDisabled();
  std::string dataPath;
  if (resident) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = openDs();
    if (d == nullptr) {
      cecon::native_unlink(payloadPath);
      return eval::noDataLoaded();
    }
    dataPath = d->path();
  }
  const std::string outPath = nextPath(session.workdir(), "destring");
  std::vector<std::string> args = {"setcol",   "--data",    dataPath, "--var",  target,
                                   "--values", payloadPath, "--dtype", "f64",   "--out",
                                   outPath};
  if (!doReplace) args.push_back("--new");
  CobraRunResult cr;
  {
    std::optional<SessionState::ResidentEligible> eligible;
    if (resident) eligible.emplace(session);
    cr = runCobra(args);
  }
  cecon::native_unlink(payloadPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!resident && !cecon::native_exists(outPath))
    return engineErrorBlock("destring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);

  {
    // The recorded type is the compressed one; the display format stays the
    // created type's default (double %10.0g, float %9.0g).
    std::optional<std::string> sourceLabel;
    if (!doReplace) {
      auto it = session.varMeta.find(varname);
      if (it != session.varMeta.end() && it->second.label.has_value() &&
          !it->second.label->empty())
        sourceLabel = it->second.label;
    }
    VarMeta& vm = session.varMeta[target];
    vm.stataType = storage;
    vm.format = stataDefaultFormat(std::string(opts.asFloat ? "float" : "double"));
    if (!doReplace) {
      if (sourceLabel.has_value()) vm.label = sourceLabel;
      else vm.label.reset();
    }
  }
  session.implicitFloatVars.erase(target);

  ojson result = doReplace ? mutationResult(session, "destring", /*newVars=*/{},
                                            /*droppedVars=*/{}, /*rewrittenVars=*/{target})
                           : mutationResult(session, "destring", {target});
  if (!doReplace) {
    if (auto err = placeGeneratedAfterSource(session, "destring", varname, target, resident, result))
      return *err;
  }
  if (resident && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol memo;
    memo.dtype = "f64";
    memo.nRows = n;
    memo.f64 = std::move(values);
    session.residentComputedColPut(target, std::move(memo));
  }
  return result;
}

}  // namespace piet::handlers
