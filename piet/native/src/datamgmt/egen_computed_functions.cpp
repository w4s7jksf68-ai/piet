// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The egen functions PIET computes itself from the columns: anyvalue, anycount,
// anymatch, cut and concat (runEgenAdoFunction, called from runEgen). Split from
// egen.cpp without changing behavior.
#include "piet/handlers/variables.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum
#include "piet/translate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"  // isIdentByte: cut()'s argument
#include "data/missing.h"  // missing_kind: string() of a .a cell
#include "io/win_compat.h"  // to_native_path, native_exists, native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // cut's value label and the any* note are ordinary commands
#include "piet/display_hardening.h"  // exprUnboundNameRefusal: cut()'s =/exp
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // BUG-1001: expandVarlist, the one varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "egen_internal.h"

namespace piet::handlers {

using namespace egen_detail;

namespace {
// anyvalue/anycount/anymatch, and cut/concat in the forms the engine path does not
// serve, are computed here from the columns directly (help egen; the behaviours and
// messages are measured against StataBE 19.5, StataBE oracle arms F_* to O_*).

// numlist (the forms these options are written in): numbers, `a/b`, `a(s)b`.
bool expandEgenNumlist(const std::string& spec, std::vector<double>* out) {
  // Commas may separate the numbers as blanks do: `egen ... = cut(x), at(0,10,20,30)`
  // is the corpus spelling (probe_b4).
  std::string flat = spec;
  for (char& ch : flat)
    if (ch == ',') ch = ' ';
  std::istringstream iss(flat);
  std::string tok;
  while (iss >> tok) {
    const size_t lp = tok.find('(');
    const size_t slash = tok.find('/');
    char* end = nullptr;
    if (lp != std::string::npos && tok.back() != ')') {
      const size_t rp = tok.find(')', lp);
      if (rp == std::string::npos) return false;
      const double a = std::strtod(tok.substr(0, lp).c_str(), &end);
      const double st = std::strtod(tok.substr(lp + 1, rp - lp - 1).c_str(), &end);
      const double b = std::strtod(tok.substr(rp + 1).c_str(), &end);
      if (!(st > 0)) return false;
      for (double v = a; v <= b + 1e-9 * std::max(1.0, std::fabs(b)); v += st) out->push_back(v);
      continue;
    }
    if (slash != std::string::npos) {
      const double a = std::strtod(tok.substr(0, slash).c_str(), &end);
      const double b = std::strtod(tok.substr(slash + 1).c_str(), &end);
      for (double v = a; v <= b; v += 1.0) out->push_back(v);
      continue;
    }
    const double v = std::strtod(tok.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    out->push_back(v);
  }
  return !out->empty();
}

using EgenOptions = std::vector<std::pair<std::string, eval::OptValue>>;

// What every computed function works from once its argument and options have passed.
struct ComputedEgen {
  const std::string& newvar;
  const std::vector<std::string>& vars;
  const std::optional<std::string>& ifExpr;
  const std::optional<std::string>& inRange;
  const std::string& storeType;
  SessionState& session;
};

// Option `full` as typed, under whichever abbreviation spelled it; nullptr when absent.
const eval::OptValue* optionNamed(const EgenOptions& opts, const EgenFnVocab& vocab,
                                  const char* full) {
  const eval::OptValue* found = nullptr;
  for (const auto& [key, value] : opts) {
    if (key == "by") continue;
    const EgenOptSpec* spec = matchEgenOpt(key, vocab.known);
    if (spec != nullptr && std::string(spec->full) == full) found = &value;
  }
  return found;
}

// The value a variable of storage type `type` holds for v. Only float changes a number
// these functions store: their integer targets widen instead (storage::promotedType).
double heldIn(const std::string& type, double v) {
  if (type != "float" || std::isnan(v)) return v;
  if (std::fabs(v) > static_cast<double>(std::numeric_limits<float>::max()))
    return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(static_cast<float>(v));
}

// Stata's string(v) or string(v, format), by the expression layer's own function.
std::string numberAsText(double v, const std::string* format) {
  double operand = v;
  if (std::isnan(v)) {
    const int kind = cecon::missing_kind(v);
    operand = kind > 0 ? eval::stataMissingWithKind(kind) : eval::kStataMissingSentinel;
  }
  std::vector<eval::PyValue> args = {eval::PyValue::floating(operand)};
  if (format != nullptr) args.push_back(eval::PyValue::str(*format));
  return eval::fnString(args).strVal();
}

// One flag per row: inside the command's if and in. Returns the refusal, if any.
std::optional<ojson> flagQualifiedRows(const ComputedEgen& call, std::vector<char>* inside) {
  SessionState& session = call.session;
  const int64_t rows = session.datastore()->nRows();
  inside->assign(static_cast<size_t>(rows), 1);
  if (call.inRange.has_value() && !call.inRange->empty()) {
    const InRangeBounds bounds = parseInRangeBounds(*call.inRange, rows);
    if (!bounds.ok) {
      InRangeFilterResult refused;
      refused.ok = false;
      refused.refusal = bounds.refusal;
      return inRangeRejectError(refused, *call.inRange, "egen");
    }
    for (int64_t i = 0; i < rows; ++i)
      (*inside)[static_cast<size_t>(i)] = i + 1 >= bounds.a && i + 1 <= bounds.b;
  }
  if (!call.ifExpr.has_value() || call.ifExpr->empty()) return std::nullopt;
  RowIndexCache maskFrame;
  maskFrame.path = *session.currentHbin();
  maskFrame.nRows = rows;
  if (auto refusal = addIfMaskColumn(session, maskFrame, *call.ifExpr, {})) return refusal;
  {
    Datastore maskData(maskFrame.path);
    const double* truth = maskData.col(maskFrame.maskVar).f64();
    for (int64_t i = 0; i < rows; ++i)
      if (truth[i] == 0.0) (*inside)[static_cast<size_t>(i)] = 0;
  }
  cecon::native_unlink(maskFrame.path);
  return std::nullopt;
}

// Writes `payload` (one line per row) as new column `var` of the head (cobra setcol) and
// makes the result the head.
std::optional<ojson> installColumn(SessionState& session, const std::string& var, bool isText,
                                   const std::string& payload) {
  const std::string minted = mintScratchPath(session.workdir(), "egenval");
  forgetScratchMint(minted);
  const std::string valuesPath = minted.substr(0, minted.size() - 5) + ".csv";  // ".hbin"
  registerScratchMint(valuesPath);
  {
    std::ofstream file(cecon::to_native_path(valuesPath), std::ios::binary);
    file << payload;
    file.flush();
    if (!file) {
      cecon::native_unlink(valuesPath);
      return egenRcError("could not write the values of " + var + " (disk full or I/O error)", 693);
    }
  }
  const std::string outPath = nextPath(session.workdir(), "egenval");
  const CobraRunResult run =
      runCobra({"setcol", "--data", *session.currentHbin(), "--var", var, "--values", valuesPath,
                "--dtype", isText ? "factor" : "f64", "--new", "--out", outPath});
  cecon::native_unlink(valuesPath);
  if (!run.ok) {
    cecon::native_unlink(outPath);
    return engineErrorBlock(cobraErrorStr(errorFromRaw(run)));
  }
  // The engine creates --out before it can notice a full disk (BUG-124).
  if (!cecon::native_exists(outPath))
    return egenRcError("could not create " + var + " (disk full or I/O error)", 693);
  session.load(outPath);
  return std::nullopt;
}

std::optional<ojson> installNumbers(SessionState& session, const std::string& var,
                                    const std::vector<double>& values) {
  std::string payload;
  payload.reserve(values.size() * 4);
  char digits[40];
  for (double v : values) {
    if (!std::isnan(v)) {
      std::snprintf(digits, sizeof digits, "%.17g", v);
      payload += digits;
    }
    payload += '\n';
  }
  return installColumn(session, var, false, payload);
}

// The type a numeric result ends with. `created` is the one it was declared with;
// a widening keeps that type's display format.
void noteResultType(SessionState& session, const std::string& var, const std::string& created,
                    const std::string& ended) {
  session.implicitFloatVars.erase(var);
  session.varMeta[var].stataType = created;
  storage::recordReplaceType(session, var, created, ended);
}

// values() as an integer numlist. The two any* families word the refusal differently.
std::optional<ojson> readIntegerList(const std::string& text, const std::string& lead,
                                     std::vector<double>* out) {
  if (!expandEgenNumlist(text, out)) return egenRcError(lead + "invalid numlist", 121);
  for (double v : *out)
    if (v != std::floor(v)) return egenRcError(lead + "invalid numlist has noninteger elements", 126);
  return std::nullopt;
}

// A result label too long for a variable label goes to a note instead (Stata's 80-byte
// limit, measured: oracle part 5, N_*). `shortened` "" keeps `full` as the label.
void labelAnyResult(SessionState& session, const std::string& var, const std::string& full,
                    const std::string& shortened) {
  if (shortened.empty()) {
    session.setVarLabel(var, full);
    return;
  }
  session.setVarLabel(var, shortened);
  (void)dispatch("quietly notes " + var + ": " + full, session);
}

// anyvalue: the variable's value where it equals a listed integer, else missing.
ojson computeAnyvalue(const ComputedEgen& call, const std::vector<double>& wanted,
                      const std::string& typedValues) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  const std::string& var = call.vars.front();
  const double* source = session.datastore()->col(var).f64();

  std::vector<double> kept(inside.size(), std::numeric_limits<double>::quiet_NaN());
  double lowest = std::numeric_limits<double>::infinity();
  double highest = -lowest;
  for (size_t i = 0; i < inside.size(); ++i) {
    if (!inside[i] || std::find(wanted.begin(), wanted.end(), source[i]) == wanted.end()) continue;
    kept[i] = source[i];
    lowest = std::min(lowest, source[i]);
    highest = std::max(highest, source[i]);
  }
  const bool anyKept = lowest <= highest;
  const std::string ended =
      anyKept ? storage::promotedType(call.storeType, false, lowest, highest) : call.storeType;
  for (double& v : kept) v = heldIn(ended, v);

  if (auto failed = installNumbers(session, call.newvar, kept)) return *failed;
  noteResultType(session, call.newvar, call.storeType, ended);
  const std::string label = var + " if " + typedValues;
  labelAnyResult(session, call.newvar, label, label.size() > 80 ? var + ": see notes" : "");
  return mutationResult(session, "egen", {call.newvar});
}

// anycount: (variable, listed value) pairs that match, a value counted as often as it is
// listed; anymatch: whether any does. Byte whatever was declared; 0 outside the sample.
ojson computeAnycount(const ComputedEgen& call, const std::vector<double>& wanted, bool asFlag) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  std::vector<double> tally(inside.size(), 0.0);
  Datastore* ds = session.datastore();
  for (const std::string& var : call.vars) {
    const double* source = ds->col(var).f64();
    for (size_t i = 0; i < inside.size(); ++i) {
      if (!inside[i]) continue;
      tally[i] += static_cast<double>(std::count(wanted.begin(), wanted.end(), source[i]));
    }
  }
  double most = 0.0;
  for (double& n : tally) {
    if (asFlag) n = n > 0.0 ? 1.0 : 0.0;
    most = std::max(most, n);
  }

