// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Hand scans that stand in for a library call on the per-command path, each held
// to the call it replaces, which is the judge here:
//
// - testRegexScansMatchRegex: every function of piet/regex_scans.h against the
//   std::regex it stands for, on generated text built from the fragments those
//   patterns care about (word and space bytes of every class, newlines, quotes,
//   parentheses, the command words, high bytes, NUL). A function gives the same
//   yes/no and the same groups; a `may...` prefilter is never false where the
//   regex matches.
// - testDumpedJsonSizeMatchesDump: doexec::dumpedJsonSize against the length of
//   dump(-1, ' ', false, replace) -- the stored-transcript budget's proxy -- on
//   generated JSON (control bytes, split UTF-8, floats, extreme integers,
//   nesting).
// - testServeResponseFastRead: serve_response::readFast (engine/serve_response.h)
//   against the general reading of a `cobra serve` response, on generated
//   responses of the engine's shapes and of many it never sends (wrong types,
//   unknown and repeated keys, escapes, hidden columns, cut or padded lines).
//   Where the fast reading accepts, the general one must not throw and must
//   write the same; where it declines, it must have written nothing.
//
// The generator is seeded, so a failure names a case that reproduces.
#include "selftest_cases.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "engine/serve_response.h"
#include "piet/doexec.h"
#include "piet/regex_scans.h"
#include "selftest_support.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

const std::vector<std::string>& fragments() {
  static const std::vector<std::string> f = {
      "max",   "mean",    "min",       "pc",      "pctile",  "total",   "sum",      "count",
      "rank",  "median",  "x",         "y1",      "_z",      "Ab",      "if",       "IF",
      "If",    "iF",      "iff",       "in",      "xi",      "XI",      "ritest",   "RItest",
      "bootstrap", "Bootstrap", "statsby", "jackknife", "permute", "ROLLING", "str", "str12",
      "strl",  "strL",    "byte",      "int",     "double",  "float",   "long",     " ",
      "  ",    "\t",      "\n",        "\r",      "\v",      "\f",      "(",        ")",
      "()",    "( )",     "=",         "= ",      "\"",      "\"\"",    "`",        "'",
      "[",     "]",       "[1]",       ",",       ":",       "!",       "1",        "2.5",
      "-",     "+",       "*",         ".",       "$",       "\xc3\xa9", "\x80",    "\xff",
      std::string(1, '\0')};
  return f;
}

std::string randomText(std::mt19937& g, size_t maxFrag) {
  std::uniform_int_distribution<size_t> count(0, maxFrag);
  std::uniform_int_distribution<size_t> pick(0, fragments().size() - 1);
  std::string s;
  const size_t k = count(g);
  for (size_t i = 0; i < k; ++i) s += fragments()[pick(g)];
  return s;
}

std::string shown(const std::string& s) {
  std::ostringstream o;
  for (unsigned char c : s) {
    if (c >= 0x20 && c < 0x7f && c != '\\') {
      o << c;
    } else {
      static const char* hex = "0123456789abcdef";
      o << "\\x" << hex[c >> 4] << hex[c & 15];
    }
  }
  return o.str();
}

// One report per function: the first disagreement, or a pass with the count.
struct Tally {
  std::string name;
  long cases = 0;
  long matched = 0;
  std::string firstBad;
  void add(bool agree, bool regexMatched, const std::string& input) {
    ++cases;
    if (regexMatched) ++matched;
    if (!agree && firstBad.empty()) firstBad = shown(input);
  }
  void report() const {
    check(firstBad.empty(), "hand scan " + name + " agrees with its regex on " +
                                std::to_string(cases) + " inputs (" + std::to_string(matched) +
                                " matching)" + (firstBad.empty() ? "" : "; differs on [" + firstBad + "]"));
    // A generator that never reaches the matching side would prove nothing.
    check(matched > 0, "hand scan " + name + ": the generated inputs include matches");
  }
};

