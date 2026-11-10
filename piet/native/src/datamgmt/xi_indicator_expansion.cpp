// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xi and the xi: prefix: indicator-variable expansion.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "estimation/statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// ===========================================================================
// xi -- indicator expansion, command form and prefix form
//
// Matches Stata's xi; the option arrangement, the letter sequence and the omit and
// listing-width rules were identified from its ado, the rest measured on StataBE 19.5
// (the help file is the only documentation left; xi has no manual entry since Stata
// 19). Two choices are PIET's own: generated
// columns are f64 with format %9.0g rather than byte, and a name stem cut in
// bytes backs off to a UTF-8 character boundary rather than splitting one.

namespace {

// Dataset characteristics that remember, across runs and save/use, which columns
// an earlier run generated and under which prefix. A new run removes only those,
// so a user's own `_Ifoo` survives.
const char kXiMadeChar[] = "__xi__Vars__To__Drop__";
const char kXiStubChar[] = "__xi__Vars__Prefix__";

bool xiCellMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

bool xiHasFactorTag(const std::string& word) {
  return word.size() >= 2 && (word[0] == 'i' || word[0] == 'I') && word[1] == '.';
}

// Blank-separated words; blanks inside parentheses do not separate (`prefix( _S )`).
std::vector<std::string> xiWords(const std::string& text) {
  std::vector<std::string> words;
  std::string word;
  int nesting = 0;
  for (char ch : text) {
    if (ch == '(') ++nesting;
    if (ch == ')') --nesting;
    if (nesting <= 0 && std::isspace(static_cast<unsigned char>(ch))) {
      if (!word.empty()) words.push_back(std::move(word));
      word.clear();
      continue;
    }
    word += ch;
  }
  if (!word.empty()) words.push_back(std::move(word));
  return words;
}

// The leading `bytes` bytes of a name, shortened further if the cut would land
// inside a multibyte character.
std::string xiStem(const std::string& name, size_t bytes) {
  if (name.size() <= bytes) return name;
  while (bytes > 0 && (static_cast<unsigned char>(name[bytes]) & 0xC0) == 0x80) --bytes;
  return name.substr(0, bytes);
}

int xiChars(const std::string& s) { return static_cast<int>(cecon::ident::codePointCount(s)); }

// Stata's abbrev(): widths under 5 act as 5; a trailing digit run stays whole when
// it fits beside at least one leading character, otherwise only the last character
// survives after the tilde. The dataset-aware variant (avoiding an abbreviation two
// variables share) is not reproduced.
std::string xiShorten(const std::string& name, int width) {
  const int room = std::max(width, 5);
  if (xiChars(name) <= room) return name;
  size_t digitStart = name.size();
  while (digitStart > 0 && std::isdigit(static_cast<unsigned char>(name[digitStart - 1])) != 0) --digitStart;
  const int digitCount = static_cast<int>(name.size() - digitStart);
  if (digitCount > 0 && digitCount + 2 <= room)
    return cecon::ident::truncateCodePoints(name, static_cast<size_t>(room - 1 - digitCount)) + "~" +
           name.substr(digitStart);
  size_t lastChar = name.size() - 1;
  while (lastChar > 0 && (static_cast<unsigned char>(name[lastChar]) & 0xC0) == 0x80) --lastChar;
  return cecon::ident::truncateCodePoints(name, static_cast<size_t>(room - 2)) + "~" + name.substr(lastChar);
}

// A listing row: the term from column 1, the name range from column 19, the
// coding note from column 39. An overlong middle field just pushes the note along.
std::string xiListingRow(const std::string& term, const std::string& names, const std::string& note) {
  auto padded = [](std::string s, int cols) {
    const int have = xiChars(s);
    if (have < cols) s.append(static_cast<size_t>(cols - have), ' ');
    return s;
  };
  return padded(term, 18) + padded(names, 20) + note + "\n";
}

// How the listing writes an interaction term. The width formulas reproduce
// StataBE's output over a grid of name lengths (oracle cr_xi_c, section C2).
std::string xiFactorCrossText(const std::string& leftTag, const std::string& left, const std::string& rightTag,
                              const std::string& right) {
  const std::string shownLeft = xiShorten(left, std::min(xiChars(left), std::max(5, 8 - xiChars(right))));
  const std::string shownRight = xiShorten(right, std::min(xiChars(right), 10 - xiChars(shownLeft)));
  return leftTag + shownLeft + "*" + rightTag + shownRight;
}
std::string xiContinuousText(const std::string& leftTag, const std::string& left, char joint,
                             const std::string& right) {
  if (xiChars(left) + xiChars(right) <= 12) return leftTag + left + joint + right;
  const std::string shownLeft = xiShorten(left, std::min(xiChars(left), std::max(6, 12 - xiChars(right))));
  const std::string shownRight = xiShorten(right, std::min(xiChars(right), 12 - xiChars(shownLeft)));
  return leftTag + shownLeft + joint + shownRight;
}

// One distinct non-missing value of a factor.
struct XiLevel {
  double number = 0.0;
  std::string text;
  long long code = 0;
  size_t rows = 0;
  std::string shown;  // the value as a label prints it
};

// A factor as this run sees it. `rowLevel` indexes `levels`, -1 = missing.
struct XiFactor {
  std::string var;
  bool textual = false;
  bool natural = false;
  std::vector<XiLevel> levels;
  std::vector<int> rowLevel;
  int omitted = -1;
  std::vector<int> retained;
  std::string root;  // `_Ia_`, or a collision letter form such as `_Iaa`
  std::string wildcard() const { return root + "*"; }
  bool hasIndicators() const { return !retained.empty(); }
};

// `i.a*i.b` and friends, taken apart without looking at the data.
struct XiTermShape {
  std::string leftTag;
  std::string left;
  char joint = '\0';  // '\0', '*' or '|'
  bool rightIsFactor = false;
  std::string rightTag;
  std::string right;
};

std::optional<ojson> xiReadShape(const std::string& token, XiTermShape* out) {
  const std::string body = token.substr(2);
  out->leftTag = token.substr(0, 2);
  const size_t jointAt = body.find_first_of("*|");
  out->left = body.substr(0, jointAt);
  if (jointAt != std::string::npos) {
    out->joint = body[jointAt];
    std::string after = body.substr(jointAt + 1);
    if (xiHasFactorTag(after)) {
      if (out->joint == '|') return aerErrorReason("I.xxx|I.yyy not allowed", 198);
      out->rightIsFactor = true;
      out->rightTag = after.substr(0, 2);
      after.erase(0, 2);
    }
    out->right = after;
    if (after.empty()) return aerErrorReason("varlist required", 100);
  }
  if (out->left.empty()) return aerErrorReason("varlist required", 100);
  return std::nullopt;
}

// One run of xi over one line. Columns reach the dataset as soon as they are
// computed: a later term's refusal leaves the earlier terms' columns standing, as
// in Stata, and the collision test simply looks at the current dataset.
class XiExpansion {
 public:
  XiExpansion(SessionState& session, std::string stub, bool keepEveryLevel)
      : session_(session), stub_(std::move(stub)), keepEveryLevel_(keepEveryLevel) {}