  std::string varNames;
  for (const std::string& var : call.vars) varNames += (varNames.empty() ? "" : " ") + var;
  std::string listed;
  char digits[40];
  for (double v : wanted) {
    std::snprintf(digits, sizeof digits, "%.0f", v);
    listed += (listed.empty() ? "" : " ") + std::string(digits);
  }

  if (auto failed = installNumbers(session, call.newvar, tally)) return *failed;
  noteResultType(session, call.newvar, "byte", storage::promotedType("byte", false, 0.0, most));
  const std::string label = varNames + " == " + listed;
  std::string shortened;
  if (varNames.size() >= 69) shortened = "see notes";
  else if (label.size() > 80) shortened = varNames + ": see notes";
  labelAnyResult(session, call.newvar, label, shortened);
  return mutationResult(session, "egen", {call.newvar});
}

// cut() takes a variable. Blanks and enclosing parentheses do not matter to Stata, which
// reads the argument as an expression but first requires summarize to accept it (measured:
// `cut(x y)` looks for xy, `cut((x))` runs, `cut(x*)` is "x* invalid name").
std::string cutArgumentName(const std::string& argText) {
  std::string name;
  for (char c : argText)
    if (!std::isspace(static_cast<unsigned char>(c))) name += c;
  while (name.size() >= 2 && name.front() == '(' && name.back() == ')')
    name = name.substr(1, name.size() - 2);
  return name;
}

bool isPlainVariableName(const std::string& name) {
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return cecon::ident::isIdentByte(static_cast<unsigned char>(c));
  });
}

