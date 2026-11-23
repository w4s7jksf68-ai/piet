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
#include <cstring>
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
#include "engine/scratch_columns.h"
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
ojson computeConcat(const ComputedEgen& call, const std::string& punct, const std::string* format,
                    bool decode, size_t maxLength) {
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
          const double value = source.f64()[row];
          if (decode) {
            // decode, then string() for what decode leaves empty: a variable with no usable
            // value label, a code the label does not name, a noninteger value.
            auto attached = session.varLblname.find(call.vars[j]);
            auto defined = attached == session.varLblname.end()
                               ? session.labelDefs.end()
                               : session.labelDefs.find(attached->second);
            auto labels = session.valueLabels.find(call.vars[j]);
            if (defined != session.labelDefs.end() && labels != session.valueLabels.end() &&
                !std::isnan(value) && value == std::floor(value)) {
              auto hit = labels->second.find(static_cast<int>(value));
              if (hit != labels->second.end() && !hit->second.empty()) {
                std::string label = hit->second;
                if (label.size() > maxLength) {
                  size_t cut = maxLength;
                  while (cut > 0 && (static_cast<unsigned char>(label[cut]) & 0xC0) == 0x80) --cut;
                  label.resize(cut);
                }
                if (!label.empty()) {
                  text += label;
                  text += punct;
                  continue;
                }
              }
            }
          }
          try {
            text += numberAsText(value, format);
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

// skew, kurt, mdev, mad, fill, diff, rowfirst/rowlast (rfirst/rlast),
// rowmedian, rowpctile and ends (help egen; each ado's method, in our code).

// A string payload line for installColumn: quoted, an embedded quote doubled.
std::string quotedLine(const std::string& text) {
  std::string line = "\"";
  for (char c : text) {
    if (c == '"') line += '"';
    line += c;
  }
  return line + "\"\n";
}

// One value per row of the expression `rhs`, as generate evaluates it (a missing
// value of any kind is a NaN).
std::optional<ojson> evaluateColumn(SessionState& session, const std::string& rhs,
                                    std::vector<double>* out) {
  ScratchColumns scratch(session);
  std::string col;
  if (auto failed = scratch.add(rhs, "", &col)) return failed;
  *out = scratch.values(col);
  for (double& v : *out)
    if (std::fabs(v) >= eval::kStataMissingSentinel) v = std::numeric_limits<double>::quiet_NaN();
  return std::nullopt;
}

// Group number per row of the by-variables (equal values, a missing one
// included, share a group); every row is group 0 without a by-list.
std::optional<ojson> byGroups(SessionState& session, const std::string& byText,
                              std::vector<long long>* group) {
  const int64_t rows = session.datastore()->nRows();
  group->assign(static_cast<size_t>(rows), 0);
  if (trim(byText).empty()) return std::nullopt;
  loops::VarlistExpandResult expanded = loops::expandVarlist(byText, session);
  if (expanded.isError) return expanded.error;
  if (auto unknown = loops::checkKnownVarlist(expanded.tokens, session)) return unknown;
  Datastore* ds = session.datastore();
  std::map<std::vector<double>, long long> ids;
  std::vector<std::vector<double>> keys(static_cast<size_t>(rows));
  for (const std::string& var : expanded.tokens) {
    const cecon::Column& c = ds->col(var);
    if (c.dtype == cecon::DType::Factor) {
      const int32_t* codes = c.codes();
      for (int64_t i = 0; i < rows; ++i) {
        const int32_t code = codes[i];
        const std::string text = code >= 0 && static_cast<size_t>(code) < c.levels.size()
                                     ? c.levels[static_cast<size_t>(code)]
                                     : std::string();
        // Equal strings are one group: the key is the text's own hash and length.
        keys[static_cast<size_t>(i)].push_back(static_cast<double>(std::hash<std::string>{}(text)));
        keys[static_cast<size_t>(i)].push_back(static_cast<double>(text.size()));
      }
    } else {
      const double* v = c.f64();
      for (int64_t i = 0; i < rows; ++i) {
        double x = v[i];
        if (std::isnan(x)) x = eval::kStataMissingSentinel + 1.0;  // one key for a NaN
        keys[static_cast<size_t>(i)].push_back(x);
      }
    }
  }
  for (int64_t i = 0; i < rows; ++i) {
    auto it = ids.emplace(keys[static_cast<size_t>(i)], static_cast<long long>(ids.size())).first;
    (*group)[static_cast<size_t>(i)] = it->second;
  }
  return std::nullopt;
}

double medianOfSorted(const std::vector<double>& v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = v.size();
  return n % 2 == 1 ? v[(n - 1) / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// rowpctile's rule (also rowmedian's, p = 50): rank threshold n p / 100, the
// two neighbours averaged when it is a whole number (within 1e-8).
double rowPercentile(std::vector<double> v, double p) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  const double threshold = static_cast<double>(v.size()) * p / 100.0;
  double smallest = std::ceil(threshold);
  if (std::fabs(smallest - threshold) < 1e-8) smallest += 1.0;
  const size_t k = static_cast<size_t>(smallest);
  if (std::fabs(threshold - (smallest - 1.0)) < 1e-8) {
    if (k < 2 || k > v.size()) return std::numeric_limits<double>::quiet_NaN();
    return (v[k - 2] + v[k - 1]) / 2.0;
  }
  if (k < 1 || k > v.size()) return std::numeric_limits<double>::quiet_NaN();
  return v[k - 1];
}

std::optional<ojson> installTyped(SessionState& session, const std::string& newvar,
                                  const std::string& declared, std::vector<double> values) {
  const std::string type = declared.empty() ? std::string("float") : declared;
  for (double& v : values) v = heldIn(type, v);
  if (auto failed = installNumbers(session, newvar, values)) return failed;
  noteResultType(session, newvar, type, type);
  return std::nullopt;
}

// installTyped(), then the rows whose result is an extended missing value
// (.a-.z) are set to that value: the values file of the engine's column install
// carries numbers and `.` only.
std::optional<ojson> installTypedKeepingKinds(SessionState& session, const std::string& newvar,
                                              const std::string& declared,
                                              const std::vector<double>& values) {
  if (auto failed = installTyped(session, newvar, declared, values)) return failed;
  std::vector<double> kinds(values.size(), std::numeric_limits<double>::quiet_NaN());
  std::set<int> present;
  for (size_t i = 0; i < values.size(); ++i) {
    const int kind = cecon::missing_kind(values[i]);
    if (kind < 1) continue;
    kinds[i] = kind;
    present.insert(kind);
  }
  if (present.empty()) return std::nullopt;
  std::string marker;
  {
    int serial = 0;
    do marker = "__egenkind" + std::to_string(++serial);
    while (session.datastore()->has(marker));
  }
  if (auto failed = installNumbers(session, marker, kinds)) return failed;
  std::optional<ojson> failure;
  for (int kind : present) {
    ojson blocks = dispatch("quietly replace " + newvar + " = ." +
                                std::string(1, static_cast<char>('a' + kind - 1)) + " if " +
                                marker + " == " + std::to_string(kind),
                            session);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") failure = b;
    if (failure) break;
  }
  {
    SessionState::TempDropScope quietDrop(session);
    (void)dispatch("quietly drop " + marker, session);
  }
  return failure;
}

}  // namespace

namespace egen_detail {

bool egenMoreFunction(const std::string& fn) {
  static const std::set<std::string> kFns = {
      "skew",  "kurt",    "mdev",   "mad",       "fill",      "diff", "rowfirst",
      "rowlast", "rfirst", "rlast", "rowmedian", "rowpctile", "ends"};
  return kFns.count(fn) != 0;
}

std::optional<ojson> runEgenMoreFunction(const std::string& fnTyped, const std::string& newvar,
                                         const std::string& argText,
                                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                         const std::optional<std::string>& ifExpr,
                                         const std::optional<std::string>& inRange,
                                         const std::string& storeType, SessionState& session) {
  auto isMissingKey = [](const std::string& key) {
    return key.size() >= 4 && key.size() <= 7 && std::string("missing").compare(0, key.size(), key) == 0;
  };
  // max()/min() with `missing` (the forms without it are the engine's).
  const bool extremeWithMissing =
      (fnTyped == "max" || fnTyped == "min") &&
      std::any_of(opts.begin(), opts.end(), [&](const auto& kv) { return isMissingKey(kv.first); });
  if (!egenMoreFunction(fnTyped) && !extremeWithMissing) return std::nullopt;
  const std::string fn = fnTyped == "rfirst" ? "rowfirst" : fnTyped == "rlast" ? "rowlast" : fnTyped;
  const bool isMoment = fn == "skew" || fn == "kurt" || fn == "mdev";
  const bool isRow = fn == "rowfirst" || fn == "rowlast" || fn == "rowmedian" || fn == "rowpctile";
  const std::string arg = trim(argText);

  // Options, as each ado's syntax declares them.
  std::string byText;
  bool haveBy = false;
  double p = 50.0;
  std::string punct = " ";
  int endsForms = 0;
  bool head = false, tail = false, last = false, trimIt = false;
  auto abbrevOf = [](const std::string& key, const std::string& full, size_t minLen) {
    return key.size() >= minLen && key.size() <= full.size() && full.compare(0, key.size(), key) == 0;
  };
  for (const auto& [key, value] : opts) {
    if (key == "by") {
      haveBy = true;
      byText = value.isBool ? std::string() : value.strValue;
      continue;
    }
    if (extremeWithMissing && isMissingKey(key) && value.isBool) continue;
    if (fn == "rowpctile" && key == "p") {
      const std::optional<double> v = eval::pythonFloat(trim(value.strValue));
      if (value.isBool || !v.has_value()) return egenRcError("option p() incorrectly specified", 198);
      p = *v;
      continue;
    }
    if (fn == "ends") {
      if (abbrevOf(key, "punct", 1) && !value.isBool) {
        punct = value.strValue;
        if (punct.size() >= 2 && punct.front() == '"' && punct.back() == '"')
          punct = punct.substr(1, punct.size() - 2);
        if (punct.empty()) punct = " ";
        continue;
      }
      // ends()'s options abbreviate to h, l, t and tr.
      if (value.isBool && abbrevOf(key, "head", 1)) { head = true; ++endsForms; continue; }
      if (value.isBool && abbrevOf(key, "tail", 1)) { tail = true; ++endsForms; continue; }
      if (value.isBool && abbrevOf(key, "last", 1)) { last = true; ++endsForms; continue; }
      if (value.isBool && abbrevOf(key, "trim", 2)) { trimIt = true; continue; }
    }
    return ojson(eval::errorBlock("unsupported_option", "option " + key + " not allowed",
                                  std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  if (fn == "rowpctile" && (p <= 0.0 || p >= 100.0))
    return egenRcError("p(" + numberAsText(p, nullptr) + ") must be between 0 and 100", 198);
  if (haveBy && !(isMoment || fn == "mad" || extremeWithMissing))
    return egenRcError("egen ... " + fn + "() may not be combined with by", 190);
  if (fn == "ends" && endsForms > 1)
    return egenRcError("you can only specify one option, head, tail or last", 198);

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  const int64_t rows = ds->nRows();
  const ComputedEgen call{newvar, {}, ifExpr, inRange, storeType, session};
  std::vector<char> inside;
  const double kNaN = std::numeric_limits<double>::quiet_NaN();

  if (fn == "fill") {
    if (ifExpr.has_value() || inRange.has_value()) return egenRcError("invalid syntax", 198);
    std::vector<double> given;
    if (!expandEgenNumlist(arg, &given)) return egenRcError("invalid numlist", 121);
    if (given.size() < 2) return egenRcError("too few elements in numlist", 122);
    const std::string type = storeType.empty() ? std::string("float") : storeType;
    const size_t k = given.size();
    const size_t k2 = static_cast<size_t>(std::floor(static_cast<double>(k) / 2.0 + 0.5));
    const size_t k3 = k2 + 1;
    std::vector<double> v(static_cast<size_t>(rows), kNaN), d(static_cast<size_t>(rows), kNaN);
    for (size_t i = 0; i < k && i < v.size(); ++i) v[i] = heldIn(type, given[i]);
    for (size_t i = 0; i + 1 < v.size(); ++i) d[i] = heldIn("float", v[i + 1] - v[i]);
    for (size_t i = k3; i <= v.size(); ++i) d[i - 1] = i > k2 ? d[i - 1 - k2] : kNaN;
    for (size_t i = k3; i <= v.size(); ++i)
      if (i >= 2) v[i - 1] = heldIn(type, v[i - 2] + d[i - 2]);
    if (auto failed = installNumbers(session, newvar, v)) return *failed;
    noteResultType(session, newvar, type, type);
    return mutationResult(session, "egen", {newvar});
  }

  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;

  if (extremeWithMissing) {
    // `missing`: a missing value takes part in the comparison, as Stata orders
    // it (above every number; `.` < `.a` < ... < `.z`), over the rows of if/in.
    std::vector<double> x;
    {
      ScratchColumns scratch(session);
      std::string col;
      if (auto failed = scratch.add(arg, "", &col)) return *failed;
      x = scratch.values(col);
    }
    // Each missing value keeps its kind, so `.` < `.a` < ... < `.z` order it.
    for (double& v : x)
      if (std::isnan(v)) v = eval::stataMissingWithKind(std::max(0, cecon::missing_kind(v)));
    std::vector<long long> group;
    if (auto failed = byGroups(session, haveBy ? byText : std::string(), &group)) return *failed;
    std::map<long long, double> result;
    for (int64_t i = 0; i < rows; ++i) {
      if (!inside[static_cast<size_t>(i)]) continue;
      const double v = x[static_cast<size_t>(i)];
      auto it = result.find(group[static_cast<size_t>(i)]);
      if (it == result.end()) result[group[static_cast<size_t>(i)]] = v;
      else it->second = fn == "max" ? std::max(it->second, v) : std::min(it->second, v);
    }
    std::vector<double> out(static_cast<size_t>(rows), kNaN);
    for (int64_t i = 0; i < rows; ++i) {
      if (!inside[static_cast<size_t>(i)]) continue;
      const double v = result[group[static_cast<size_t>(i)]];
      out[static_cast<size_t>(i)] =
          v >= eval::kStataMissingSentinel
              ? cecon::stata_missing(eval::stataExtendedMissingKind(v).value_or(0))
              : v;
    }
    if (auto failed = installTypedKeepingKinds(session, newvar, storeType, out)) return *failed;
    return mutationResult(session, "egen", {newvar});
  }

  if (isMoment || fn == "mad") {
    std::vector<double> x;
    if (auto failed = evaluateColumn(session, arg, &x)) return *failed;
    std::vector<long long> group;
    if (auto failed = byGroups(session, haveBy ? byText : std::string(), &group)) return *failed;
    std::map<long long, std::vector<double>> members;
    for (int64_t i = 0; i < rows; ++i)
      if (inside[static_cast<size_t>(i)] && !std::isnan(x[static_cast<size_t>(i)]))
        members[group[static_cast<size_t>(i)]].push_back(x[static_cast<size_t>(i)]);
    std::map<long long, double> result;
    for (auto& [g, xs] : members) {
      const double n = static_cast<double>(xs.size());
      double value = kNaN;
      if (fn == "mad") {
        std::sort(xs.begin(), xs.end());
        const double med = medianOfSorted(xs);
        std::vector<double> dev;
        for (double v : xs) dev.push_back(std::fabs(v - med));
        std::sort(dev.begin(), dev.end());
        value = medianOfSorted(dev);
      } else {
        double sum = 0.0;
        for (double v : xs) sum += v;
        const double m = sum / n;
        double s2 = 0.0, s3 = 0.0, s4 = 0.0, sa = 0.0;
        for (double v : xs) {
          const double e = v - m;
          s2 += e * e;
          s3 += e * e * e;
          s4 += e * e * e * e;
          sa += std::fabs(e);
        }
        const double m2 = s2 / n, m3 = s3 / n, m4 = s4 / n;
        if (fn == "skew") value = m2 > 0.0 ? m3 / std::pow(std::sqrt(m2), 3) : kNaN;
        if (fn == "kurt") value = m2 > 0.0 ? m4 / (m2 * m2) : kNaN;
        if (fn == "mdev") value = sa / n;
      }
      result[g] = value;
    }
    std::vector<double> out(static_cast<size_t>(rows), kNaN);
    for (int64_t i = 0; i < rows; ++i) {
      if (!inside[static_cast<size_t>(i)]) continue;
      auto it = result.find(group[static_cast<size_t>(i)]);
      if (it != result.end()) out[static_cast<size_t>(i)] = it->second;
    }
    if (auto failed = installTyped(session, newvar, storeType, out)) return *failed;
    // What the ados leave behind. Without a by-list skew() and kurt() end on
    // their last moment (an r-class helper: r(moment) = m3, m4), mdev() on
    // `summarize ..., meanonly` of |x - mean|; an =/exp that is one variable is
    // s(varname) (the expression helper's sclass result; mad() reports it only
    // without if/in). Bank 1b59372e2a7bcbf8, 77c2c90d0b492ea7, 6a4a17baede56029.
    const bool qualified = (ifExpr.has_value() && !ifExpr->empty()) ||
                           (inRange.has_value() && !inRange->empty());
    const auto asVariable = isPlainVariableName(arg) ? resolveVarName(arg, session) : std::nullopt;
    if (asVariable.has_value() && (fn != "mad" || !qualified))
      session.eval.rStrings.set(std::string("\x01s:varname"), *asVariable);
    if (!haveBy && isMoment) {
      const std::vector<double>& xs = members[0];
      if (!xs.empty()) {
        const double n = static_cast<double>(xs.size());
        double sum = 0.0;
        for (double v : xs) sum += v;
        const double m = sum / n;
        eval::replaceRClass(session.eval);
        if (fn == "mdev") {
          double total = 0.0, lo = std::numeric_limits<double>::infinity(), hi = -lo;
          for (double v : xs) {
            const double a = std::fabs(v - m);
            total += a;
            lo = std::min(lo, a);
            hi = std::max(hi, a);
          }
          session.eval.rValues.set("N", n);
          session.eval.rValues.set("sum_w", n);
          session.eval.rValues.set("sum", total);
          session.eval.rValues.set("mean", total / n);
          session.eval.rValues.set("min", lo);
          session.eval.rValues.set("max", hi);
          eval::declareRPosted(session.eval, "egen", {"N", "sum_w", "sum", "mean", "min", "max"});
        } else {
          const int power = fn == "skew" ? 3 : 4;
          double moment = 0.0;
          for (double v : xs) moment += std::pow(v - m, power);
          session.eval.rValues.set("moment", moment / n);
          eval::declareRPosted(session.eval, "egen", {"moment"});
        }
      }
    }
    return mutationResult(session, "egen", {newvar});
  }

  // Varlist functions.
  loops::VarlistExpandResult expanded = loops::expandVarlist(arg, session);
  if (expanded.isError) return expanded.error;
  if (auto unknown = loops::checkKnownVarlist(expanded.tokens, session)) return unknown;
  const std::vector<std::string>& vars = expanded.tokens;
  ds = session.datastore();
  if (vars.empty()) return egenRcError("too few variables specified", 102);

  if (fn == "ends") {
    if (vars.size() > 1) return egenRcError("too many variables specified", 103);
    const cecon::Column& c = ds->col(vars.front());
    if (c.dtype != cecon::DType::Factor) return egenRcError("type mismatch", 109);
    const int32_t* codes = c.codes();
    std::string payload;
    size_t longest = 1;
    for (int64_t i = 0; i < rows; ++i) {
      const int32_t code = codes[i];
      const std::string text = code >= 0 && static_cast<size_t>(code) < c.levels.size()
                                   ? c.levels[static_cast<size_t>(code)]
                                   : std::string();
      std::string out;
      if (inside[static_cast<size_t>(i)] && !text.empty()) {
        if (last) {
          // The separator is found left to right, each search starting after
          // the previous match, and the text after the final match is kept
          // as it is: trim applies only to head and tail.
          size_t at = text.find(punct);
          while (at != std::string::npos) {
            const size_t next = text.find(punct, at + punct.size());
            if (next == std::string::npos) break;
            at = next;
          }
          out = at == std::string::npos ? text : text.substr(at + punct.size());
        } else {
          const size_t at = text.find(punct);
          if (tail) out = at == std::string::npos ? std::string() : text.substr(at + punct.size());
          else out = at == std::string::npos ? text : text.substr(0, at);
          if (trimIt) out = trim(out);
        }
      }
      longest = std::max(longest, out.size());
      payload += quotedLine(out);
    }
    if (auto failed = installColumn(session, newvar, true, payload)) return *failed;
    session.implicitFloatVars.erase(newvar);
    session.varMeta[newvar].stataType =
        longest > 2045 ? std::string("strL") : "str" + std::to_string(longest);
    return mutationResult(session, "egen", {newvar});
  }

  std::vector<const double*> cols;
  for (const std::string& var : vars) {
    if (ds->dtype(var) != "f64") {
      if (fn == "rowmedian" || fn == "rowpctile")
        return egenRcError("string variable " + var + " not allowed", 109);
      if (fn != "diff") return egenRcError("type mismatch", 109);
      return ojson(eval::unsupportedCommand("egen diff() with string variables", std::nullopt,
                                            std::nullopt, "refused:egen_diff_string",
                                            eval::UnsupportedCommandKind::kNotImplemented));
    }
    cols.push_back(ds->col(var).f64());
  }
  if (fn == "diff" && vars.size() < 2) return egenRcError("too few variables specified", 102);
  auto miss = [](double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; };
  std::vector<double> out(static_cast<size_t>(rows), kNaN);
  for (int64_t i = 0; i < rows; ++i) {
    if (!inside[static_cast<size_t>(i)]) continue;
    const size_t r = static_cast<size_t>(i);
    if (fn == "diff") {
      // 1 when any later variable differs from the first (missing values compare as values).
      double flag = 0.0;
      for (size_t j = 1; j < cols.size(); ++j) {
        const double a = cols[0][r], b = cols[j][r];
        const bool equal = (miss(a) && miss(b)) ? (std::isnan(a) ? eval::kStataMissingSentinel : a) ==
                                                      (std::isnan(b) ? eval::kStataMissingSentinel : b)
                                                : a == b;
        if (!equal) flag = 1.0;
      }
      out[r] = flag;
    } else if (fn == "rowfirst" || fn == "rowlast") {
      // rowfirst: the first non-missing value from the left; rowlast: from the
      // right. A row with none keeps the missing value of the variable the scan
      // starts from (rowfirst's last variable, rowlast's first), .a-.z included.
      const bool fromLeft = fn == "rowfirst";
      double v = cols[fromLeft ? cols.size() - 1 : 0][r];
      for (size_t j = 0; j < cols.size(); ++j) {
        const double c = cols[fromLeft ? j : cols.size() - 1 - j][r];
        if (!miss(c)) {
          v = c;
          break;
        }
      }
      if (miss(v)) v = std::isnan(v) ? v : cecon::stata_missing(eval::stataExtendedMissingKind(v).value_or(0));
      out[r] = v;
    } else {
      std::vector<double> present;
      for (const double* c : cols)
        if (!miss(c[r])) present.push_back(c[r]);
      out[r] = rowPercentile(present, fn == "rowmedian" ? 50.0 : p);
    }
  }
  if (fn == "diff") {
    if (auto failed = installNumbers(session, newvar, out)) return *failed;
    const std::string type = storeType.empty() ? std::string("float") : storeType;
    noteResultType(session, newvar, type, type);
    session.setVarLabel(newvar, "diff " + [&] {
      std::string names;
      for (const auto& v : vars) names += (names.empty() ? "" : " ") + v;
      return names;
    }());
    return mutationResult(session, "egen", {newvar});
  }
  if (auto failed = installTypedKeepingKinds(session, newvar, storeType, out)) return *failed;
  return mutationResult(session, "egen", {newvar});
}

std::optional<ojson> egenWriteColumn(SessionState& session, const std::string& var,
                                     const std::vector<double>& values, bool isNew,
                                     const std::string& storeType) {
  const std::string minted = mintScratchPath(session.workdir(), "egenval");
  forgetScratchMint(minted);
  const std::string valuesPath = minted.substr(0, minted.size() - 5) + ".csv";  // ".hbin"
  registerScratchMint(valuesPath);
  {
    std::ofstream file(cecon::to_native_path(valuesPath), std::ios::binary);
    char digits[40];
    for (double v : values) {
      if (!std::isnan(v)) {
        std::snprintf(digits, sizeof digits, "%.17g", v);
        file << digits;
      }
      file << '\n';
    }
    file.flush();
    if (!file) {
      cecon::native_unlink(valuesPath);
      return egenRcError("could not write the values of " + var + " (disk full or I/O error)", 693);
    }
  }
  const std::string outPath = nextPath(session.workdir(), "egenval");
  std::vector<std::string> argv = {"setcol", "--data", *session.currentHbin(), "--var", var,
                                   "--values", valuesPath, "--dtype", "f64"};
  if (isNew) argv.push_back("--new");
  if (!storeType.empty()) {
    argv.push_back("--store-type");
    argv.push_back(storeType);
  }
  argv.push_back("--out");
  argv.push_back(outPath);
  const CobraRunResult run = runCobra(argv);
  cecon::native_unlink(valuesPath);
  if (!run.ok) {
    cecon::native_unlink(outPath);
    return engineErrorBlock(cobraErrorStr(errorFromRaw(run)));
  }
  if (!cecon::native_exists(outPath))
    return egenRcError("could not create " + var + " (disk full or I/O error)", 693);
  session.load(outPath);
  if (!storeType.empty()) {
    session.implicitFloatVars.erase(var);
    session.varMeta[var].stataType = storeType;
  }
  return std::nullopt;
}

bool gegenOwnFunction(const std::string& fn) {
  static const std::set<std::string> kFns = {
      "nunique", "select",   "cv",       "first",      "firstnm",   "last",    "lastnm",
      "geomean", "kurtosis", "skewness", "nansum",     "nmissing",  "percent", "range",
      "semean",  "sebinomial", "sepoisson", "variance"};
  return kFns.count(fn) != 0;
}

// gtools' gegen statistics that egen has no function for, per by-group over the
// if/in rows, by the same definitions gcollapse uses (src/data/frame.cpp's
// collapse): sample sd for cv/semean/variance, population moments for
// skewness/kurtosis, first/last raw and firstnm/lastnm non-missing, percent of
// the non-missing count of the whole sample, select(), n(#) the #th smallest
// (n(-#) the #th largest).
std::optional<ojson> runGegenFunction(const std::string& fn, const std::string& newvar,
                                      const std::string& argText,
                                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                      const std::optional<std::string>& ifExpr,
                                      const std::optional<std::string>& inRange,
                                      const std::string& storeType, SessionState& session) {
  if (!gegenOwnFunction(fn)) return std::nullopt;
  std::string byText;
  long long pick = 0;
  bool havePick = false;
  for (const auto& [key, value] : opts) {
    if (key == "by") {
      byText = value.isBool ? std::string() : value.strValue;
      continue;
    }
    if (fn == "select" && key == "n" && !value.isBool) {
      const std::optional<double> v = eval::pythonFloat(trim(value.strValue));
      if (!v.has_value() || *v != std::floor(*v) || *v == 0.0)
        return egenRcError("option n() incorrectly specified", 198);
      pick = static_cast<long long>(*v);
      havePick = true;
      continue;
    }
    return ojson(eval::errorBlock("unsupported_option", "option " + key + " not allowed",
                                  std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  // gtools: select() without n() is r(110) (vs-stata bank e70a448ff5a6472e).
  if (fn == "select" && !havePick)
    return egenRcError("n() should be a positive or negative integer", 110);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  const int64_t rows = ds->nRows();
  const ComputedEgen call{newvar, {}, ifExpr, inRange, storeType, session};
  std::vector<char> inside;
  if (auto refusal = flagQualifiedRows(call, &inside)) return *refusal;
  std::vector<double> x;
  if (auto failed = evaluateColumn(session, trim(argText), &x)) return *failed;
  std::vector<long long> group;
  if (auto failed = byGroups(session, byText, &group)) return *failed;
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::map<long long, std::vector<int64_t>> members;
  double sampleCount = 0.0;
  for (int64_t i = 0; i < rows; ++i) {
    if (!inside[static_cast<size_t>(i)]) continue;
    members[group[static_cast<size_t>(i)]].push_back(i);
    if (!std::isnan(x[static_cast<size_t>(i)])) sampleCount += 1.0;
  }
  std::map<long long, double> result;
  for (const auto& [g, rs] : members) {
    std::vector<double> s;
    for (int64_t r : rs)
      if (!std::isnan(x[static_cast<size_t>(r)])) s.push_back(x[static_cast<size_t>(r)]);
    const double n = static_cast<double>(s.size());
    double sum = 0.0;
    for (double v : s) sum += v;
    const double mean = n > 0 ? sum / n : kNaN;
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (double v : s) {
      const double d = v - mean;
      m2 += d * d;
      m3 += d * d * d;
      m4 += d * d * d * d;
    }
    double agg = kNaN;
    if (fn == "first") agg = x[static_cast<size_t>(rs.front())];
    else if (fn == "last") agg = x[static_cast<size_t>(rs.back())];
    else if (fn == "firstnm") agg = n > 0 ? s.front() : kNaN;
    else if (fn == "lastnm") agg = n > 0 ? s.back() : kNaN;
    else if (fn == "nmissing") agg = static_cast<double>(rs.size()) - n;
    else if (fn == "nansum") agg = n > 0 ? sum : kNaN;
    else if (fn == "percent") agg = sampleCount > 0 ? n / sampleCount * 100.0 : kNaN;
    else if (fn == "nunique") {
      std::sort(s.begin(), s.end());
      agg = static_cast<double>(std::unique(s.begin(), s.end()) - s.begin());
    } else if (n > 0) {
      if (fn == "range") {
        agg = *std::max_element(s.begin(), s.end()) - *std::min_element(s.begin(), s.end());
      } else if (fn == "variance") {
        if (n >= 2) agg = m2 / (n - 1);
      } else if (fn == "cv") {
        if (n >= 2) agg = std::sqrt(m2 / (n - 1)) / mean;
      } else if (fn == "semean") {
        if (n >= 2) agg = (1.0 / std::sqrt(n)) * std::sqrt(m2 / (n - 1));
      } else if (fn == "skewness" || fn == "kurtosis") {
        const double p2 = m2 / n, p3 = m3 / n, p4 = m4 / n;
        if (p2 > 0) agg = fn == "skewness" ? p3 / std::pow(p2, 1.5) : p4 / (p2 * p2);
      } else if (fn == "sebinomial") {
        bool ok = true;
        for (double v : s) ok = ok && (v == 0.0 || v == 1.0);
        if (ok) agg = std::sqrt(mean * (1.0 - mean) / n);
      } else if (fn == "sepoisson") {
        bool ok = true;
        for (double v : s) ok = ok && v >= 0.0;
        if (ok) agg = std::sqrt(std::floor(mean * n + 0.5)) / n;
      } else if (fn == "geomean") {
        bool ok = true;
        double logs = 0.0;
        for (double v : s) {
          ok = ok && v >= 0.0;
          logs += std::log(v);
        }
        if (ok) agg = std::exp(logs / n);
      } else if (fn == "select") {
        std::sort(s.begin(), s.end());
        const long long k = pick < 0 ? -pick : pick;
        if (k <= static_cast<long long>(s.size()))
          agg = pick > 0 ? s[static_cast<size_t>(k - 1)] : s[s.size() - static_cast<size_t>(k)];
      }
    }
    result[g] = agg;
  }
  std::vector<double> out(static_cast<size_t>(rows), kNaN);
  for (int64_t i = 0; i < rows; ++i)
    if (inside[static_cast<size_t>(i)]) out[static_cast<size_t>(i)] = result[group[static_cast<size_t>(i)]];
  if (auto failed = installTyped(session, newvar, storeType, out)) return *failed;
  return mutationResult(session, "egen", {newvar});
}

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
  const bool decodeOpt = optionNamed(opts, vocab, "decode") != nullptr;
  const eval::OptValue* maxLengthOpt = optionNamed(opts, vocab, "maxlength");
  if (isConcat && punctOpt == nullptr && formatOpt == nullptr && !decodeOpt &&
      maxLengthOpt == nullptr)
    return std::nullopt;

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
    // maxlength(#): an integer from 1 to c(maxvlabellen); read even without decode.
    size_t maxLength = 32000;
    if (maxLengthOpt != nullptr && !maxLengthOpt->isBool && !trim(maxLengthOpt->strValue).empty()) {
      const std::optional<double> n = eval::pythonFloat(trim(maxLengthOpt->strValue));
      if (!n.has_value() || std::isnan(*n) || *n != std::floor(*n) || *n < 1 || *n > 32000)
        return egenRcError("invalid maxlength( )", 198);
      maxLength = static_cast<size_t>(*n);
    }
    return computeConcat(call, punct, format.empty() ? nullptr : &format, decodeOpt, maxLength);
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

void gtoolsPublishGroups(SessionState& session, const std::vector<std::string>& groupVars,
                         const std::optional<std::string>& ifExpr,
                         const std::optional<std::string>& inRange, bool dropMissingKeys,
                         bool postN, const char* command) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return;
  try {
    const std::string none;
    const std::vector<std::string> noVars;
    const ComputedEgen sample{none, noVars, ifExpr, inRange, none, session};
    std::vector<char> inside;
    if (flagQualifiedRows(sample, &inside)) return;
    ds = session.datastore();
    std::vector<const cecon::Column*> cols;
    for (const std::string& v : groupVars) {
      if (!ds->has(v)) return;
      cols.push_back(&ds->col(v));
    }
    std::map<std::vector<uint64_t>, int64_t> sizes;
    int64_t used = 0;
    for (size_t row = 0; row < inside.size(); ++row) {
      if (!inside[row]) continue;
      std::vector<uint64_t> key;
      bool missingKey = false;
      for (const cecon::Column* c : cols) {
        if (c->dtype == cecon::DType::Factor) {
          const int32_t code = c->codes()[row];
          if (code < 0 || c->levels[static_cast<size_t>(code)].empty()) missingKey = true;
          key.push_back(static_cast<uint64_t>(static_cast<int64_t>(code)));
        } else {
          double v = c->f64()[row];
          if (std::isnan(v)) missingKey = true;
          if (v == 0.0) v = 0.0;  // -0 and 0 are one group
          uint64_t bits = 0;
          std::memcpy(&bits, &v, sizeof bits);
          key.push_back(bits);
        }
      }
      if (missingKey && dropMissingKeys) continue;
      ++used;
      ++sizes[key];
    }
    int64_t lo = 0, hi = 0;
    for (const auto& kv : sizes) {
      lo = lo == 0 ? kv.second : std::min(lo, kv.second);
      hi = std::max(hi, kv.second);
    }
    if (postN) session.eval.rValues.set("N", static_cast<double>(used));
    session.eval.rValues.set("J", static_cast<double>(sizes.size()));
    session.eval.rValues.set("minJ", static_cast<double>(lo));
    session.eval.rValues.set("maxJ", static_cast<double>(hi));
    if (postN) eval::declareRPosted(session.eval, command, {"N", "J", "minJ", "maxJ"});
    else eval::declareRPosted(session.eval, command, {"J", "minJ", "maxJ"});
  } catch (const std::exception&) {
  }
}

void gtoolsPublishExtremes(SessionState& session, const std::string& var,
                           const std::optional<std::string>& ifExpr,
                           const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "f64") return;
  const std::string none;
  const std::vector<std::string> noVars;
  const ComputedEgen sample{none, noVars, ifExpr, inRange, none, session};
  std::vector<char> inside;
  if (flagQualifiedRows(sample, &inside)) return;
  ds = session.datastore();
  const double* x = ds->col(var).f64();
  std::vector<double> values;
  for (size_t row = 0; row < inside.size(); ++row)
    if (inside[row] && !std::isnan(x[row])) values.push_back(x[row]);
  std::sort(values.begin(), values.end());
  const double miss = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::string> names;
  for (int k = 1; k <= 4; ++k) {
    const size_t i = static_cast<size_t>(k - 1);
    const std::string lo = "smallest" + std::to_string(k), hi = "largest" + std::to_string(k);
    session.eval.rValues.set(lo, i < values.size() ? values[i] : miss);
    session.eval.rValues.set(hi, i < values.size() ? values[values.size() - 1 - i] : miss);
    names.push_back(lo);
    names.push_back(hi);
  }
  eval::declareRPosted(session.eval, "gstats", names);
}

}  // namespace piet::handlers