  std::optional<ojson> run(const std::vector<std::string>& tokens) {
    if (auto bad = retirePreviousRun()) return bad;
    std::optional<ojson> refusal;
    for (const std::string& token : tokens) {
      refusal = expandTerm(token);
      if (refusal) break;
    }
    rememberThisRun();
    return refusal;
  }

  const std::string& listing() const { return listing_; }
  const std::vector<std::string>& made() const { return made_; }
  const std::vector<std::string>& substitutions() const { return substitutions_; }
  bool changedData() const { return !made_.empty() || !retired_.empty(); }
  // Columns the run removed and did not make again.
  std::vector<std::string> gone() const {
    std::vector<std::string> names;
    for (const auto& n : retired_)
      if (std::find(made_.begin(), made_.end(), n) == made_.end()) names.push_back(n);
    return names;
  }

 private:
  std::string characteristic(const std::string& owner, const std::string& key) const {
    for (const auto& entry : session_.characteristics)
      if (entry.varname == owner && entry.charname == key) return entry.value;
    return std::string();
  }

  void setDatasetCharacteristic(const std::string& key, const std::string& value) {
    auto& table = session_.characteristics;
    table.erase(std::remove_if(table.begin(), table.end(),
                               [&](const DtaCharacteristic& e) { return e.varname == "_dta" && e.charname == key; }),
                table.end());
    if (!value.empty()) table.push_back(DtaCharacteristic{"_dta", key, value});
  }