// Text that sometimes starts with one of the prefix words, so the prefilters
// meet their pattern's matching side often.
std::string prefixedText(std::mt19937& g) {
  static const char* const kHeads[] = {"bootstrap", "jackknife", "permute", "rolling", "statsby",
                                       "ritest",    "xi",        "!"};
  std::uniform_int_distribution<int> coin(0, 1);
  std::uniform_int_distribution<size_t> head(0, sizeof kHeads / sizeof kHeads[0] - 1);
  std::string s;
  if (coin(g)) {
    s += coin(g) ? " " : "";
    std::string w = kHeads[head(g)];
    for (char& c : w)
      if (coin(g) && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    s += w;
  }
  s += randomText(g, 6);
  if (coin(g)) s += std::string(coin(g) ? " :" : ":") + randomText(g, 3);
  return s;
}

}  // namespace

void testRegexScansMatchRegex() {
  std::cout << "\n== hand scans vs the std::regex they stand for ==\n";
  std::mt19937 g(20261120u);
  const int kCases = 20000;

  {
    static const std::regex re(R"(^(\w+)\s*\((.*)\)\s*$)");
    Tally t{"matchFnCall"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 7);
      if (i % 3 == 0) s = "max(" + s + ")";
      std::smatch m;
      std::string fn, args;
      const bool r = std::regex_match(s, m, re);
      const bool h = piet::rx::matchFnCall(s, &fn, &args);
      t.add(r == h && (!r || (fn == m.str(1) && args == m.str(2))), r, s);
    }
    t.report();
  }
  {
    static const std::regex re(
        R"(^([^=]*=\s*)(count|iqr|kurt|mad|max|mdev|mean|median|min|pc|pctile|rank|sd|skew|std|sum|total)\s*\((.*)\)\s*$)");
    static const std::set<std::string> kFns = {"count", "iqr",  "kurt", "mad",    "max", "mdev",
                                               "mean",  "median", "min", "pc",   "pctile", "rank",
                                               "sd",    "skew", "std",  "sum",    "total"};
    Tally t{"matchAssignFnCall"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 8);
      if (i % 3 == 0) s = "t = " + s;
      if (i % 5 == 0) s += ")";
      if (i % 4 == 1) {  // the matching shape, with generated pieces in every slot
        static const char* const kTry[] = {"max", "mean", "pc", "pctile", "maxx", "Mean", "total"};
        s = randomText(g, 2) + "=" + randomText(g, 1) + kTry[static_cast<size_t>(i) % 7] +
            randomText(g, 1) + "(" + randomText(g, 4) + ")" + randomText(g, 1);
      }
      std::smatch m;
      std::string lhs, fn, args;
      const bool r = std::regex_match(s, m, re);
      const bool h = piet::rx::matchAssignFnCall(s, &lhs, &fn, &args) && kFns.count(fn) != 0;
      t.add(r == h && (!r || (lhs == m.str(1) && fn == m.str(2) && args == m.str(3))), r, s);
    }
    t.report();
  }
  {
    static const std::regex re(R"(^(\w+)(\s*\(.*)$)");
    Tally t{"matchFnHead"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 6);
      std::smatch m;
      std::string head, rest;
      const bool r = std::regex_match(s, m, re);
      const bool h = piet::rx::matchFnHead(s, &head, &rest);
      t.add(r == h && (!r || (head == m.str(1) && rest == m.str(2))), r, s);
    }
    t.report();
  }
  for (const bool lead : {false, true}) {
    static const std::regex reBare(R"(^(\w+)\s*\()");
    static const std::regex reLead(R"(^\s*(\w+)\s*\()");
    Tally t{lead ? "searchFnName(leading space)" : "searchFnName"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 6);
      std::smatch m;
      std::string fn;
      const bool r = std::regex_search(s, m, lead ? reLead : reBare);
      const bool h = piet::rx::searchFnName(s, lead, &fn);
      t.add(r == h && (!r || fn == m.str(1)), r, s);
    }
    t.report();
  }
  {
    static const std::regex re(R"(^(\w+)\s*\(\s*\)\s*$)");
    Tally t{"matchFnEmptyCall"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 5);
      if (i % 2 == 0) s = "mean" + s;
      std::smatch m;
      std::string fn;
      const bool r = std::regex_match(s, m, re);
      const bool h = piet::rx::matchFnEmptyCall(s, &fn);
      t.add(r == h && (!r || fn == m.str(1)), r, s);
    }
    t.report();
  }
  {
    static const std::regex re(R"(\s+if(?:\s+|(?=\()))", std::regex::icase);
    Tally t{"nextSplitIf"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 9);
      if (i % 2 == 0) {  // an `if` word between generated pieces, in the shapes that split
        static const char* const kIf[] = {" if ", " IF(", "\tif\n", "  iF  ", " if", " iff ", "if "};
        s = randomText(g, 3) + kIf[static_cast<size_t>(i / 2) % 7] + randomText(g, 3) +
            kIf[static_cast<size_t>(i / 3) % 7] + randomText(g, 2);
      }
      std::vector<std::pair<size_t, size_t>> want, got;
      for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
        want.emplace_back(static_cast<size_t>(it->position(0)), static_cast<size_t>(it->length(0)));
      size_t from = 0, pos = 0, len = 0;
      while (piet::rx::nextSplitIf(s, from, &pos, &len)) {
        got.emplace_back(pos, len);
        from = pos + len;
      }
      t.add(want == got, !want.empty(), s);
    }
    t.report();
  }
  {
    static const std::regex re(R"(^(byte|int|long|float|double|str\d+|strl)$)");
    Tally t{"matchLowerTypeWord"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 2);
      if (i % 4 == 0) s = "str" + s;
      t.add(std::regex_match(s, re) == piet::rx::matchLowerTypeWord(s), std::regex_match(s, re), s);
    }
    t.report();
  }
  {
    static const std::regex re(
        R"((^|[^\w.$`\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[([^\[\],]+)\])");
    Tally t{"nextVarSubscript"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 9);
      if (i % 2 == 0) s += "x[" + randomText(g, 2) + "]" + randomText(g, 3);
      std::vector<std::vector<size_t>> want, got;
      for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
        want.push_back({static_cast<size_t>(it->position(0)), static_cast<size_t>(it->position(2)),
                        static_cast<size_t>(it->length(2)), static_cast<size_t>(it->position(3)),
                        static_cast<size_t>(it->length(3)),
                        static_cast<size_t>(it->position(0) + it->length(0))});
      piet::rx::VarSubscriptMatch m;
      for (size_t from = 0; piet::rx::nextVarSubscript(s, from, &m); from = m.end)
        got.push_back({m.start, m.namePos, m.nameLen, m.idxPos, m.idxLen, m.end});
      t.add(want == got, !want.empty(), s);
    }
    t.report();
  }
  {
    static const std::regex re(R"(^\s*(-?\d+)\s*$)");
    Tally t{"matchSignedInt"};
    for (int i = 0; i < kCases; ++i) {
      std::string s = randomText(g, 3);
      if (i % 3 == 0) s = randomText(g, 1) + "-12" + randomText(g, 1);
      std::smatch m;
      std::string digits;
      const bool r = std::regex_match(s, m, re);
      const bool h = piet::rx::matchSignedInt(s, &digits);
      t.add(r == h && (!r || digits == m.str(1)), r, s);
    }
    t.report();
  }
  {
    Tally t{"splitWords"};
    for (int i = 0; i < kCases; ++i) {
      const std::string s = randomText(g, 8);
      std::vector<std::string> want;
      std::istringstream iss(s);
      std::string w;
      while (iss >> w) want.push_back(w);
      t.add(want == piet::rx::splitWords(s), !want.empty(), s);
    }
    t.report();
  }
  {
    // The prefilters: never "cannot match" where the pattern matches.
    struct Prefix {
      const char* name;
      std::regex re;
      std::vector<const char*> words;
    };
    const std::vector<Prefix> kPrefixes = {
        {"resampling prefix",
         std::regex(R"(^\s*(bootstrap|jackknife|permute|rolling|statsby)\b(.*)$)", std::regex::icase),
         {"bootstrap", "jackknife", "permute", "rolling", "statsby"}},
        {"ritest", std::regex(R"(^\s*ritest(\s.*)?$)", std::regex::icase), {"ritest"}},
        {"xi prefix", std::regex(R"(^\s*xi\s*(?:,\s*([^:]*?))?\s*:\s*(\S.*)$)", std::regex::icase), {"xi"}},
        {"shell bang", std::regex(R"(^\s*!\s*(\S.*)$)"), {"!"}},
    };
    for (const auto& p : kPrefixes) {
      Tally t{std::string("mayStartWithWordCI for the ") + p.name};
      for (int i = 0; i < kCases; ++i) {
        const std::string s = prefixedText(g);
        const bool r = std::regex_match(s, p.re);
        bool may = false;
        for (const char* w : p.words) may = may || piet::rx::mayStartWithWordCI(s, w);
        t.add(!r || may, r, s);
      }
      t.report();
    }
    {
      // runEgen's row-function test runs its regex only for a head that names
      // a row function (egen.cpp; here a sample of the list).
      static const std::regex re(R"(^(\w+)\s*\(([^)]+)\)\s*$)");
      static const std::set<std::string> kRow = {"rowmax", "rowmean", "rowtotal", "max"};
      auto low = [](std::string x) {
        for (char& c : x) c = piet::rx::foldAscii(c);
        return x;
      };
      Tally t{"searchFnName for the row functions"};
      for (int i = 0; i < kCases; ++i) {
        std::string s = randomText(g, 5);
        if (i % 2 == 0) s = (i % 4 == 0 ? "RowMax" : "max") + s;
        std::smatch m;
        const bool r = std::regex_match(s, m, re) && kRow.count(low(m.str(1))) != 0;
        std::string head;
        const bool may = piet::rx::searchFnName(s, false, &head) && kRow.count(low(head)) != 0;
        t.add(!r || may, r, s);
      }
      t.report();
    }
    {
      // needsConditionMask (row_evaluation.cpp) runs its empty-literal regex only
      // where two quotes stand in a row.
      static const std::regex re(R"re((^|[^"`])""([^"']|$))re");
      Tally t{"two-quote prefilter for the empty literal"};
      for (int i = 0; i < kCases; ++i) {
        const std::string s = randomText(g, 6);
        const bool r = std::regex_search(s, re);
        t.add(!r || s.find("\"\"") != std::string::npos, r, s);
      }
      t.report();
    }
    {
      static const std::regex re(R"(^[A-Za-z_][A-Za-z0-9_]*\s+[A-Za-z_][A-Za-z0-9_]*$)");
      Tally t{"hasSpace for two names"};
      for (int i = 0; i < kCases; ++i) {
        const std::string s = randomText(g, 4);
        const bool r = std::regex_match(s, re);
        t.add(!r || piet::rx::hasSpace(s), r, s);
      }
      t.report();
    }
  }
}

namespace {

ojson randomJson(std::mt19937& g, int depth) {
  std::uniform_int_distribution<int> kind(0, depth > 0 ? 9 : 7);
  switch (kind(g)) {
    case 0:
      return nullptr;
    case 1:
      return std::uniform_int_distribution<int>(0, 1)(g) == 1;
    case 2: {
      static const std::int64_t kInts[] = {0, 1, -1, 9, 10, -10, 99, 100, 123456789,
                                           std::numeric_limits<std::int64_t>::min(),
                                           std::numeric_limits<std::int64_t>::max()};
      return kInts[std::uniform_int_distribution<size_t>(0, 10)(g)];
    }
    case 3: {
      static const std::uint64_t kUints[] = {0u, 7u, 10u, 1000u,
                                             std::numeric_limits<std::uint64_t>::max()};
      return kUints[std::uniform_int_distribution<size_t>(0, 4)(g)];
    }
    case 4: {
      static const double kFloats[] = {0.0, -0.0, 0.1, 1.5, -2.25, 1e300, 3.0, 1.0 / 3.0,
                                       std::numeric_limits<double>::quiet_NaN()};
      return kFloats[std::uniform_int_distribution<size_t>(0, 8)(g)];
    }
    case 5:
    case 6:
    case 7: {
      std::string s = randomText(g, 6);
      if (std::uniform_int_distribution<int>(0, 3)(g) == 0) s += static_cast<char>(0x1f);
      if (std::uniform_int_distribution<int>(0, 3)(g) == 0) s += "\x7f\\\b";
      return s;
    }
    case 8: {
      ojson a = ojson::array();
      const int n = std::uniform_int_distribution<int>(0, 4)(g);
      for (int i = 0; i < n; ++i) a.push_back(randomJson(g, depth - 1));
      return a;
    }
    default: {
      ojson o = ojson::object();
      const int n = std::uniform_int_distribution<int>(0, 4)(g);
      for (int i = 0; i < n; ++i) o[randomText(g, 2)] = randomJson(g, depth - 1);
      return o;
    }
  }
}

}  // namespace

void testDumpedJsonSizeMatchesDump() {
  std::cout << "\n== doexec::dumpedJsonSize vs dump().size() ==\n";
  std::mt19937 g(1120u);
  long cases = 0;
  std::string firstBad;
  for (int i = 0; i < 20000; ++i) {
    const ojson j = randomJson(g, 3);
    ++cases;
    const size_t want = j.dump(-1, ' ', false, ojson::error_handler_t::replace).size();
    const size_t got = piet::doexec::dumpedJsonSize(j);
    if (want != got && firstBad.empty())
      firstBad = j.dump(-1, ' ', true, ojson::error_handler_t::replace) + " want " +
                 std::to_string(want) + " got " + std::to_string(got);
  }
  // The block shapes a do-file loop actually stores.
  ojson mut = {{"type", "data_mutation"}, {"action", "gen"}, {"n_rows_after", 3}};
  mut["new_vars"] = ojson::array({"fdr_temp1"});
  mut["varlist_after"] = ojson::array({{{"name", "pval"}, {"dtype", "f64"}, {"format", "%9.0g"},
                                        {"label", ""}, {"n_missing", 0}}});
  for (const ojson& j : {mut, ojson{{"type", "echo"}, {"text", ".  gen x = 1 if y != \"\""}}}) {
    ++cases;
    if (piet::doexec::dumpedJsonSize(j) != j.dump(-1, ' ', false, ojson::error_handler_t::replace).size() &&
        firstBad.empty())
      firstBad = j.dump();
  }
  check(firstBad.empty(), "dumpedJsonSize equals the dump length on " + std::to_string(cases) +
                              " values" + (firstBad.empty() ? "" : "; differs on " + firstBad));
}

namespace {

using piet::CobraEstMeta;
using piet::CobraFrameMeta;
using piet::CobraFrameVar;
using piet::CobraRunResult;

bool sameVars(const std::vector<CobraFrameVar>& a, const std::vector<CobraFrameVar>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].name != b[i].name || a[i].type != b[i].type || a[i].nMissing != b[i].nMissing ||
        a[i].nLevels != b[i].nLevels || a[i].format != b[i].format || a[i].label != b[i].label)
      return false;
  }
  return true;
}

bool sameMeta(const CobraFrameMeta& a, const CobraFrameMeta& b) {
  return a.valid == b.valid && a.rows == b.rows && a.cols == b.cols && a.changed == b.changed &&
         a.added == b.added && a.dropped == b.dropped && a.droppedNames == b.droppedNames &&
         a.structure == b.structure && a.card_stable == b.card_stable &&
         a.mergeCountsValid == b.mergeCountsValid && a.mergeMasterOnly == b.mergeMasterOnly &&
         a.mergeUsingOnly == b.mergeUsingOnly && a.mergeMatched == b.mergeMatched &&
         sameVars(a.varsDelta, b.varsDelta) && sameVars(a.vars, b.vars);
}

bool sameEst(const CobraEstMeta& a, const CobraEstMeta& b) {
  return a.valid == b.valid && a.handle == b.handle && a.k == b.k && a.nObs == b.nObs &&
         a.vcovType == b.vcovType && a.fits == b.fits;
}

bool sameResult(const CobraRunResult& a, const CobraRunResult& b) {
  return a.ok == b.ok && a.exit_code == b.exit_code && a.stdout_text == b.stdout_text &&
         a.stderr_text == b.stderr_text && a.error == b.error;
}

// A value of the type `want` most of the time, of another now and then.
ojson pick(std::mt19937& g, const char* want) {
  static const std::vector<std::string> kNames = {"pval", "x", "__piet_esample__", "__piet_qual3",
                                                 "rank", "y_1", "z"};
  std::uniform_int_distribution<int> roll(0, 19);
  const int r = roll(g);
  const std::string w = want;
  if (r == 0) return nullptr;
  if (r == 1) return 2.5;
  if (r == 2) return "3";
  if (r == 3) return true;
  if (r == 4) return ojson::array();
  if (r == 5 && w != "int") return 7;
  if (w == "int") {
    static const std::int64_t kInts[] = {0, 3, -1, 12, 4000000000LL,
                                         std::numeric_limits<std::int64_t>::max(),
                                         std::numeric_limits<std::int64_t>::min()};
    return kInts[std::uniform_int_distribution<size_t>(0, 6)(g)];
  }
  if (w == "bool") return std::uniform_int_distribution<int>(0, 1)(g) == 1;
  if (w == "name") return kNames[std::uniform_int_distribution<size_t>(0, kNames.size() - 1)(g)];
  if (w == "names") {
    ojson a = ojson::array();
    const int n = std::uniform_int_distribution<int>(0, 3)(g);
    for (int i = 0; i < n; ++i) a.push_back(pick(g, "name"));
    return a;
  }
  // "text": output a command could print
  static const char* const kText[] = {"", "{\"n_rows\":3}\n", "say \"hi\"\tthere\\", "a/b",
                                      "\x01" "ctl", "caf\xc3\xa9", "line1\r\nline2"};
  return std::string(kText[std::uniform_int_distribution<size_t>(0, 6)(g)]);
}

ojson randomVar(std::mt19937& g) {
  ojson v = ojson::object();
  std::uniform_int_distribution<int> coin(0, 3);
  if (coin(g)) v["name"] = pick(g, "name");
  if (coin(g)) v["type"] = coin(g) ? ojson("f64") : ojson("factor");
  if (coin(g)) v["n_missing"] = pick(g, "int");
  if (coin(g) == 0) v["n_levels"] = pick(g, "int");
  if (coin(g) == 0) v["format"] = "%9.0g";
  if (coin(g) == 0) v["label"] = pick(g, "text");
  if (coin(g) == 0 && coin(g) == 0) v["extra"] = 1;
  return v;
}

std::string randomResponse(std::mt19937& g) {
  std::uniform_int_distribution<int> coin(0, 3);
  ojson r = ojson::object();
  if (coin(g)) r["id"] = pick(g, "int");
  if (coin(g)) r["exit"] = coin(g) ? ojson(0) : pick(g, "int");
  if (coin(g)) r["stdout"] = pick(g, "text");
  if (coin(g)) r["stderr"] = pick(g, "text");
  if (coin(g)) {
    ojson f = ojson::object();
    if (coin(g)) f["rows"] = pick(g, "int");
    if (coin(g)) f["cols"] = pick(g, "int");
    for (const char* k : {"changed", "added", "dropped"})
      if (coin(g)) f[k] = pick(g, "names");
    if (coin(g)) f["structure"] = pick(g, "bool");
    if (coin(g)) f["card_stable"] = pick(g, "bool");
    if (coin(g) == 0) f["merge_counts"] = {{"master_only", pick(g, "int")}, {"matched", 2}};
    if (coin(g) == 0) f["path"] = "/w/x.hbin";
    for (const char* k : {"vars_delta", "vars"}) {
      if (coin(g) == 0) continue;
      ojson a = ojson::array();
      const int n = std::uniform_int_distribution<int>(0, 3)(g);
      for (int i = 0; i < n; ++i) a.push_back(randomVar(g));
      f[k] = a;
    }
    r["frame"] = coin(g) == 0 && coin(g) == 0 ? ojson(5) : f;
  }
  if (coin(g) == 0) {
    ojson e = ojson::object();
    if (coin(g)) e["handle"] = coin(g) ? ojson("est:ab:1") : ojson("");
    if (coin(g)) e["k"] = pick(g, "int");
    if (coin(g)) e["n_obs"] = pick(g, "int");
    if (coin(g)) e["vcov_type"] = "hc1";
    if (coin(g)) e["fits"] = pick(g, "int");
    r["est"] = e;
  }
  if (coin(g) == 0 && coin(g) == 0) r["serve"] = 1;
  std::string line = r.dump(coin(g) == 0 ? 1 : -1, ' ', false, ojson::error_handler_t::replace);
  switch (std::uniform_int_distribution<int>(0, 11)(g)) {
    case 0:  // a key twice
      if (line.size() > 2) line.insert(line.size() - 1, line.size() > 2 ? ",\"exit\":1" : "");
      break;
    case 1:  // cut
      line.resize(line.size() / 2);
      break;
    case 2:
      line += "  \n";
      break;
    case 3:
      line += "x";
      break;
    case 4:
      line = " \t" + line;
      break;
    default:
      break;
  }
  return line;
}

}  // namespace

void testServeResponseFastRead() {
  std::cout << "\n== serve_response::readFast vs the general reading ==\n";
  std::mt19937 g(31u);
  long cases = 0, accepted = 0;
  std::string firstBad;
  auto one = [&](const std::string& line, bool withMeta) {
    ++cases;
    CobraFrameMeta mg, mf;
    CobraEstMeta eg, ef;
    CobraRunResult rg, rf;
    // Sentinels: a declined fast reading must leave them as they are.
    mf.rows = -77;
    ef.k = -77;
    rf.stdout_text = "untouched";
    const CobraFrameMeta mf0 = mf;
    const CobraEstMeta ef0 = ef;
    const CobraRunResult rf0 = rf;
    bool threw = false;
    try {
      piet::serve_response::readGeneral(line, withMeta ? &mg : nullptr, &eg, &rg);
    } catch (const nlohmann::json::exception&) {
      threw = true;
    }
    const bool fast = piet::serve_response::readFast(line, withMeta ? &mf : nullptr, &ef, &rf);
    bool agree = true;
    if (fast) {
      ++accepted;
      agree = !threw && sameEst(ef, eg) && sameResult(rf, rg) && (!withMeta || sameMeta(mf, mg));
    } else {
      agree = sameMeta(mf, mf0) && sameEst(ef, ef0) && sameResult(rf, rf0);
    }
    if (!agree && firstBad.empty()) firstBad = shown(line) + (withMeta ? " (with meta)" : "");
  };
  // The engine's own shapes (from a recorded run): each must be accepted.
  const std::vector<std::string> kReal = {
      R"({"id":7,"exit":0,"stdout":"","stderr":"","frame":{"rows":3,"cols":1,"changed":[],"added":["pval"],"dropped":["pval1"],"structure":true,"card_stable":false,"vars_delta":[{"name":"pval","type":"f64","n_missing":0}]}})",
      R"({"id":8,"exit":0,"stdout":"{\"n_rows\":3,\"n_rows_initial\":3,\"n_cols\":1,\"summarize\":{\"pval\":{\"N\":3,\"mean\":0.08566666666666667}}}\n","stderr":"","frame":{"rows":3,"cols":1,"changed":[],"added":[],"dropped":[],"structure":false,"card_stable":true,"vars_delta":[]}})",
      R"({"id":1,"exit":0,"stdout":"","stderr":"","frame":{"rows":3,"cols":1,"vars":[{"name":"pval1","type":"f64","n_missing":0}]}})",
      R"({"id":2,"exit":198,"stdout":"","stderr":"error: bad\n"})"};
  long realAccepted = 0;
  for (const auto& line : kReal) {
    const long before = accepted;
    one(line, true);
    one(line, false);
    if (accepted == before + 2) ++realAccepted;
  }
  for (int i = 0; i < 30000; ++i) {
    const std::string line = randomResponse(g);
    one(line, i % 2 == 0);
  }
  check(firstBad.empty(), "readFast agrees with the general reading on " + std::to_string(cases) +
                              " responses (" + std::to_string(accepted) + " read fast)" +
                              (firstBad.empty() ? "" : "; differs on [" + firstBad + "]"));
  check(realAccepted == static_cast<long>(kReal.size()),
        "readFast reads every recorded engine response shape (" + std::to_string(realAccepted) +
            "/" + std::to_string(kReal.size()) + ")");
  check(accepted > 1000, "the generated responses include ones read fast (" +
                             std::to_string(accepted) + ")");
}
