// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// delimited export.
#include "datamgmt/data_io_internal.h"
#include "piet/user_paths.h"
namespace piet::handlers::data_io_detail {


// ---------------------------------------------------------------------------
// export / import delimited (P1 wave). PIET-native: the writer needs the
// session's value labels, which live here and not in the .hbin.




// A numeric cell the way Stata's `export delimited` writes it. A missing is
// `.` or its extended code `.a`..`.z` (oracle: Stata's own p1_roundtrip.csv
// carries literal `.a`).
//
// W:P8. The numeric spelling used to be the shortest round-trip decimal
// (`pythonFloatRepr`), which is NOT what Stata writes — measured cell-by-cell
// against Stata's own export of the same fixture, every one of 4,800 data rows
// differed. Two separate deviations: an integral value came out `1.0` where
// Stata writes `1` (so every id/year/region column in every exported CSV
// carried a spurious `.0` for whatever read it next), and a fractional value
// came out at 17 significant digits with its leading zero where Stata writes
// 16 without it. Stata's real rule, pinned by probing StataBE 19.5 with
// exact-valued doubles rather than inferred: `%.16g`, leading zero dropped
// for |x|<1, and the exponent branch re-spelled `%.11e` (`1e-20` ->
// `1.00000000000e-20`, `123456789012345678` -> `1.23456789012e+17`).
//
// Deliberate consequence, not an oversight: 16 digits is one short of a
// guaranteed round trip for a double, so a value can lose its last bit
// through a CSV. That is Stata's own behaviour and gate ① puts matching it
// first; PIET's own lossless path is `.dta`/`.hbin`.
//
// Known remaining divergence, out of reach here rather than unnoticed: Stata
// writes a `float` column at 8 significant digits, and PIET's in-memory
// datastore is all-f64, so a float column loaded from a .dta exports at
// double width. Closing that needs per-column storage types the datastore
// does not carry.
// `v` written with `dec` decimals, rounded HALF AWAY FROM ZERO (printf rounds
// half to even, which is what put 22 of 28,649 measured cells one unit below
// Stata in the 16th digit), then tidied Stata's way: trailing zeros and a bare
// trailing point dropped, and no leading zero on |x|<1.
// `v` written with `dec` decimals, then tidied Stata's way: trailing zeros and
// a bare trailing point dropped, and no leading zero on |x|<1.
std::string fixedDecimals(double v, int dec) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  if (out.find('.') != std::string::npos) {
    out.erase(out.find_last_not_of('0') + 1);
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  if (out.rfind("0.", 0) == 0) out.erase(0, 1);
  else if (out.rfind("-0.", 0) == 0) out.erase(1, 1);
  return out;
}

FixedSpan fixedDecimalsAbs(char* buf, size_t cap, double av, int dec) {
  FixedSpan s;
  // std::to_chars(fixed, precision) and snprintf("%.*f") are both required to
  // be correctly rounded, so they emit the same digits; to_chars is simply not
  // obliged to consult the locale or parse a format string on the way, which is
  // most of what snprintf was costing per cell. The byte-identity diff of an
  // exported CSV is what certifies the equality, not this note.
  std::to_chars_result r = std::to_chars(buf, buf + cap, av, std::chars_format::fixed, dec);
  if (r.ec != std::errc()) return s;
  size_t len = static_cast<size_t>(r.ptr - buf);
  if (std::memchr(buf, '.', len) != nullptr) {
    while (len > 0 && buf[len - 1] == '0') --len;
    if (len > 0 && buf[len - 1] == '.') --len;
  }
  size_t off = 0;
  if (len >= 2 && buf[0] == '0' && buf[1] == '.') { off = 1; --len; }
  s.p = buf + off;
  s.len = len;
  return s;
}


std::string csvNumeric(double v) {
  if (std::isnan(v)) {
    // W:P8: an EXTENDED missing keeps its code (`.a`..`.z`, as Stata's own
    // p1_roundtrip.csv shows), but a plain system missing is written as an
    // EMPTY field — live-verified. Emitting `.` for it made 131 cells of the
    // P8 fixture read as the literal string "." to anything that opened the
    // file expecting a blank.
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    return m == "." ? std::string() : m;
  }
  if (v == 0.0) return "0";
  // Stata spells the value as the WIDEST plain decimal that fits 17 characters
  // of magnitude (sign excluded) — not as a fixed count of significant digits,
  // which is why `.1473717297922637` carries 16 of them and
  // `.0000617833399588` only 12. Exponent form appears only when nothing plain
  // fits, and is `%.11e` exactly (`1e-20` -> `1.00000000000e-20`).
  //
  // MEASURED RESIDUAL, stated rather than hidden: against Stata's own export of
  // the 4,800-row P8 fixture, 139,185 of 139,200 cells are identical. The
  // remaining FIFTEEN differ by one unit in the last of 16-17 digits, and only
  // where the shortest round-trip decimal needs all 17 — Stata's own decimal
  // conversion is not correctly rounded there. Three candidate reconstructions
  // of its exact behaviour (round the 17-digit decimal half away from zero;
  // scale by a power of ten in double arithmetic; truncate the shortest
  // representation) were each implemented and measured, each matched FEWER
  // cells than the plain correctly-rounded conversion kept here, and they
  // contradicted one another on the same cells. Chasing a vendor float-printer
  // quirk below 1e-16 relative is not worth a bespoke decimal engine; the
  // format itself — width budget, integer spelling, dropped leading zero,
  // missing and date spelling, exponent form — is exact.
  for (int dec = 17; dec >= 0; --dec) {
    std::string s = fixedDecimals(v, dec);
    if (s == "0" || s == "-0") continue;  // rounded away to nothing; keep looking
    const size_t visible = s.size() - (s[0] == '-' ? 1 : 0);
    if (visible <= 17) return s;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.11e", v);
  return std::string(buf);
}


// P2-PERF ARC-B/S08 — the same function, appending instead of returning, with
// one shortcut. `export delimited` on a 500k-cell frame called csvNumeric once
// per cell and csvNumeric allocated a std::string per snprintf attempt; the
// writer measured 21 MB/s, which is not a formatting rate, it is an allocation
// rate.
//
// The shortcut is the INTEGRAL case (`id`, `year`, `region`, any coded
// variable — most columns of most datasets): the general loop reaches it by
// asking snprintf for "%.17f" and then deleting the seventeen zeros it just
// asked for. An integral double under 1e17 spells identically through
// std::to_chars, in one pass, with no allocation and no trimming. Above 1e17
// the integer part alone exceeds the 17-character budget, so that case still
// belongs to the loop (which sends it to the `%.11e` branch, unchanged).
//
// Everything else is byte-for-byte the old path: same widest-plain-decimal
// search from dec=17 down, same "rounded away to nothing" skip, same `%.11e`
// fallback. The gate is a byte-identity diff of an exported CSV, not a reading
// of this comment.
void appendCsvNumeric(std::string& out, double v) {
  if (std::isnan(v)) {
    std::string m = cecon::missing_text(cecon::missing_kind(v));
    if (m != ".") out += m;
    return;
  }
  if (v == 0.0) { out += '0'; return; }

  if (v == std::floor(v) && std::fabs(v) < 1e17) {
    char ib[32];
    const long long iv = static_cast<long long>(v);
    std::to_chars_result r = std::to_chars(ib, ib + sizeof(ib), iv);
    if (r.ec == std::errc()) {
      const size_t len = static_cast<size_t>(r.ptr - ib);
      if (len - (iv < 0 ? 1u : 0u) <= 17) { out.append(ib, len); return; }
    }
  }

  const bool neg = v < 0;
  const double av = neg ? -v : v;
  char buf[512];
  for (int dec = 17; dec >= 0; --dec) {
    FixedSpan s = fixedDecimalsAbs(buf, sizeof(buf), av, dec);
    if (s.p == nullptr) break;
    if (s.len == 1 && s.p[0] == '0') continue;  // rounded away to nothing
    if (s.len <= 17) {
      if (neg) out += '-';
      out.append(s.p, s.len);
      return;
    }
  }
  char eb[64];
  std::snprintf(eb, sizeof(eb), "%.11e", v);
  out += eb;
}


// P2-PERF ARC-B/S08: csvField's no-quoting-needed case returned a COPY of the
// level string for every cell. Appending skips it.
void appendCsvField(std::string& out, const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') { needQuote = true; break; }
  }
  if (!needQuote) { out += s; return; }
  out += '"';
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
}


int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }


// days since 1970-01-01 -> civil y/m/d (Hinnant's civil_from_days).
void civilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yr = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  *y = yr + (*m <= 2);
}


std::string tdDayString(int64_t dayIndex) {
  static const char* const kMon[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec"};
  int64_t y; unsigned m, d;
  civilFromDays(dayIndex - kDaysTdEpochToUnix, &y, &m, &d);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02u%s%lld", d, kMon[m - 1], static_cast<long long>(y));
  return std::string(buf);
}


// The letter after `%t` for a date-family format, or 0 when `fmt` is not one
// this renderer owns. A detail suffix (`%tdNN/DD/CCYY`) yields 0 on purpose.
char dateFamilyOf(const std::string& fmt) {
  if (fmt.size() != 3 || fmt[0] != '%' || fmt[1] != 't') return 0;
  switch (fmt[2]) {
    case 'd': case 'w': case 'm': case 'q': case 'h': case 'y': case 'c':
      return fmt[2];
    default: return 0;
  }
}


std::string formatStataDate(double v, char family) {
  if (!std::isfinite(v)) return std::string();
  const int64_t i = static_cast<int64_t>(std::floor(v));
  char buf[48];
  switch (family) {
    case 'd': return tdDayString(i);
    case 'w': std::snprintf(buf, sizeof(buf), "%lldw%lld",
                            static_cast<long long>(1960 + floorDiv(i, 52)),
                            static_cast<long long>(floorMod(i, 52) + 1)); return buf;
    case 'm': std::snprintf(buf, sizeof(buf), "%lldm%lld",
                            static_cast<long long>(1960 + floorDiv(i, 12)),
                            static_cast<long long>(floorMod(i, 12) + 1)); return buf;
    case 'q': std::snprintf(buf, sizeof(buf), "%lldq%lld",
                            static_cast<long long>(1960 + floorDiv(i, 4)),
                            static_cast<long long>(floorMod(i, 4) + 1)); return buf;
    case 'h': std::snprintf(buf, sizeof(buf), "%lldh%lld",
                            static_cast<long long>(1960 + floorDiv(i, 2)),
                            static_cast<long long>(floorMod(i, 2) + 1)); return buf;
    case 'y': std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i)); return buf;
    case 'c': {
      const int64_t days = floorDiv(i, kMsPerDay);
      const int64_t rem = floorMod(i, kMsPerDay);
      std::snprintf(buf, sizeof(buf), "%s %02lld:%02lld:%02lld", tdDayString(days).c_str(),
                    static_cast<long long>(floorDiv(rem, 3600000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 3600000), 60000)),
                    static_cast<long long>(floorDiv(floorMod(rem, 60000), 1000)));
      return buf;
    }
    default: return std::string();
  }
}