  std::optional<ojson> retirePreviousRun() {
    const std::vector<std::string> names = xiWords(characteristic("_dta", kXiMadeChar));
    const std::vector<std::string> stubs = xiWords(characteristic("_dta", kXiStubChar));
    Datastore* ds = session_.datastore();
    for (size_t i = 0; i < names.size() && i < stubs.size(); ++i) {
      if (stubs[i] != stub_) {
        foreignNames_.push_back(names[i]);
        foreignStubs_.push_back(stubs[i]);
      } else if (ds != nullptr && ds->has(names[i])) {
        retired_.push_back(names[i]);
      }
    }
    if (retired_.empty()) return std::nullopt;
    const std::string target = mintScratchPath(session_.workdir(), "xidrop");
    std::vector<std::string> args = {"eval", "--data", *session_.currentHbin(), "--out", target};
    for (const auto& name : retired_) {
      args.push_back("--drop");
      args.push_back(name);
    }
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      retired_.clear();
      return aerError(cobraErrorMessage(ce), ce.code);
    }
    session_.load(target);
    for (const auto& name : retired_) session_.varMeta.erase(name);
    return std::nullopt;
  }

  void rememberThisRun() {
    std::string names, stubs;
    auto add = [&](const std::string& name, const std::string& stub) {
      names += (names.empty() ? "" : " ") + name;
      stubs += (stubs.empty() ? "" : " ") + stub;
    };
    for (size_t i = 0; i < foreignNames_.size(); ++i) add(foreignNames_[i], foreignStubs_[i]);
    for (const auto& name : made_) add(name, stub_);
    setDatasetCharacteristic(kXiMadeChar, names);
    setDatasetCharacteristic(kXiStubChar, stubs);
  }

  bool rootInUse(const std::string& root) const {
    Datastore* ds = session_.datastore();
    if (ds == nullptr) return false;
    for (const auto& name : ds->names())
      if (name.compare(0, root.size(), root) == 0) return true;
    return false;
  }

  // A root some variable already begins gives way to its letter forms.
  std::string claimRoot(std::string root) const {
    if (!rootInUse(root)) return root;
    static const std::string kLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (char letter : kLetters) {
      root.back() = letter;
      if (!rootInUse(root)) break;
    }
    return root;
  }

  std::optional<ojson> bindVariable(const std::string& typed, std::string* full) {
    bool ambiguous = false;
    std::optional<std::string> bound = resolveVarName(typed, session_, &ambiguous);
    if (ambiguous) return aerErrorReason(typed + " ambiguous abbreviation", 111);
    if (!bound) return aerErrorReason("variable " + typed + " not found", 111);
    *full = *bound;
    return std::nullopt;
  }

  bool writeColumn(const std::string& name, const std::string& label, const std::vector<double>& values,
                   ojson* failure) {
    if (!estimWriteColumn(session_, name, values, failure)) return false;
    session_.setVarLabel(name, label);
    made_.push_back(name);
    return true;
  }

