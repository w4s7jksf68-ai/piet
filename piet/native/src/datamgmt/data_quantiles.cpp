// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data quantiles.
#include "data_io_internal.h"
#include "piet/handlers/data_values.h"
namespace piet::handlers::data_io_detail {


bool parseIntOptDV(const std::string& text, int& out) {
  if (text.empty()) return false;
  size_t pos = 0;
  long v = 0;
  try {
    v = std::stol(text, &pos);
  } catch (...) {
    return false;
  }
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos != text.size()) return false;
  out = static_cast<int>(v);
  return true;
}


// Does the live shadow card (or, failing that, the datastore) already carry this
// name? Used to refuse overwriting an existing column, matching Stata's r(110).
bool columnExistsDV(SessionState& session, const std::string& name) {
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card)
      if (info.name == name) return true;
    return false;
  }
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}


// A refused new-variable name under Stata's own code (strutil::NameRefusal).
ojson nameRefusalBlock(const strutil::NameRefusal& r) {
  return ojson(eval::errorBlock("syntax_error", r.message, std::nullopt, std::nullopt,
                                std::nullopt, "r(" + std::to_string(r.rc) + ")"));
}


ojson newVarGuardDV(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return engineErrorBlock(
        verb + ": invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  // BUG-1120: `tssmooth ma _cons = x, window(1)' is "_cons invalid varname" r(198)
  // (StataBE 19.5); ipolate and expand answer the rest of the rule first.
  if (strutil::isReservedVarName(newvar))
    return nameRefusalBlock(strutil::NameRefusal{198, newvar + " invalid varname"});
  if (columnExistsDV(session, newvar))
    return engineErrorBlock(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // empty => no objection
}




// `[type] newvar` as typed before the `=`. A storage type in front of a name is
// accepted and ignored; the same word alone is what Stata's syntax reads as a
// misplaced type, hence its sentence about n() (StataBE 19.5).
std::optional<ojson> readXtileTarget(const std::string& typed, std::string* name) {
  std::vector<std::string> words;
  std::istringstream splitter(typed);
  for (std::string word; splitter >> word;) words.push_back(word);
  if (words.size() == 2 && strutil::isStorageTypeWord(words[0])) words.erase(words.begin());
  if (words.size() > 1) return rcErrorBlock("too many variables specified", 103);
  if (words.empty() || words[0] == "if") return rcErrorBlock("varlist required", 100);
  const std::string& word = words[0];
  if (word == "_all" || word == "_skip") return rcErrorBlock(word + " not allowed", 101);
  if (strutil::isStorageTypeWord(word))
    return rcErrorBlock("option n() incorrectly specified", 198);
  if (strutil::isReservedVarName(word) || word == "in" || word == "using" || word == "with")
    return rcErrorBlock(word + " invalid varname", 198);
  if (!strutil::isValidStataIdentifier(word)) return rcErrorBlock(word + " invalid name", 198);
  *name = word;
  return std::nullopt;
}


// cutpoints(): one numeric variable, abbreviations as anywhere in a varlist.
std::optional<ojson> readXtileCutVariable(const eval::OptValue& option, SessionState& session,
                                          std::string* resolved) {
  const std::string typed = option.isBool ? std::string() : trim(option.strValue);
  if (typed.empty()) return rcErrorBlock("option cutpoints() incorrectly specified", 198);
  if (typed.find_first_of(" \t") != std::string::npos)
    return rcErrorBlock("cutpoints():  too many variables specified", 103);
  bool ambiguous = false;
  const auto found = resolveVarName(typed, session, &ambiguous);
  if (ambiguous) return rcErrorBlock(typed + " ambiguous abbreviation\n(error in option cutpoints())", 111);
  if (!found.has_value())
    return rcErrorBlock("variable " + typed + " not found\n(error in option cutpoints())", 111);
  if (session.datastore()->col(*found).dtype == cecon::DType::Factor)
    return rcErrorBlock("string variables not allowed in option cutpoints();\n" + *found +
                            " is a string variable",
                        109);
  *resolved = *found;
  return std::nullopt;
}


// nquantiles(): `2`, `2.0` and `3.` are integers as written; `2.5` and `1e1` are
// not; a bare word is evaluated as a name, which is how `n(abc)` comes to be
// "abc not found". `shown` keeps the number as typed, for the variable label.
std::optional<ojson> readXtileCount(const eval::OptValue& option, SessionState& session,
                                    long long* count, std::string* shown) {
  const ojson malformed = rcErrorBlock("option n() incorrectly specified", 198);
  if (option.isBool) return malformed;
  const std::string typed = trim(option.strValue);
  if (typed.empty()) return std::nullopt;  // n() keeps the default
  if (strutil::isValidStataIdentifier(typed)) {
    if (resolveVarName(typed, session).has_value()) return malformed;
    return rcErrorBlock(typed + " not found", 111);
  }
  const size_t digitsFrom = (typed[0] == '+' || typed[0] == '-') ? 1 : 0;
  const size_t point = typed.find('.');
  const std::string whole = typed.substr(digitsFrom, point == std::string::npos ? point : point - digitsFrom);
  const std::string fraction = point == std::string::npos ? std::string() : typed.substr(point + 1);
  if (whole.empty() || whole.find_first_not_of("0123456789") != std::string::npos ||
      fraction.find_first_not_of('0') != std::string::npos)
    return malformed;
  *count = std::strtoll(whole.c_str(), nullptr, 10);
  if (typed[0] == '-') *count = -*count;
  *shown = typed;
  return std::nullopt;
}


// Stata's limit on a variable label, in characters.
std::string clippedVarLabel(const std::string& text) {
  constexpr size_t kLabelCharacters = 80;
  size_t characters = 0;
  for (size_t at = 0; at < text.size(); ++at) {
    if ((static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) continue;
    if (characters++ == kLabelCharacters) return text.substr(0, at);
  }
  return text;
}


// The smallest integer type that holds the result's largest category.
const char* xtileStorageType(double largestCategory) {
  if (largestCategory <= 100.0) return "byte";
  if (largestCategory <= 32740.0) return "int";
  return "long";
}
}
namespace piet::handlers {
using namespace data_io_detail;


ojson runXtile(const std::string& newvar, const std::string& srcvar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session,
               const std::optional<std::string>& weightType,
               const std::optional<std::string>& weightVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (srcvar.empty())
    return engineErrorBlock("xtile: syntax is `xtile newvar = varname, nquantiles(#)`",
                            "syntax_error");
  ojson guard = newVarGuardDV(session, "xtile", newvar);
  if (!guard.is_null() && !guard.empty()) return guard;
  if (ifExpr.has_value() && !ifExpr->empty()) {
    // NOT a wiring shortcut: with `if`, Stata computes the CUTPOINTS from the
    // restricted sample only. Forwarding a --keepif would instead compute them
    // from every row and then blank the rest — the same bins with a different
    // name, which is a wrong number. Refused with the recipe until the engine
    // grows a restricted-sample form.
    return engineErrorBlock(
        "xtile with `if` is not implemented: Stata derives the quantile cutpoints from the "
        "restricted sample, and this engine computes them over all rows. Restrict first "
        "(`preserve` + `keep if ...`, or a materialized subset frame), then xtile.",
        "unimplemented");
  }
  int nq = kXtileDefaultNq;
  bool nqGiven = false;
  std::string cutvar;
  for (const auto& [k, v] : opts) {
    if (k == "nquantiles") {
      if (v.isBool || !parseIntOptDV(v.strValue, nq))
        return engineErrorBlock("xtile: nquantiles() requires a whole number", "syntax_error");
      nqGiven = true;
    } else if (k == "cutpoints") {
      // BUG-371: the bin edges come from another variable's values instead of from
      // quantiles of srcvar. A different SOURCE of edges, not a modifier — Stata
      // treats the two as mutually exclusive and so does this.
      if (v.isBool)
        return engineErrorBlock("xtile: cutpoints() requires a variable name", "syntax_error");
      cutvar = trim(v.strValue);
    }
  }
  if (!cutvar.empty() && nqGiven)
    return engineErrorBlock(
        "xtile: nquantiles() and cutpoints() are alternatives — cutpoints() supplies the bin "
        "edges directly, so there are no quantiles left to ask for.",
        "bad_input");
  if (cutvar.empty() && nq < 2)
    return engineErrorBlock("xtile: nquantiles() must be at least 2", "bad_input");
  if (!cutvar.empty() && !columnExistsDV(session, cutvar))
    return engineErrorBlock("variable " + cutvar + " not found", "unknown_variable");
  if (weightType.has_value() && !weightType->empty() && weightVar.has_value() &&
      !weightVar->empty()) {
    // Quantile cutpoints are the weighted percentiles
    // (pctile.cpp runXtileWeighted). With cutpoints() xtile.ado's
    // MakeCut ignores the weight but `mark` still drops the zero- and
    // missing-weight rows, which the engine's --xtile cannot do: refused by
    // name rather than categorizing rows Stata leaves missing.
    if (!cutvar.empty())
      return engineErrorBlock(
          "xtile: cutpoints() with a weight is not supported in this version of PIET.",
          "unsupported_option");
    return runXtileWeighted(newvar, srcvar, nq, *weightVar, *weightType, session);
  }

  std::string outPath = nextPath(session.workdir(), "xtile");
  const std::string spec =
      cutvar.empty() ? (newvar + "=" + srcvar + ", nq(" + std::to_string(nq) + ")")
                     : (newvar + "=" + srcvar + ", cutpoints(" + cutvar + ")");
  std::vector<std::string> argv = {"eval",  "--data", *session.currentHbin(),
                                   "--xtile", spec,
                                   "--out", outPath};
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(argv);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "gen", {newvar});
}


// The `xtile` command, [D] pctile: the checks in the order Stata answers them when
// several faults meet (black-box oracle in the item's folder), the sample as a
// source column that is missing outside it, and `cobra eval --xtile` for the
// quantile work.
ojson runXtileStata(const std::string& newvar, const std::string& expText,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange,
                    const std::optional<std::string>& weightVar, SessionState& session,
                    const std::optional<std::string>& weightType) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::string target;
  if (auto refusal = readXtileTarget(newvar, &target)) return *refusal;
  if (columnExistsDV(session, target))
    return rcErrorBlock("variable " + target + " already defined", 110);

  const eval::OptValue* countOption = nullptr;
  const eval::OptValue* cutOption = nullptr;
  bool countRepeated = false, cutRepeated = false;
  for (const auto& [key, value] : opts) {
    if (key == "nquantiles") {
      countRepeated = countRepeated || countOption != nullptr;
      countOption = &value;
    } else if (key == "cutpoints") {
      cutRepeated = cutRepeated || cutOption != nullptr;
      cutOption = &value;
    }
  }

  std::string cutVariable;
  if (cutOption != nullptr)
    if (auto refusal = readXtileCutVariable(*cutOption, session, &cutVariable)) return *refusal;

  // if/in and exp, evaluated by generate: its "not found" and range answers are the
  // ones Stata gives here, and they outrank everything about the options.
  std::string qualifier;
  if (ifExpr.has_value() && !trim(*ifExpr).empty()) qualifier += " if " + *ifExpr;
  if (inRange.has_value() && !trim(*inRange).empty()) qualifier += " in " + *inRange;

  ScratchColumns scratch(session);
  std::string restrictionColumn;
  if (!qualifier.empty())
    if (auto failed = scratch.add("1", qualifier, &restrictionColumn)) return *failed;

  std::string sourceColumn;
  bool sourceIsString = false;
  if (auto failed = scratch.add(expText, qualifier, &sourceColumn)) {
    if (eval::coarseRc(*failed) != "109") return *failed;
    sourceIsString = true;
  }

  if (countOption != nullptr && cutOption != nullptr)
    return rcErrorBlock("both nquantiles() and cutpoints() cannot be specified", 198);

  const bool weighted = weightVar.has_value() && !weightVar->empty();
  std::string weightColumn;
  if (weighted)
    if (auto failed = scratch.add(*weightVar, "", &weightColumn)) return *failed;
  if (sourceIsString) return rcErrorBlock("type mismatch", 109);

  const auto rowCount = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> restricted(rowCount, 1);
  if (!restrictionColumn.empty()) {
    const auto marks = scratch.values(restrictionColumn);
    for (size_t row = 0; row < rowCount; ++row) restricted[row] = std::isnan(marks[row]) ? 0 : 1;
  }

  // Weight values: over the whole dataset for quantiles, over the restriction for
  // cutpoints() (measured; [D] pctile documents neither).
  std::vector<double> weights;
  if (weighted) {
    weights = scratch.values(weightColumn);
    const std::string kind = weightType.value_or("");
    const bool frequencyWeight = kind == "fw" || kind == "fweight";
    bool negative = false, fractional = false;
    for (size_t row = 0; row < rowCount; ++row) {
      if (std::isnan(weights[row]) || (cutOption != nullptr && !restricted[row])) continue;
      negative = negative || weights[row] < 0.0;
      fractional = fractional || weights[row] != std::floor(weights[row]);
    }
    if (negative) return rcErrorBlock("negative weights encountered", 402);
    if (frequencyWeight && fractional)
      return rcErrorBlock("may not use noninteger frequency weights", 401);
  }

  long long quantileCount = kXtileDefaultNq;
  std::string countShown = std::to_string(kXtileDefaultNq);
  if (countRepeated) return rcErrorBlock("option n() not allowed", 198);
  if (cutRepeated) return rcErrorBlock("option cutpoints() not allowed", 198);
  if (countOption != nullptr)
    if (auto refusal = readXtileCount(*countOption, session, &quantileCount, &countShown))
      return *refusal;

  // The marked sample: if, in, and a positive non-missing weight. A marked row
  // whose exp is missing still counts towards N.
  const auto sourceCells = scratch.values(sourceColumn);
  long long markedRows = 0, usableRows = 0;
  bool weightDropsARow = false;
  for (size_t row = 0; row < rowCount; ++row) {
    if (!restricted[row]) continue;
    if (weighted && !(weights[row] > 0.0)) {
      weightDropsARow = true;
      continue;
    }
    ++markedRows;
    if (!std::isnan(sourceCells[row])) ++usableRows;
  }
  if (cutOption == nullptr) {
    if (quantileCount < 2)
      return rcErrorBlock("nquantiles() must be greater than or equal to 2", 198);
    if (quantileCount > markedRows + 1)
      return rcErrorBlock(
          "nquantiles() must be less than or equal to number of observations plus one", 198);
  }
  if (usableRows == 0) return rcErrorBlock("no observations", 2000);

  if (weighted && weightDropsARow) {
    const std::string keep = sourceColumn;
    if (auto failed = scratch.add(keep, " if " + weightColumn + " > 0 & " + weightColumn + " < .",
                                  &sourceColumn))
      return *failed;
  }

  std::string engineCall = target + "=" + sourceColumn + ", ";
  if (cutOption != nullptr) {
    engineCall += "cutpoints(" + cutVariable + ")";
  } else {
    engineCall += "nq(" + std::to_string(quantileCount) + ")";
    if (weighted) engineCall += " w(" + weightColumn + ")";
  }
  (void)session.datastore();  // the head file the engine reads
  const std::string written = nextPath(session.workdir(), "xtile");
  {
    SessionState::ResidentEligible resident(session);
    const CobraRunResult run =
        runCobra({"eval", "--data", *session.currentHbin(), "--xtile", engineCall, "--out", written});
    if (!run.ok) {
      const CobraError failure = errorFromRaw(run);
      return engineErrorBlock(cobraErrorStr(failure), failure.code);
    }
  }
  session.load(written);
  scratch.dropAll();

  const Datastore* result = session.datastore();
  const double* categories = result->col(target).f64();
  double largest = 0.0;
  for (size_t row = 0; row < rowCount; ++row)
    if (categories[row] > largest) largest = categories[row];  // false for a missing category

  VarMeta& meta = session.varMeta[target];
  meta.stataType = xtileStorageType(largest);
  meta.format = "%8.0g";
  meta.label = clippedVarLabel(cutOption != nullptr
                                   ? expText + " categorized by " + cutVariable
                                   : countShown + " quantiles of " + expText);
  scratch.noteDataChanged();
  return mutationResult(session, "gen", {target});
}
}