bool hasFlagOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::string& name) {
  for (const auto& kv : opts) if (kv.first == name) return true;
  return false;
}


// An option's value with its surrounding double quotes off. `sheet("Alpha")`
// and `sheet(Alpha)` are the same request.
std::string optDequote(const std::string& raw) {
  std::string v = trim(raw);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return trim(v);
}


// The value of a named option, or nullopt when it is absent or was given as a
// bare flag.
std::optional<std::string> optStrOf(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& name) {
  for (const auto& kv : opts) {
    if (kv.first != name) continue;
    if (kv.second.isBool) return std::nullopt;
    return optDequote(kv.second.strValue);
  }
  return std::nullopt;
}


// Stata's pathsuffix(): the extension of the last path component, "" when it
// has none. export delimited / import delimited supply ".csv" and the excel
// pair ".xlsx" to a name without one (w2ar dx_exd_noext log:760-769 writes
// ne1.csv and leaves the bare name absent; dx_exx_noext log:1527).
bool pathHasSuffix(const std::string& p) {
  const size_t slash = p.find_last_of("/\\");
  const size_t dot = p.find_last_of('.');
  if (dot == std::string::npos) return false;
  if (slash != std::string::npos && dot < slash) return false;
  return true;
}


std::string withDefaultSuffix(const std::string& p, const char* suffix) {
  return pathHasSuffix(p) ? p : p + suffix;
}