  // The distinct values with their counts, ascending, whichever way they are keyed,
  // and each row's position among them.
  template <typename Key>
  static std::map<Key, int> tallyLevels(const std::map<Key, size_t>& counts, std::vector<XiLevel>* levels) {
    std::map<Key, int> position;
    for (const auto& [key, count] : counts) {
      position[key] = static_cast<int>(levels->size());
      XiLevel level;
      level.rows = count;
      levels->push_back(std::move(level));
    }
    return position;
  }

  std::optional<ojson> readLevels(XiFactor* factor) {
    Datastore* ds = session_.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    const cecon::Column& column = ds->col(factor->var);
    const size_t rows = static_cast<size_t>(ds->nRows());
    factor->rowLevel.assign(rows, -1);
    if (column.dtype == cecon::DType::Factor) {
      factor->textual = true;
      const int32_t* cells = column.codes();
      auto textAt = [&](size_t r) -> const std::string* {
        if (cells == nullptr || cells[r] < 0) return nullptr;
        const std::string& s = column.levels[static_cast<size_t>(cells[r])];
        return s.empty() ? nullptr : &s;
      };
      std::map<std::string, size_t> counts;
      for (size_t r = 0; r < rows; ++r)
        if (const std::string* s = textAt(r)) ++counts[*s];
      std::map<std::string, int> position = tallyLevels(counts, &factor->levels);
      for (const auto& [value, index] : position) {
        factor->levels[static_cast<size_t>(index)].text = value;
        factor->levels[static_cast<size_t>(index)].shown = value;
      }
      for (size_t r = 0; r < rows; ++r)
        if (const std::string* s = textAt(r)) factor->rowLevel[r] = position[*s];
    } else {
      std::vector<double> cells;
      std::string why;
      if (!numericColumn(*ds, factor->var, &cells, &why)) return aerError(why);
      std::map<double, size_t> counts;
      for (double v : cells)
        if (!xiCellMissing(v)) ++counts[v];
      std::map<double, int> position = tallyLevels(counts, &factor->levels);
      for (const auto& [value, index] : position) {
        factor->levels[static_cast<size_t>(index)].number = value;
        factor->levels[static_cast<size_t>(index)].shown = strutil::trim(eval::applyStataFmt(value, "%18.0g"));
      }
      for (size_t r = 0; r < rows; ++r)
        if (!xiCellMissing(cells[r])) factor->rowLevel[r] = position[cells[r]];
    }
    factor->natural = !factor->textual;
    for (const XiLevel& level : factor->levels)
      if (level.number != std::floor(level.number) || level.number < 0.0 || level.number > 999999999.0)
        factor->natural = false;
    long long ordinal = 0;
    for (XiLevel& level : factor->levels)
      level.code = factor->natural ? static_cast<long long>(level.number) : ++ordinal;
    return std::nullopt;
  }

  // Which level the indicators leave out: noomit (none) beats char v[omit], which
  // beats any text in char _dta[omit] (the most frequent level, `prevalent` being only
  // the documented spelling), which beats the first level. A number in char v[omit]
  // is compared at float precision, so `0.1` finds a float column's 0.1.
  std::optional<ojson> chooseOmitted(XiFactor* factor) {
    if (keepEveryLevel_) return std::nullopt;
    const std::string wanted = strutil::trim(characteristic(factor->var, "omit"));
    if (!wanted.empty()) {
      float numericTarget = 0.0F;
      if (!factor->textual) {
        char* stop = nullptr;
        numericTarget = static_cast<float>(std::strtod(wanted.c_str(), &stop));
        if (stop == wanted.c_str() || *stop != '\0')
          return aerErrorReason(" characteristic " + factor->var + "[omit] (" + wanted + ") invalid;\nvariable " +
                                    factor->var + " is numeric",
                                198);
      }
      // Several double levels can share one float value; the highest of them is left out.
      for (size_t i = 0; i < factor->levels.size(); ++i) {
        const XiLevel& level = factor->levels[i];
        if (factor->textual ? level.text == wanted : static_cast<float>(level.number) == numericTarget)
          factor->omitted = static_cast<int>(i);
      }
      if (factor->omitted >= 0) return std::nullopt;
      listing_ += "(characteristic " + factor->var + "[omit]: " + wanted + "\nyet variable " +
                  xiShorten(factor->var, 22) + " never equals " + wanted + "; characteristic ignored)\n";
    }
    factor->omitted = 0;
    if (!characteristic("_dta", "omit").empty())
      for (size_t i = 1; i < factor->levels.size(); ++i)
        if (factor->levels[i].rows > factor->levels[static_cast<size_t>(factor->omitted)].rows)
          factor->omitted = static_cast<int>(i);
    return std::nullopt;
  }