// L.x and its kin: the engine computes them, into a scratch copy of the frame.
std::optional<ojson> engineColumnOf(SessionState& session, const std::string& tsExpr,
                                    std::vector<double>* out) {
  static const char* const kCopy = "__piet_egen_arg__";
  const std::string scratch = nextPath(session.workdir(), "egen_arg");
  try {
    runCobraJson(translate::injectTsContextForExpr(
        {"eval", "--data", *session.currentHbin(), "--gen", std::string(kCopy) + "=" + tsExpr,
         "--out", scratch},
        tsExpr, session));
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  {
    Datastore copy(scratch);
    const double* values = copy.col(kCopy).f64();
    out->assign(values, values + copy.nRows());
  }
  cecon::native_unlink(scratch);
  return std::nullopt;
}

// `var` with its last character (a whole UTF-8 sequence) swapped for each ending Stata
// tries, in the order Stata tries them: lowercase, digits, uppercase.
std::vector<std::string> respelledLabelNames(const std::string& var) {
  std::string kept = var;
  while (kept.size() > 1 && (static_cast<unsigned char>(kept.back()) & 0xC0) == 0x80)
    kept.pop_back();
  kept.pop_back();
  std::vector<std::string> names;
  names.reserve(62);
  for (char last : std::string("abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
    names.push_back(kept + last);
  return names;
}

// The value-label set a labelled cut() uses: the variable's own name while no set holds it,
// else the first free respelling (measured: price -> prica, zz with za taken -> zb). With
// every respelling taken the last one is answered, and the caller adds to that set.
std::string freeValueLabelName(const SessionState& session, const std::string& var) {
  const auto isFree = [&](const std::string& name) { return session.labelDefs.count(name) == 0; };
  if (isFree(var)) return var;
  const std::vector<std::string> names = respelledLabelNames(var);
  const auto found = std::find_if(names.begin(), names.end(), isFree);
  return found != names.end() ? *found : names.back();
}

std::optional<ojson> errorIn(const ojson& blocks) {
  for (const auto& block : blocks)
    if (block.is_object() && block.value("type", std::string()) == "error") return block;
  return std::nullopt;
}

// Stata's default quantile rule for ascending values ([D] pctile, Methods and formulas):
// with P = N*p/100, the average of ranks P and P+1 when P is whole, else rank ceil(P).
double defaultQuantile(const std::vector<double>& ascending, double p) {
  const int64_t n = static_cast<int64_t>(ascending.size());
  const double position = static_cast<double>(n) * p / 100.0;
  const double whole = std::floor(position);
  int64_t rank = static_cast<int64_t>(whole);
  if (rank == whole && rank >= 1 && rank < n)
    return (ascending[static_cast<size_t>(rank - 1)] + ascending[static_cast<size_t>(rank)]) / 2.0;
  rank = std::min(std::max<int64_t>(rank + 1, 1), n);
  return ascending[static_cast<size_t>(rank - 1)];
}

// cut() over the session's float or double copy of the variable (the default storage
// type decides the binned value, whatever the result's type: BUG-1107). `edges` holds
// at()'s numbers, or nothing under group(k), which reads them off the sample: the
// minimum of the WHOLE variable, the k-1 inner quantiles held in the default type, and
// the sample's maximum plus one (StataBE: `cut(x) if x > 2, group(2) label` on 1..6
// names 1-, 4.5-, 7-).
ojson computeCut(const ComputedEgen& call, std::vector<double> edges, int quantiles,
                 bool wantIndex, bool wantLabel, const std::vector<double>* computedArg) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;

  const std::string& defaultType = session.defaultStorageType;
  std::vector<double> binned(inside.size());
  {
    const double* source =
        computedArg != nullptr ? computedArg->data() : session.datastore()->col(call.vars.front()).f64();
    for (size_t i = 0; i < inside.size(); ++i) binned[i] = heldIn(defaultType, source[i]);
  }

  // at(): a value at or past the top edge has no interval and leaves the sample.
  std::vector<double> sample;
  sample.reserve(binned.size());
  for (size_t i = 0; i < binned.size(); ++i) {
    if (std::isnan(binned[i]) || (quantiles == 0 && binned[i] >= edges.back())) inside[i] = 0;
    if (inside[i]) sample.push_back(binned[i]);
  }
  std::sort(sample.begin(), sample.end());

  if (quantiles != 0) {
    if (sample.empty()) return egenRcError("no observations", 2000);
    double lowest = sample.front();
    for (double v : binned)
      if (!std::isnan(v)) lowest = std::min(lowest, v);
    edges.push_back(lowest);
    for (int i = 1; i < quantiles; ++i)
      edges.push_back(heldIn(defaultType, defaultQuantile(sample, 100.0 * i / quantiles)));
    edges.push_back(sample.back() + 1.0);
  }
  // A declared string type fails only here, on the first number it would store.
  if (call.storeType.empty()) return egenRcError("type mismatch", 109);

  // Filling the variable edge by edge in ascending order, every edge some row reaches is
  // stored at least once and widens an integer variable if it must, even when all those
  // rows go on to a higher edge.
  const bool asIndex = wantIndex || wantLabel || quantiles != 0;
  std::string held = call.storeType;
  std::vector<double> valueOfBin(edges.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < edges.size() && !sample.empty() && sample.back() >= edges[i]; ++i) {
    const double v = asIndex ? static_cast<double>(i) : edges[i];
    held = storage::promotedType(held, v != std::floor(v), v, v);
    valueOfBin[i] = heldIn(held, v);
  }

  std::vector<double> result(binned.size(), std::numeric_limits<double>::quiet_NaN());
  for (size_t i = 0; i < binned.size(); ++i) {
    if (!inside[i]) continue;
    const auto above = std::upper_bound(edges.begin(), edges.end(), binned[i]);
    if (above != edges.begin()) result[i] = valueOfBin[static_cast<size_t>(above - edges.begin()) - 1];
  }

  // The set goes first: a refused definition must leave no variable behind.
  std::string setName;
  if (wantLabel) {
    setName = freeValueLabelName(session, call.newvar);
    std::string define = "quietly label define " + setName;
    for (size_t i = 0; i < edges.size(); ++i)
      define += " " + std::to_string(i) + " \"" + numberAsText(edges[i], nullptr) + "-\"";
    if (auto bad = errorIn(dispatch(define + ", add", session))) return *bad;
  }
  if (auto failed = installNumbers(session, call.newvar, result)) return *failed;
  noteResultType(session, call.newvar, call.storeType, held);
  if (wantLabel) {
    if (auto bad = errorIn(dispatch("quietly label values " + call.newvar + " " + setName, session)))
      return *bad;
  }
  return mutationResult(session, "egen", {call.newvar});
}

// format() as `format` would take it (Stata checks it that way, r(120)): %9g and %9.2 are
// not formats, a string format does not suit a number, and text with no % reaches
// string(), which answers "".
bool concatFormatUsable(const std::string& format) {
  if (format.empty() || format[0] != '%') return true;
  static const std::regex reWidthOnly(R"(^%-?0?\d*[fge]c?$)");
  return isValidDisplayFormat(format) && format.back() != 's' &&
         !std::regex_match(format, reWidthOnly);
}

// concat() with punct()/format(): as the engine's (a separator after every field, the last
// one dropped, blanks trimmed), and rows outside the sample are "". The str# is sized by
// the longest text before that drop and trim, the width the variable reached being built.
ojson computeConcat(const ComputedEgen& call, const std::string& punct, const std::string* format) {
  SessionState& session = call.session;
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  Datastore* ds = session.datastore();
  std::vector<const cecon::Column*> sources;
  std::vector<char> isText;
  for (const std::string& var : call.vars) {
    sources.push_back(&ds->col(var));
    isText.push_back(ds->dtype(var) != "f64");
  }

  std::string payload;
  size_t longest = 0;
  for (size_t row = 0; row < inside.size(); ++row) {
    std::string text;
    if (inside[row]) {
      for (size_t j = 0; j < sources.size(); ++j) {
        const cecon::Column& source = *sources[j];
        if (isText[j]) {
          const int32_t code = source.codes()[row];
          if (code >= 0) text += source.levels[static_cast<size_t>(code)];
        } else {
          try {
            text += numberAsText(source.f64()[row], format);
          } catch (const eval::PyExc&) {
            return egenRcError("Invalid format", 120);
          }
        }
        text += punct;
      }
      longest = std::max(longest, text.size());
      text.resize(text.size() - punct.size());
      const size_t first = text.find_first_not_of(' ');
      text = first == std::string::npos
                 ? std::string()
                 : text.substr(first, text.find_last_not_of(' ') - first + 1);
    }
    payload += '"';
    for (char c : text) {
      if (c == '"') payload += '"';
      payload += c;
    }
    payload += "\"\n";
  }

  if (auto failed = installColumn(session, call.newvar, true, payload)) return *failed;
  session.implicitFloatVars.erase(call.newvar);
  session.varMeta[call.newvar].stataType =
      longest > 2045 ? std::string("strL") : "str" + std::to_string(std::max<size_t>(1, longest));
  return mutationResult(session, "egen", {call.newvar});
}

}  // namespace

namespace egen_detail {

std::optional<ojson> runEgenAdoFunction(const std::string& fn, const std::string& newvar,
                                        const std::string& argText,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::optional<std::string>& ifExpr,
                                        const std::optional<std::string>& inRange,
                                        const std::string& storeType, SessionState& session) {
  const bool isCut = fn == "cut";
  const bool isConcat = fn == "concat";
  const bool isAnyvalue = fn == "anyvalue";
  const bool isAnyCounter = fn == "anycount" || fn == "anymatch";
  if (!isCut && !isConcat && !isAnyvalue && !isAnyCounter) return std::nullopt;

  const EgenFnVocab& vocab = egenFnVocab().at(fn);
  const eval::OptValue* atOpt = optionNamed(opts, vocab, "at");
  const eval::OptValue* groupOpt = optionNamed(opts, vocab, "group");
  const bool wantIndex = optionNamed(opts, vocab, "icodes") != nullptr;
  const bool wantLabel = optionNamed(opts, vocab, "label") != nullptr;
  const eval::OptValue* punctOpt = optionNamed(opts, vocab, "punct");
  const eval::OptValue* formatOpt = optionNamed(opts, vocab, "format");
  const eval::OptValue* valuesOpt = optionNamed(opts, vocab, "values");
  // A bare cut(x) or concat(a b) is the engine path's.
  if (isCut && atOpt == nullptr && groupOpt == nullptr && !wantIndex && !wantLabel)
    return std::nullopt;
  if (isConcat && punctOpt == nullptr && formatOpt == nullptr) return std::nullopt;

  // Stata reads the argument first, then the options, then the by-list.
  const std::string arg = trim(argText);
  std::vector<std::string> vars;
  if (!isCut) {
    loops::VarlistExpandResult expanded = loops::expandVarlist(arg, session);
    if (expanded.isError) return expanded.error;
    if (auto unknown = loops::checkKnownVarlist(expanded.tokens, session)) return unknown;
    vars = std::move(expanded.tokens);
    if (vars.empty()) return egenRcError("too few variables specified", 102);
    if (isAnyvalue && vars.size() > 1) return egenRcError("too many variables specified", 103);
  } else if (arg.empty()) {
    return egenRcError("invalid syntax", 198);
  }
  Datastore* ds = session.datastore();
  if (isAnyCounter) {
    for (const std::string& var : vars) {
      if (ds->dtype(var) != "f64") {
        return egenRcError("string variables not allowed in varlist;\n" + var +
                               " is a string variable", 109);
      }
    }
  }

  std::optional<ojson> unimplementedOpt;
  if (auto bad = egenOptionNet(opts, vocab, nullptr, &unimplementedOpt)) return bad;

  std::vector<double> numbers;
  if ((isAnyvalue || isAnyCounter) && valuesOpt == nullptr)
    return egenRcError("option values() required", 198);
  if (isAnyCounter) {
    if (auto bad = readIntegerList(valuesOpt->strValue, "values() invalid -- ", &numbers)) return bad;
  }
  double groupWanted = 0.0;
  if (isCut) {
    if (atOpt != nullptr) {
      if (!expandEgenNumlist(atOpt->strValue, &numbers))
        return egenRcError("at() invalid -- invalid numlist", 121);
      for (size_t j = 1; j < numbers.size(); ++j) {
        if (!(numbers[j - 1] < numbers[j]))
          return egenRcError("at() invalid -- invalid numlist has elements out of order", 124);
      }
    }
    if (groupOpt != nullptr) {
      const std::string text = trim(groupOpt->strValue);
      char* parsedTo = nullptr;
      groupWanted = std::strtod(text.c_str(), &parsedTo);
      if (text.empty() || *parsedTo != '\0')
        return egenRcError("option group() incorrectly specified", 198);
    }
  }

  for (const auto& entry : opts)
    if (entry.first == "by") return egenRcError("egen ... " + fn + "() may not be combined with by", 190);
  if (unimplementedOpt.has_value()) return unimplementedOpt;

  // group(0) is how Stata reads "no group()".
  const ComputedEgen call{newvar, vars, ifExpr, inRange, storeType, session};
  if (isAnyvalue) {
    if (auto bad = readIntegerList(valuesOpt->strValue, "", &numbers)) return bad;
    if (ds->dtype(vars.front()) != "f64" || storeType.empty()) return egenRcError("type mismatch", 109);
    return computeAnyvalue(call, numbers, trim(valuesOpt->strValue));
  }
  if (isAnyCounter) return computeAnycount(call, numbers, fn == "anymatch");

  if (isConcat) {
    std::string punct = punctOpt != nullptr ? punctOpt->strValue : std::string();
    if (punct.size() >= 2 && punct.front() == '"' && punct.back() == '"')
      punct = punct.substr(1, punct.size() - 2);
    const std::string format = formatOpt != nullptr ? trim(formatOpt->strValue) : std::string();
    if (!concatFormatUsable(format)) return egenRcError("Invalid format", 120);
    return computeConcat(call, punct, format.empty() ? nullptr : &format);
  }

  std::vector<double> computedArg;
  const std::string name = cutArgumentName(arg);
  if (translate::exprHasTsToken(name)) {
    if (auto bad = engineColumnOf(session, name, &computedArg)) return bad;
    ds = session.datastore();
  } else if (name.find_first_of("*?") != std::string::npos) {
    return egenRcError(name + " invalid name", 198);
  } else if (!isPlainVariableName(name)) {
    return egenRcError("cut(" + arg + ")   invalid, variable name expected", 198);
  } else {
    vars.push_back(name);
    if (auto unknown = loops::checkKnownVarlist(vars, session)) return unknown;
  }
  if (numbers.empty() && groupWanted == 0.0)
    return egenRcError("one of at() or group() must be specified", 198);
  if (!numbers.empty() && groupWanted != 0.0)
    return egenRcError("cannot specify both at() and group() option", 198);
  if (!vars.empty() && ds->dtype(vars.front()) != "f64") return egenRcError("type mismatch", 109);
  if (groupWanted != 0.0) {
    if (groupWanted != std::floor(groupWanted))
      return egenRcError("option nq() incorrectly specified", 198);
    if (groupWanted < 2.0)
      return egenRcError("nquantiles() must be greater than or equal to 2", 198);
    if (groupWanted > static_cast<double>(ds->nRows()) + 1.0)
      return egenRcError("nquantiles() must be less than or equal to number of observations plus one", 198);
  }
  return computeCut(call, std::move(numbers), static_cast<int>(groupWanted), wantIndex, wantLabel,
                    vars.empty() ? &computedArg : nullptr);
}

}  // namespace egen_detail

}  // namespace piet::handlers