// The file as the user typed it, with the suffix the command supplies: what
// Stata's export messages name (`file out.csv saved`, `file ./sub/o2.csv
// saved`, `file out.xlsx already exists`), never the resolved absolute path,
// which would put this machine's working directory into every captured
// transcript (BUG-410).
std::string typedFileName(const std::string& pathArg, const char* suffix) {
  return withDefaultSuffix(stripOneQuotePair(trim(pathArg)), suffix);
}


// True when the directory a file would be written into does not exist:
// Stata's export answers r(603) "file could not be opened" there (w2ar
// dx_exd_missing_dir, log:796).
bool parentDirMissing(const std::string& p) {
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return false;
  std::error_code ec;
  return !std::filesystem::is_directory(cecon::to_native_path(p.substr(0, slash)), ec);
}


// A float column's text: Stata writes float storage at 8 significant digits
// (w2ar dx_exd_plain_L2 `1.01` for float 1.01, log:455; w2b 7.0100002,
// w2b log:420-423), integers in full, without the leading zero of |x|<1 (the
// same tidy rule the double path uses).
void appendFloatNumeric(std::string& out, double v) {
  if (std::isnan(v) || v == std::floor(v)) { appendCsvNumeric(out, v); return; }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.8g", v);
  std::string s(buf);
  if (s.rfind("0.", 0) == 0) s.erase(0, 1);
  else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
  out += s;
}


std::string trimBlanks(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return std::string();
  const size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}


void appendQuotedField(std::string& out, const std::string& s, char delim,
                       DelimWriteSpec::Quote q) {
  switch (q) {
    case DelimWriteSpec::Quote::kMinimal: appendCsvField(out, s, delim); return;
    case DelimWriteSpec::Quote::kNone: out += s; return;
    case DelimWriteSpec::Quote::kAllVerbatim: out += '"'; out += s; out += '"'; return;
    case DelimWriteSpec::Quote::kAll:
      out += '"';
      for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
      }
      out += '"';
      return;
  }
}




std::string stripOneQuotePair(const std::string& v) {
  if (v.size() >= 4 && v.compare(0, 2, "`\"") == 0 && v.compare(v.size() - 2, 2, "\"'") == 0)
    return v.substr(2, v.size() - 4);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  return v;
}


