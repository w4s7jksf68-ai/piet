// BUG-471 T2: the storage-type resolver. See piet/storage_type.h for the rules
// and the oracle arms each one rests on.
#include "piet/storage_type.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <regex>

#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/translate.h"

namespace piet::storage {

using ojson = nlohmann::ordered_json;

namespace {

std::atomic<int64_t> gUnrecordedReplaceFallbacks{0};

// The probe's second scratch column: 1 where the stored value is not an integer.
const char* const kReplaceProbeFrac = "__piet_rp_frac__";

// Stata's integer ranges (data_types.sthlp; the engine's kStore*Lo/Hi in
// src/data/frame_internal.h, which is not in PIET's include path).
constexpr double kByteLo = -127.0, kByteHi = 100.0;
constexpr double kIntLo = -32767.0, kIntHi = 32740.0;
constexpr double kLongLo = -2147483647.0, kLongHi = 2147483620.0;

double jsonNumberOr(const ojson& j, const char* key, double fallback) {
  if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return fallback;
  return j[key].get<double>();
}

}  // namespace

const char* const kReplaceProbeVar = "__piet_rp_val__";

bool isNumericType(const std::string& t) {
  return t == "byte" || t == "int" || t == "long" || t == "float" || t == "double";
}

bool isIntegerType(const std::string& t) { return t == "byte" || t == "int" || t == "long"; }

std::string resolveCreateType(const SessionState& session, const std::string& declaredToken) {
  if (!declaredToken.empty()) return isNumericType(declaredToken) ? declaredToken : std::string();
  return session.defaultStorageType;
}

std::string resolveReplaceType(SessionState& session, const std::string& varname) {
  auto it = session.varMeta.find(varname);
  if (it != session.varMeta.end() && it->second.stataType.has_value()) {
    const std::string& t = *it->second.stataType;
    if (isNumericType(t)) return t;
    return std::string();  // a string type: the numeric store has no say
  }
  if (session.implicitFloatVars.count(varname)) return "float";
  ++session.unrecordedReplaceFallbacks;
  ++gUnrecordedReplaceFallbacks;
  return "double";
}

int64_t unrecordedReplaceFallbackTotal() { return gUnrecordedReplaceFallbacks.load(); }

void appendStoreTypeArg(std::vector<std::string>& args, const std::string& type) {
  if (type.empty() || type == "double") return;
  args.push_back("--store-type");
  args.push_back(type);
}

std::string promotedType(const std::string& current, bool anyFraction, double lo, double hi) {
  if (!isIntegerType(current)) return current;
  // A fraction: byte/int -> float (:810 :843, and still float under `set type
  // double` :912 — the target is not c(type)); long -> double (:859).
  if (anyFraction) return current == "long" ? "double" : "float";
  auto fits = [&](double a, double b) { return lo >= a && hi <= b; };
  if (current == "byte" && fits(kByteLo, kByteHi)) return "byte";
  // Integer overflow widens to the next integer type that holds it: byte -> int
  // (:876), int -> long (:893).
  if (current != "long" && fits(kIntLo, kIntHi)) return "int";
  if (fits(kLongLo, kLongHi)) return "long";
  // DESIGN, unpinned by the capture: an integer past long's range. The help file
  // names only int and long as integer targets; double is the type that holds
  // the value exactly, so nothing is lost while the arm is uncaptured.
  return "double";
}

std::optional<double> numericLiteral(const std::string& rhsIn) {
  const std::string rhs = strutil::trim(rhsIn);
  static const std::regex reNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  static const std::regex reMiss(R"(^\.[a-z]?$)");
  if (std::regex_match(rhs, reMiss)) return std::numeric_limits<double>::quiet_NaN();
  if (!std::regex_match(rhs, reNum)) return std::nullopt;
  try {
    return std::stod(rhs);
  } catch (...) {
    return std::nullopt;  // out of double's range: let the engine answer
  }
}

ReplaceTypeDecision decideReplaceStoreType(SessionState& session, const std::string& resolved,
                                           bool noPromote, const std::string& varname,
                                           const std::string& rhs, bool qualified,
                                           const std::string& probeFlag,
                                           const std::string& probeArg,
                                           const std::vector<std::string>& colExprs,
                                           bool drawContext) {
  ReplaceTypeDecision d;
  d.storeType = resolved;
  if (!isIntegerType(resolved) || noPromote) return d;  // nopromote truncates in place (:827)

  // A literal: every touched row receives this one value. A value the type holds
  // decides without data, qualified or not (`replace d = 1 if x > 5` into a
  // byte, the commonest replace there is); one that widens it does too when
  // every row is touched. Only a widening literal under if/in reads the data,
  // because a replace that touches no row changes nothing.
  if (std::optional<double> lit = numericLiteral(rhs)) {
    if (std::isnan(*lit)) return d;  // missing fits every type
    const bool frac = std::trunc(*lit) != *lit;
    const std::string widened = promotedType(resolved, frac, *lit, *lit);
    if (widened == resolved || !qualified) {
      d.storeType = widened;
      return d;
    }
  }

  // The data pass. A projection of the columns the probe reads when the head is
  // resident; otherwise the head file itself (materialized by datastore()).
  std::string dataPath;
  std::unique_ptr<Datastore> proj;
  {
    std::vector<const std::string*> ex;
    for (const auto& s : colExprs) ex.push_back(&s);
    if (auto cols = referencedColumns(session, ex, varname)) {
      std::string projPath;
      proj = fetchColumns(session, *cols, session.workdir(), &projPath);
      ++d.engineCalls;
      if (proj) dataPath = projPath;
    }
  }
  if (!proj) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) {
      d.error = ojson(eval::noDataLoaded());
      return d;
    }
    dataPath = ds->path();
  }
  const std::string pv = kReplaceProbeVar;
  const std::string pf = kReplaceProbeFrac;
  std::vector<std::string> args = {"eval",      "--data", dataPath,
                                   probeFlag,   probeArg, "--gen",
                                   pf + "=(" + pv + "!=int(" + pv + "))",
                                   "--summarize", pv + "," + pf};
  args = translate::injectTsContextForExpr(args, probeArg, session);
  if (drawContext) {
    // The store draws from the stream the probe would consume: hand the probe
    // that stream and give it back, so both see the same draws.
    const long long savedStream = session.rngStream;
    args = translate::injectRngContextForExpr(args, probeArg, session);
    session.rngStream = savedStream;
  }
  ojson out;
  {
    // No --out: a read-only call. Stateless, so the resident frame never stages
    // the probe's scratch columns and a projection never evicts the head.
    SessionState::ResidentSuspend suspend(session);
    SessionState::StatelessRead statelessRead(session);
    try {
      out = runCobraJson(args);
    } catch (const CobraError& ce) {
      d.error = ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
      return d;
    }
  }
  ++d.engineCalls;
  const ojson summ = out.value("summarize", ojson::object());
  const ojson sv = summ.value(pv, ojson::object());
  const ojson sf = summ.value(pf, ojson::object());
  const double n = jsonNumberOr(sv, "N", 0.0);
  if (!(n > 0)) return d;  // nothing non-missing is stored: no widening
  const double lo = jsonNumberOr(sv, "min", 0.0);
  const double hi = jsonNumberOr(sv, "max", 0.0);
  const bool frac = jsonNumberOr(sf, "max", 0.0) > 0.0;
  d.storeType = promotedType(resolved, frac, lo, hi);
  return d;
}