  void listMainEffects(const XiFactor& factor, const std::string& tag) {
    if (keepEveryLevel_) return;
    const std::string range = factor.root + std::to_string(factor.levels.front().code) + "-" +
                              std::to_string(factor.levels.back().code);
    const XiLevel& out = factor.levels[static_cast<size_t>(factor.omitted)];
    const std::string outName = factor.root + std::to_string(out.code);
    // The variable's name gets what is left of a 41-byte note.
    const int nameRoom = 24 - static_cast<int>(outName.size() + out.shown.size());
    const std::string note = factor.natural ? "(naturally coded; " + outName + " omitted)"
                                            : "(" + outName + " for " + xiShorten(factor.var, nameRoom) + "==" +
                                                  out.shown + " omitted)";
    listing_ += xiListingRow(tag + xiShorten(factor.var, 13), range, note);
  }

  // The factor's main effects, made once per run by whichever term first needs them.
  std::optional<ojson> ensureFactor(const std::string& var, const std::string& tag, XiFactor** out) {
    const auto seen = factors_.find(var);
    if (seen != factors_.end()) {
      *out = &seen->second;
      return std::nullopt;
    }
    XiFactor factor;
    factor.var = var;
    if (auto bad = readLevels(&factor)) return bad;
    if (factor.levels.empty()) return aerSyntaxError("invalid syntax");  // measured: an all-missing factor
    if (auto bad = chooseOmitted(&factor)) return bad;
    const std::string plainRoot = stub_ + xiStem(var, 11 - stub_.size()) + "_";
    const std::string firstName = plainRoot + std::to_string(factor.levels.front().code);
    if (!strutil::isValidStataIdentifier(firstName)) return aerErrorReason(firstName + " invalid name", 198);
    factor.root = claimRoot(plainRoot);
    for (size_t i = 0; i < factor.levels.size(); ++i)
      if (static_cast<int>(i) != factor.omitted) factor.retained.push_back(static_cast<int>(i));
    listMainEffects(factor, tag);
    const size_t rows = factor.rowLevel.size();
    for (int level : factor.retained) {
      std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
      for (size_t r = 0; r < rows; ++r)
        if (factor.rowLevel[r] >= 0) values[r] = factor.rowLevel[r] == level ? 1.0 : 0.0;
      const XiLevel& info = factor.levels[static_cast<size_t>(level)];
      ojson failure;
      if (!writeColumn(factor.root + std::to_string(info.code), var + "==" + info.shown, values, &failure))
        return failure;
    }
    *out = &factors_.emplace(var, std::move(factor)).first->second;
    return std::nullopt;
  }

