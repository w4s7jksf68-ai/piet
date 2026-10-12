// cclass.cpp — see piet/cclass.h for the full design note (BETA Phase A4,
// BS-091/BS-101).
#include "piet/cclass.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <regex>

#include "io/win_compat.h"  // cecon::env_utf8 (BUG-051 wide %USERNAME%)
#include "piet/bundle_paths.h"  // bundleResourcesDir (c(sysdir_sample))

namespace piet {

namespace {

// c(sysdir_sample): the directory holding the bundled sample datasets.
// Resolution order mirrors `sysuse` exactly (dispatcher.cpp's path helpers,
// themselves mirroring main.cpp's runUiSmoke): bundle Resources/sample first,
// dev-tree piet/packaging/sample otherwise. Duplicated rather than shared for
// the same manifest reason dispatcher.cpp documents at its own copy.
std::string sampleDataDir() {
  if (auto res = bundleResourcesDir()) {
    std::string dir = *res + "/sample";
    std::error_code ec;
    if (std::filesystem::is_directory(cecon::to_native_path(dir), ec)) return dir;
  }
  std::string thisFile = __FILE__;  // .../piet/native/src/cclass.cpp
  auto pos = thisFile.rfind("/piet/native/src/cclass.cpp");
  std::string root = (pos == std::string::npos) ? "../.." : thisFile.substr(0, pos);
  return root + "/piet/packaging/sample";
}

std::string detectOs() {
#if defined(__APPLE__)
  return "MacOSX";
#elif defined(_WIN32)
  return "Windows";
#else
  return "Unix";
#endif
}

// Coarse, compile-time-detected description — NOT byte-identical to real
// Stata's own machine-specific string (see cclass.h's "known simplification"
// note); good enough for a c(machine_type) that reads sensibly.
std::string detectMachineType() {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "Mac (Apple Silicon)";
#else
  return "Mac (Intel 64-bit)";
#endif
#elif defined(_WIN32)
  return "PC (Windows 64-bit)";
#elif defined(__aarch64__) || defined(__arm64__)
  return "PC (ARM 64-bit)";
#else
  return "PC (Unix 64-bit)";
#endif
}

std::string detectUsername() {
#ifdef _WIN32
  // A Korean %USERNAME% comes back from narrow getenv() as CP949 bytes, which
  // would land in c(username) as mojibake (and in any path built from it).
  // env_utf8 reads it wide and hands back UTF-8 like the rest of piet.
  return cecon::env_utf8("USERNAME");
#else
  const char* u = std::getenv("USER");
  if (!u || !*u) u = std::getenv("LOGNAME");
  return (u && *u) ? std::string(u) : std::string();
#endif
}

// current_date / current_time (BUG-091) — folded in verbatim from the
// pre-A4 display_hardening.cpp resolver (no behavior change to these two
// keys' formatting, only relocation into the single shared source).
std::string currentDateOrTime(const std::string& name) {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  // MinGW-w64/MSVC have no localtime_r; localtime_s is the same thread-safe
  // fill (argument order differs) and keeps the formatting below untouched.
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  if (name == "current_date") {
    static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0;
    std::snprintf(buf, sizeof(buf), "%2d %s %04d", tmv.tm_mday, kMon[mon], tmv.tm_year + 1900);
  } else {  // current_time
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  return std::string(buf);
}

}  // namespace

std::optional<CValue> resolveCValue(const std::string& name, SessionState& session) {
  // -- numeric keys ----------------------------------------------------
  if (name == "N" || name == "obs") {
    // c(obs) is accepted as a harmless synonym of c(N) — see cclass.h.
    Datastore* ds = session.datastore();
    return CValue{false, std::to_string(ds ? ds->nRows() : 0)};
  }
  if (name == "k") {
    Datastore* ds = session.datastore();
    return CValue{false, std::to_string(ds ? static_cast<long long>(ds->names().size()) : 0)};
  }
  if (name == "rc") {
    // Same value _rc resolves to (macros.cpp's evalArith bare-_rc rule) —
    // read straight from the same store so the two never disagree.
    return CValue{false, session.eval.locals.get("_rc", "0")};
  }
  if (name == "level") {
    return CValue{false, std::to_string(session.level)};
  }
  if (name == "seed") {
    return CValue{false, std::to_string(session.seed.value_or(0))};
  }
  if (name == "changed") {
    return CValue{false, session.datasetChanged ? "1" : "0"};
  }
  if (name == "version") {
    // Fixed emulation-target constant — see cclass.h's "known
    // simplification" note (bare `version #` stays BS-041/A1's own
    // deliberately one-shot, non-persisted mechanism; untouched here).
    return CValue{false, "18"};
  }

  // -- string keys -------------------------------------------------------
  if (name == "os") return CValue{true, detectOs()};
  if (name == "machine_type") return CValue{true, detectMachineType()};
  if (name == "username") return CValue{true, detectUsername()};
  if (name == "pwd") {
    return CValue{true, session.cwd.has_value() && !session.cwd->empty()
                             ? *session.cwd
                             : std::filesystem::current_path().u8string()};
  }
  if (name == "filename") return CValue{true, session.currentFile()};
  // BUG-471 T2: c(type), the default storage type `set type` writes
  // (w2e_float_default.log settype_ctype_default :1236 "float",
  // settype_double_ctype :1262 "double").
  if (name == "type") return CValue{true, session.defaultStorageType};
  if (name == "sysdir_sample") return CValue{true, sampleDataDir()};
  if (name == "current_date" || name == "current_time") {
    return CValue{true, currentDateOrTime(name)};
  }

  return std::nullopt;  // outside this build's ~15-key spine — honest gap.
}

std::string substituteCRefs(const std::string& expr, SessionState& session) {
  if (expr.find("c(") == std::string::npos) return expr;  // cheap common-case skip
  static const std::regex reC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  auto begin = std::sregex_iterator(expr.begin(), expr.end(), reC);
  auto end = std::sregex_iterator();
  if (begin == end) return expr;

  std::string out;
  out.reserve(expr.size());
  size_t last = 0;
  bool any = false;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    auto resolved = resolveCValue(m.str(1), session);
    if (!resolved.has_value()) continue;  // leave THIS occurrence untouched
    any = true;
    out.append(expr, last, static_cast<size_t>(m.position(0)) - last);
    if (resolved->isString) {
      out += '"';
      out += resolved->text;
      out += '"';
    } else {
      out += resolved->text;
    }
    last = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
  }
  if (!any) return expr;
  out.append(expr, last, expr.size() - last);
  return out;
}

}  // namespace piet