void recordReplaceType(SessionState& session, const std::string& varname,
                       const std::string& resolved, const std::string& stored) {
  if (varname.empty() || stored.empty() || stored == resolved) return;
  session.implicitFloatVars.erase(varname);
  session.varMeta[varname].stataType = stored;
}

std::string egenFunctionName(const std::string& rhs) {
  static const std::regex reFn(R"(^\s*(\w+)\s*\()");
  std::smatch m;
  if (!std::regex_search(rhs, m, reFn)) return std::string();
  return strutil::lower(m.str(1));
}

bool egenIgnoresStoreType(const std::string& fn) {
  return fn == "tag" || fn == "seq" || fn == "mode";
}

bool recordEgenFixedType(SessionState& session, const std::string& newvar,
                         const std::string& rhs, const std::string& seqType) {
  const std::string fn = egenFunctionName(rhs);
  if (!egenIgnoresStoreType(fn) || newvar.empty()) return false;
  if (fn == "tag" || fn == "seq") {
    session.implicitFloatVars.erase(newvar);
    // seq: `gen byte` widened by its replace (_gseq.ado:42, :45-49) to
    // int/long once a value leaves byte's range (the lattice of :876/:893).
    session.varMeta[newvar].stataType =
        fn == "seq" && isIntegerType(seqType) ? seqType : std::string("byte");
    return true;
  }
  // mode: `gen `type' g = varlist[_N]` with type = the source's own type
  // (_gmode.ado:19, :122). The source is the one variable inside mode(...).
  static const std::regex reSrc(R"(^\s*\w+\s*\(\s*([A-Za-z_]\w*)\s*\))");
  std::smatch m;
  std::string src;
  if (std::regex_search(rhs, m, reSrc)) src = m.str(1);
  session.implicitFloatVars.erase(newvar);
  auto it = session.varMeta.find(src);
  if (!src.empty() && it != session.varMeta.end() && it->second.stataType.has_value()) {
    session.varMeta[newvar].stataType = *it->second.stataType;
  } else if (!src.empty() && session.implicitFloatVars.count(src)) {
    session.implicitFloatVars.insert(newvar);
  }
  // else: the source's type was never recorded, and neither is the copy's.
  return true;
}

}  // namespace piet::storage