  std::optional<ojson> expandFactorCross(const XiTermShape& shape, const std::string& leftVar) {
    XiFactor* left = nullptr;
    if (auto bad = ensureFactor(leftVar, shape.leftTag, &left)) return bad;
    if (shape.right.find('*') != std::string::npos)
      return aerErrorReason("factor-variable and time-series operators not allowed", 101);
    if (shape.right.find('|') != std::string::npos) return aerErrorReason("| invalid name", 198);
    std::string rightVar;
    if (auto bad = bindVariable(shape.right, &rightVar)) return bad;
    XiFactor* right = nullptr;
    if (auto bad = ensureFactor(rightVar, shape.rightTag, &right)) return bad;
    const std::string caption = xiFactorCrossText(shape.leftTag, leftVar, shape.rightTag, rightVar);
    if (!left->hasIndicators() || !right->hasIndicators()) {
      listing_ += xiListingRow(caption, "", "(requires no interaction terms)");
      substitutions_.push_back(left->hasIndicators() ? left->wildcard() : leftVar);
      return std::nullopt;
    }
    const std::string root = claimRoot(stub_ + xiStem(leftVar, 3) + "X" + xiStem(rightVar, 3) + "_");
    listing_ += xiListingRow(caption, root + "#_#", "(coded as above)");
    const size_t rows = left->rowLevel.size();
    for (int a : left->retained) {
      const XiLevel& aInfo = left->levels[static_cast<size_t>(a)];
      for (int b : right->retained) {
        const XiLevel& bInfo = right->levels[static_cast<size_t>(b)];
        std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
        for (size_t r = 0; r < rows; ++r)
          if (left->rowLevel[r] >= 0 && right->rowLevel[r] >= 0)
            values[r] = (left->rowLevel[r] == a && right->rowLevel[r] == b) ? 1.0 : 0.0;
        ojson failure;
        if (!writeColumn(root + std::to_string(aInfo.code) + "_" + std::to_string(bInfo.code),
                         leftVar + "==" + aInfo.shown + " & " + rightVar + "==" + bInfo.shown, values, &failure))
          return failure;
      }
    }
    substitutions_.push_back(left->wildcard() + " " + right->wildcard() + " " + root + "*");
    return std::nullopt;
  }

  std::optional<ojson> expandContinuousCross(const XiTermShape& shape, const std::string& leftVar) {
    std::string rightVar;
    if (auto bad = bindVariable(shape.right, &rightVar)) return bad;
    if (!smoothIsNumeric(session_, rightVar)) return aerErrorReason("string variables not allowed", 109);
    std::vector<double> multiplier;
    {
      Datastore* ds = session_.datastore();
      std::string why;
      if (ds == nullptr) return ojson(eval::noDataLoaded());
      if (!numericColumn(*ds, rightVar, &multiplier, &why)) return aerError(why);
    }
    XiFactor* left = nullptr;
    if (auto bad = ensureFactor(leftVar, shape.leftTag, &left)) return bad;
    const std::string caption = xiContinuousText(shape.leftTag, leftVar, shape.joint, rightVar);
    // The command names c itself only the first time this run enters it.
    const std::string entry = enteredContinuous_.insert(rightVar).second ? rightVar + " " : std::string();
    if (!left->hasIndicators()) {
      listing_ += xiListingRow(caption, "", "(requires no interaction terms)");
      substitutions_.push_back(entry);
      return std::nullopt;
    }
    const size_t codeDigits = std::to_string(left->levels[static_cast<size_t>(left->retained.back())].code).size();
    const size_t rightBytes = codeDigits >= 3 ? 3 : 6 - codeDigits;
    const std::string root = claimRoot(stub_ + xiStem(leftVar, 3) + "X" + xiStem(rightVar, rightBytes) + "_");
    listing_ += xiListingRow(caption, root + "#", "(coded as above)");
    const size_t rows = left->rowLevel.size();
    for (int a : left->retained) {
      const XiLevel& aInfo = left->levels[static_cast<size_t>(a)];
      std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
      for (size_t r = 0; r < rows; ++r)
        if (left->rowLevel[r] >= 0 && !xiCellMissing(multiplier[r]))
          values[r] = left->rowLevel[r] == a ? multiplier[r] : 0.0;
      ojson failure;
      if (!writeColumn(root + std::to_string(aInfo.code), "(" + leftVar + "==" + aInfo.shown + ")*" + rightVar, values,
                       &failure))
        return failure;
    }
    // `i.a|c` leaves the main effects out of the command; `i.a*c` keeps them.
    substitutions_.push_back((shape.joint == '*' ? left->wildcard() + " " : std::string()) + entry + root + "*");
    return std::nullopt;
  }