// The block an import answers with: `use`'s own `open` shape (the frontend
// reads source_file/filepath off it), over the frame just adopted.
ojson openBlockFor(SessionState& session, const std::string& path) {
  ojson m = mutationResult(session, "open");
  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "open";
  out["source_file"] = path;
  out["filepath"] = path;
  for (auto it = m.begin(); it != m.end(); ++it) {
    if (it.key() == "type" || it.key() == "action") continue;
    out[it.key()] = it.value();
  }
  return out;
}
}
namespace piet::handlers {
using namespace data_io_detail;


ojson reEchoPath(ojson block, const std::string& resolved, const std::string& shown) {
  if (!block.is_object()) return block;
  // A success line carries "text"; an error block carries "message".
  for (const char* key : {"text", "message"}) {
    if (!block.contains(key) || !block[key].is_string()) continue;
    std::string text = block[key].get<std::string>();
    const size_t at = text.find(resolved);
    if (at == std::string::npos) continue;
    block[key] = text.substr(0, at) + shown + text.substr(at + resolved.size());
  }
  return block;
}


ojson writeDelimitedFile(const std::vector<std::string>& varlist, const std::string& path,
                         const DelimWriteSpec& spec, const std::optional<std::string>& ifExpr,
                         const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  const std::string& shown = spec.shown.empty() ? path : spec.shown;
  Datastore* base = session.datastore();
  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = base->names();
  for (const auto& v : cols) {
    if (!base->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  // Per-column facts read from the SESSION's frame (formats, storage types and
  // value labels are keyed by name), before any row filter.
  struct OutCol {
    std::string name;
    const std::map<std::string, std::string>* labels = nullptr;  // value labels, if any
    char dateFamily = 0;   // W:P8: letter after `%t`, 0 for a non-date column
    std::string dispFmt;   // display format (datafmt / outsheet)
    bool isFloat = false;  // float storage: 8 significant digits
  };
  std::vector<OutCol> oc;
  oc.reserve(cols.size());
  for (const auto& name : cols) {
    OutCol o;
    o.name = name;
    if (!spec.noLabel) {
      auto lv = session.varLblname.find(name);
      if (lv != session.varLblname.end()) {
        auto ld = session.labelDefs.find(lv->second);
        if (ld != session.labelDefs.end()) o.labels = &ld->second;
      }
    }
    const cecon::Column& col = base->col(name);
    if (col.dtype != cecon::DType::Factor) {
      const VarInfo vi = base->varinfo(name);
      o.dispFmt = session.displayFormat(vi);
      o.dateFamily = dateFamilyOf(o.dispFmt);
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.stataType.has_value() &&
          !vm->second.stataType->empty()) {
        o.isFloat = *vm->second.stataType == "float";
      } else {
        // No declared storage: Stata's default numeric type is float, but this
        // frame's undeclared columns can hold doubles (an engine result), so a
        // column is written as float only when every value IS one.
        o.isFloat = true;
        const auto& f = col.f64();
        for (int64_t r = 0; r < base->nRows(); ++r) {
          const double v = f[r];
          if (!std::isnan(v) && static_cast<double>(static_cast<float>(v)) != v) {
            o.isFloat = false;
            break;
          }
        }
      }
    }
    oc.push_back(std::move(o));
  }

  Datastore* ds = base;
  std::unique_ptr<Datastore> filtered;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, base->path(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), spec.verb);
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }

  // Write to a sibling temp then rename, so a failure never leaves a truncated
  // file where a readable one used to be (and dest==source is safe).
  std::string tmpPath = path + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmpPath), std::ios::binary);
    if (!f) {
      return eval::errorBlock("engine_error", "file " + shown + " could not be opened",
                              std::nullopt, std::nullopt, std::nullopt, "r(603)");
    }

    if (spec.header) {
      std::string head;
      for (size_t c = 0; c < cols.size(); ++c) {
        if (c) head += spec.delim;
        head += cols[c];
      }
      head += '\n';
      f.write(head.data(), static_cast<std::streamsize>(head.size()));
    }

    std::vector<const cecon::Column*> colp;
    colp.reserve(cols.size());
    for (const auto& name : cols) colp.push_back(&ds->col(name));

    // P2-PERF ARC-B/S08: rows accumulate into a ~1 MB block and go out with one
    // write() each, instead of one operator<< per row through the stream's
    // default buffer. Same bytes, two orders of magnitude fewer stream calls.
    const int64_t n = ds->nRows();
    std::string block;
    block.reserve(kCsvBlockBytes + 4096);
    for (int64_t r = 0; r < n; ++r) {
      for (size_t c = 0; c < oc.size(); ++c) {
        if (c) block += spec.delim;
        const cecon::Column& col = *colp[c];
        if (col.dtype == cecon::DType::Factor) {
          int32_t code = col.codes()[r];
          if (code >= 0) {
            appendQuotedField(block, col.levels[static_cast<size_t>(code)], spec.delim, spec.quote);
          } else if (spec.quote == DelimWriteSpec::Quote::kAll ||
                     spec.quote == DelimWriteSpec::Quote::kAllVerbatim) {
            block += "\"\"";
          }
          continue;
        }
        double v = col.f64()[r];
        // A value-labeled numeric exports as its LABEL (Stata's default;
        // `nolabel` gives the code) — the reason this writer lives in PIET.
        if (oc[c].labels != nullptr && !std::isnan(v)) {
          std::string key = eval::pythonFloatRepr(v);
          auto it = oc[c].labels->find(key);
          if (it == oc[c].labels->end() && v == std::floor(v)) {
            it = oc[c].labels->find(std::to_string(static_cast<long long>(v)));
          }
          if (it != oc[c].labels->end()) {
            appendQuotedField(block, it->second, spec.delim, spec.quote);
            continue;
          }
        }
        if (oc[c].dateFamily != 0 && !std::isnan(v)) {
          std::string ds8 = formatStataDate(v, oc[c].dateFamily);
          if (!ds8.empty()) {
            if (spec.quote == DelimWriteSpec::Quote::kMinimal) appendCsvField(block, ds8, spec.delim);
            else block += ds8;
            continue;
          }
        }
        if (spec.dataFmt && !std::isnan(v) && !oc[c].dispFmt.empty()) {
          std::string s;
          try {
            s = trimBlanks(eval::applyStataFmt(v, oc[c].dispFmt));
          } catch (...) {
            s.clear();
          }
          if (!s.empty()) { block += s; continue; }
        }
        if (oc[c].isFloat) appendFloatNumeric(block, v);
        else appendCsvNumeric(block, v);
      }
      block += '\n';
      if (block.size() >= kCsvBlockBytes) {
        f.write(block.data(), static_cast<std::streamsize>(block.size()));
        block.clear();
        if (!f) break;
      }
    }
    if (!block.empty()) f.write(block.data(), static_cast<std::streamsize>(block.size()));
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
      return engineErrorBlock(spec.verb + ": failed writing " + shown + " (disk full?)", "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmpPath), cecon::to_native_path(path), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmpPath), rmEc);
    return engineErrorBlock(spec.verb + ": failed writing " + shown, "data_io");
  }

  ojson block;
  block["type"] = "display";
  block["text"] = "file " + shown + " saved";
  return block;
}


ojson runExportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  return runExportDelimited(varlist, pathArg, opts, std::nullopt, std::nullopt, session);
}


ojson runExportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         const std::optional<std::string>& ifExpr,
                         const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  DelimWriteSpec spec;
  // export delimited: delimiter() is "tab" or ONE character, anything
  // longer is r(198) "<d> invalid character in delimiter() option".
  for (const auto& kv : opts) {
    if (kv.first != "delimiter" && kv.first != "delim") continue;
    if (kv.second.isBool) continue;
    std::string v = trim(kv.second.strValue);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v == "tab" || v == "\\t") spec.delim = '\t';
    else if (v.size() == 1) spec.delim = v[0];
    else {
      return engineErrorBlock(v + " invalid character in delimiter() option", "unsupported_option");
    }
  }
  spec.noLabel = hasFlagOpt(opts, "nolabel");
  spec.header = !hasFlagOpt(opts, "novarnames");
  spec.dataFmt = hasFlagOpt(opts, "datafmt");
  if (hasFlagOpt(opts, "quote")) spec.quote = DelimWriteSpec::Quote::kAll;
  const bool replace = hasFlagOpt(opts, "replace");

  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }

  const std::string path = withDefaultSuffix(resolveUserPath(session, pathArg), ".csv");
  spec.shown = typedFileName(pathArg, ".csv");
  if (cecon::native_exists(path) && !replace) {
    return eval::errorBlock("engine_error", "file " + spec.shown + " already exists", std::nullopt,
                            std::nullopt, std::nullopt, "r(602)");
  }
  if (parentDirMissing(path)) {
    return eval::errorBlock("engine_error", "file " + spec.shown + " could not be opened",
                            std::nullopt, std::nullopt, std::nullopt, "r(603)");
  }
  return writeDelimitedFile(varlist, path, spec, ifExpr, inRange, session);
}


ojson runImportDelimited(const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  return runImportDelimited(std::vector<std::string>(), pathArg, opts, session);
}
}