  std::optional<ojson> expandTerm(const std::string& token) {
    XiTermShape shape;
    if (auto bad = xiReadShape(token, &shape)) return bad;
    std::string leftVar;
    if (auto bad = bindVariable(shape.left, &leftVar)) return bad;
    if (shape.joint == '\0') {
      XiFactor* factor = nullptr;
      if (auto bad = ensureFactor(leftVar, shape.leftTag, &factor)) return bad;
      substitutions_.push_back(factor->hasIndicators() ? factor->wildcard() : std::string());
      return std::nullopt;
    }
    return shape.rightIsFactor ? expandFactorCross(shape, leftVar) : expandContinuousCross(shape, leftVar);
  }

  SessionState& session_;
  std::string stub_;
  bool keepEveryLevel_;
  std::string listing_;
  std::vector<std::string> made_;
  std::vector<std::string> retired_;
  std::vector<std::string> foreignNames_;
  std::vector<std::string> foreignStubs_;
  std::vector<std::string> substitutions_;
  std::map<std::string, XiFactor> factors_;
  std::set<std::string> enteredContinuous_;
};

struct XiCommandLine {
  std::vector<std::string> tokens;
  std::string stub;
  bool keepEveryLevel = false;
};

// `prefix(x)`, spelled with at least `pre`.
bool xiTakePrefixOption(const std::string& word, std::string* stub) {
  const size_t paren = word.find('(');
  if (paren == std::string::npos || paren < 3 || word.back() != ')') return false;
  if (std::string("prefix").compare(0, paren, word, 0, paren) != 0) return false;
  *stub = strutil::trim(word.substr(paren + 1, word.size() - paren - 2));
  return true;
}

std::string xiOptionWordName(const std::string& word) {
  size_t end = 0;
  while (end < word.size() && (std::isalnum(static_cast<unsigned char>(word[end])) != 0 || word[end] == '_')) ++end;
  return end > 0 ? word.substr(0, end) : word;
}

// The line is read raw: xi's two arrangements, identified from its ado, are not ones
// the tokenizer knows.
//   `xi terms [, options]`: the part after the comma is options and nothing else.
//   `xi, options terms`: one or two leading option words, then an ordinary term
//   line. Words without a factor tag on a term line are passed over, so
//   `xi i.a b` and `xi, prefix(_I) b i.a` expand only `i.a`.
std::optional<ojson> xiReadCommandLine(const std::string& raw, XiCommandLine* out) {
  size_t cursor = raw.find_first_not_of(" \t");
  while (cursor != std::string::npos && cursor < raw.size() && std::isalpha(static_cast<unsigned char>(raw[cursor])) != 0)
    ++cursor;
  const std::string line = cursor == std::string::npos ? std::string() : raw.substr(cursor);
  const size_t comma = line.find(',');
  auto collectTerms = [out](const std::vector<std::string>& words, size_t from) {
    for (size_t i = from; i < words.size(); ++i)
      if (xiHasFactorTag(words[i])) out->tokens.push_back(words[i]);
  };
  auto refused = [](const std::string& word) { return aerErrorReason("option " + xiOptionWordName(word) + " not allowed", 198); };
  if (comma == std::string::npos) {
    collectTerms(xiWords(line), 0);
    return std::nullopt;
  }
  const std::vector<std::string> tail = xiWords(line.substr(comma + 1));
  if (line.find_first_not_of(" \t") != comma) {
    collectTerms(xiWords(line.substr(0, comma)), 0);
    for (const std::string& word : tail) {
      if (word == "noomit") out->keepEveryLevel = true;
      else if (!xiTakePrefixOption(word, &out->stub)) return refused(word);
    }
    return std::nullopt;
  }
  size_t used = 0;
  if (!tail.empty()) {
    if (tail[0] == "noomit") {
      out->keepEveryLevel = true;
      used = 1;
      if (tail.size() > 1 && xiTakePrefixOption(tail[1], &out->stub)) used = 2;
    } else {
      if (!xiTakePrefixOption(tail[0], &out->stub)) return refused(tail[0]);
      used = 1;
      if (tail.size() > 1 && tail[1] == "noomit") {
        out->keepEveryLevel = true;
        used = 2;
      }
    }
  }
  collectTerms(tail, used);
  return std::nullopt;
}

std::optional<ojson> xiStubRefusal(const std::string& stub) {
  if (stub.size() > 4) return aerSyntaxError("prefix() invalid; stub name too long ( >4 )");
  return std::nullopt;
}

// Each term token becomes its text in place; every other byte of the line stays,
// including runs of blanks and a comma glued to the term.
std::string xiRewriteLine(const std::string& line, const std::vector<std::string>& tokens,
                          const std::vector<std::string>& texts) {
  static const char kBlanks[] = " \t";
  std::string result;
  size_t cursor = 0;
  size_t next = 0;
  while (next < tokens.size() && next < texts.size()) {
    const size_t start = line.find_first_not_of(kBlanks, cursor);
    if (start == std::string::npos) break;
    size_t stop = line.find_first_of(kBlanks, start);
    if (stop == std::string::npos) stop = line.size();
    const std::string word = line.substr(start, stop - start);
    const size_t comma = word.find(',');
    result.append(line, cursor, start - cursor);
    if (word.compare(0, comma, tokens[next]) == 0) {
      result += texts[next++];
      if (comma != std::string::npos) result += word.substr(comma);
    } else {
      result += word;
    }
    cursor = stop;
  }
  result.append(line, cursor, std::string::npos);
  return result;
}

}  // namespace

ojson runXi(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.ifExpr.has_value()) return aerErrorReason("if not allowed", 101);
  if (parsed.inRange.has_value()) return aerErrorReason("in range not allowed", 101);
  if (parsed.weightType.has_value()) return aerErrorReason("weights not allowed", 101);

  XiCommandLine line;
  if (auto bad = xiReadCommandLine(parsed.raw, &line)) return *bad;
  if (auto bad = xiStubRefusal(line.stub)) return *bad;
  if (!session.hasData() || !session.currentHbin().has_value()) {
    if (line.tokens.empty()) return ojson::array();
    return ojson(eval::noDataLoaded());
  }

  // A line with no term still retires the previous run's columns (measured: `xi i.a`
  // then `xi b` leaves no `_Ia_*`).
  XiExpansion expansion(session, line.stub.empty() ? "_I" : line.stub, line.keepEveryLevel);
  const std::optional<ojson> refusal = expansion.run(line.tokens);
  ojson blocks = ojson::array();
  if (!expansion.listing().empty()) blocks.push_back(displayBlock(expansion.listing()));
  if (expansion.changedData()) blocks.push_back(mutationResult(session, "xi", expansion.made(), expansion.gone()));
  if (refusal) blocks.push_back(*refusal);
  return blocks;
}

ojson runXiPrefix(const std::vector<std::string>& terms,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  const std::string& wrapped, SessionState& session, std::string* rewritten,
                  std::string* displayText) {
  std::string stub;
  bool keepEveryLevel = false;
  for (const auto& [key, value] : opts) {
    if (key == "prefix") stub = value.strValue;
    if (key == "noomit") keepEveryLevel = true;
  }
  if (auto bad = xiStubRefusal(stub)) return *bad;
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  XiExpansion expansion(session, stub.empty() ? "_I" : stub, keepEveryLevel);
  if (auto refusal = expansion.run(terms)) return *refusal;
  *displayText += expansion.listing();
  *rewritten = xiRewriteLine(wrapped, terms, expansion.substitutions());
  return ojson();
}

}  // namespace piet::handlers
