// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "prefix_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/variables.h"
#include "piet/eval/tokenizer.h"  // scanOneToken (label save using "file")
namespace piet::dispatch_detail {
namespace {

// rename's group syntax, as Stata's rename command defines it ([D] rename group):
// `rename (oldpatterns) (newpatterns) [, addnumber[(#)] renumber[(#)] sort dryrun r test]`.
// The pieces below follow that definition's order of work: options, the two pattern
// lists, the old names each old pattern selects, the new name built for each, the
// checks on the whole mapping, then the renames (vs-stata bank: the rename cells of
// specs/D2/rename.jsonl and specs/_optabbr/rename.jsonl).
struct RenOpts {
  bool dryrun = false, lower = false, upper = false, proper = false, r = false, sort = false,
       test = false, renumber = false, addnumber = false;
  long long from = 1;
};

struct RenEl {
  enum Kind { Str, Star, Qm, NumV, NumF, Dot, Eq } kind = Str;
  std::string text;  // Str: the literal
  int width = 0;     // NumF: the number of #
  int sub = 0;       // new: explicit [n] subscript (1-based old wildcard), 0 = none
  int link = -1;     // new: the old wildcard (0-based) this element copies
  bool numbered = false;  // new #-type element that takes the sequence number
};

struct RenPat {
  bool isList = false;             // plain names / ranges
  std::vector<std::string> names;  // isList: the names as typed (`a`, `-`, `b`)
  std::vector<RenEl> els;          // a wildcard pattern
  std::string original;
};

ojson renErr(const std::string& msg, int rc = 198) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", msg, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

bool renIsNameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
         (static_cast<unsigned char>(c) >= 0x80);
}

// One argument of the lhs: a parenthesised group or a single blank-delimited word.
bool renGetArg(std::string& s, std::string* out) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  s = s.substr(i);
  out->clear();
  if (s.empty()) return true;
  if (s[0] == '(') {
    int depth = 0;
    for (size_t j = 0; j < s.size(); ++j) {
      if (s[j] == '(') ++depth;
      else if (s[j] == ')' && --depth == 0) {
        // `(#)` inside a pattern closes inside the group; the group's own `)` is the
        // one that brings the depth back to zero.
        *out = s.substr(1, j - 1);
        s = s.substr(j + 1);
        return true;
      }
    }
    return false;  // unbalanced
  }
  size_t j = 0;
  while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
  *out = s.substr(0, j);
  s = s.substr(j);
  return true;
}

// The options after the comma, read word by word as the command does: an abbreviation
// of dryrun, lower, proper, sort, upper; `r` and `test` whole; renumber (renum..) and
// addnumber (addnum..) with an optional (#). Any other word is passed over -- Stata's
// rename accepts and ignores it (bank b052888e087a900b `rename price p, zznotanoption`
// and fd1825171e5c5bd9 `, t` both rename) -- unless a renumber/addnumber came first,
// when it is "option ... not allowed".
std::optional<ojson> renParseOptions(const std::string& ops, RenOpts& o) {
  std::vector<std::string> toks;
  {
    std::string cur;
    int depth = 0;
    for (char c : ops) {
      if (c == '(' && depth++ == 0) {
        if (!cur.empty()) toks.push_back(cur);
        cur = "(";
        continue;
      }
      if (c == ')' && depth > 0 && --depth == 0) {
        toks.push_back(cur + ")");
        cur.clear();
        continue;
      }
      if (depth == 0 && (std::isspace(static_cast<unsigned char>(c)) || c == ',')) {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
        continue;
      }
      cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
  }
  auto abbr = [](const std::string& w, const std::string& full, size_t minLen) {
    return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
  };
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& w = toks[i];
    if (w == "debug") continue;
    if (abbr(w, "dryrun", 1)) { o.dryrun = true; continue; }
    if (abbr(w, "lower", 1)) { o.lower = true; continue; }
    if (abbr(w, "proper", 1)) { o.proper = true; continue; }
    if (w == "r") { o.r = true; continue; }
    if (abbr(w, "sort", 1)) { o.sort = true; continue; }
    if (w == "test") { o.test = true; continue; }
    if (abbr(w, "upper", 1)) { o.upper = true; continue; }
    if (o.renumber || o.addnumber) return renErr("option " + w + " not allowed");
    std::string op;
    if (abbr(w, "renumber", 5)) { o.renumber = true; op = "renumber"; }
    else if (abbr(w, "addnumber", 6)) { o.addnumber = true; op = "addnumber"; }
    if (i + 1 < toks.size() && toks[i + 1].size() >= 2 && toks[i + 1][0] == '(') {
      const std::string num = toks[++i];
      if (op.empty()) continue;  // an ignored word's parenthesised argument
      const std::string in = strutil::trim(num.substr(1, num.size() - 2));
      char* end = nullptr;
      const double v = std::strtod(in.c_str(), &end);
      if (in.empty() || end == nullptr || *end != '\0' || v < 0 || v > 999999 ||
          v != std::floor(v))
        return renErr("option " + op + " misspecified");
      o.from = static_cast<long long>(v);
    }
  }
  const int nCase = int(o.lower) + int(o.upper) + int(o.proper);
  if (nCase > 1) return renErr("options lower, upper and proper cannot be specified together");
  if (nCase && o.renumber) return renErr("options of case and renumber cannot be specified together");
  if (nCase && o.addnumber) return renErr("options of case and addnumber cannot be specified together");
  return std::nullopt;
}

// One pattern word of an old (`isNew` false) or new pattern.
std::optional<ojson> renParseWord(const std::string& w, bool isNew, RenPat& p) {
  p.original = w;
  for (size_t i = 0; i < w.size();) {
    const char c = w[i];
    RenEl el;
    if (c == '*') {
      if (!p.els.empty() && p.els.back().kind == RenEl::Star) return renErr("** not allowed");
      el.kind = RenEl::Star;
      ++i;
    } else if (c == '?') {
      el.kind = RenEl::Qm;
      ++i;
    } else if (c == '#') {
      if (!p.els.empty() && (p.els.back().kind == RenEl::NumV || p.els.back().kind == RenEl::NumF))
        return renErr("## not allowed");
      el.kind = RenEl::NumV;
      ++i;
    } else if (c == '(') {
      size_t j = i + 1;
      while (j < w.size() && w[j] == '#') ++j;
      if (j == i + 1 || j >= w.size() || w[j] != ')') return renErr("parentheses unbalanced");
      el.kind = RenEl::NumF;
      el.width = static_cast<int>(j - i - 1);
      if (el.width > 10) return renErr("too many # in (#...#)");
      i = j + 1;
    } else if (c == '.' || c == '=') {
      if (!isNew) return renErr(std::string(1, c) + " not allowed in oldname");
      el.kind = c == '.' ? RenEl::Dot : RenEl::Eq;
      ++i;
    } else if (c == '[') {
      if (!isNew) return renErr("subscripts not allowed in oldname");
      const size_t j = w.find(']', i);
      if (j == std::string::npos) return renErr("no right bracket");
      if (p.els.empty() || p.els.back().kind == RenEl::Str || p.els.back().kind == RenEl::Eq)
        return renErr("subscript invalid");
      const std::string in = w.substr(i + 1, j - i - 1);
      if (in.empty() || !std::all_of(in.begin(), in.end(), [](char d) { return std::isdigit(static_cast<unsigned char>(d)); }))
        return renErr("invalid inside brackets");
      p.els.back().sub = std::stoi(in);
      i = j + 1;
      continue;
    } else {
      size_t j = i;
      while (j < w.size() && std::string("*?#(.=[").find(w[j]) == std::string::npos) ++j;
      el.kind = RenEl::Str;
      el.text = w.substr(i, j - i);
      i = j;
      if (!p.els.empty() && p.els.back().kind == RenEl::Str) {
        p.els.back().text += el.text;
        continue;
      }
    }
    p.els.push_back(el);
  }
  return std::nullopt;
}

// A pattern list: runs of plain names (with `a-b` ranges) form one list element; each
// word holding a wildcard is its own element.
std::optional<ojson> renParseList(const std::string& arg, bool isNew, std::vector<RenPat>& out) {
  std::string spaced;
  for (char c : arg) {
    if (c == '-') spaced += " - ";
    else spaced += c;
  }
  std::istringstream in(spaced);
  RenPat cur;
  cur.isList = true;
  auto flush = [&]() {
    if (!cur.names.empty()) out.push_back(cur);
    cur = RenPat{};
    cur.isList = true;
  };
  for (std::string w; in >> w;) {
    const bool plain = w == "-" || strutil::isValidStataIdentifier(w);
    if (plain) {
      if (w == "-" && (cur.names.empty() || cur.names.back() == "-"))
        return renErr("- must be preceded by a variable name");
      cur.names.push_back(w);
      continue;
    }
    if (!cur.names.empty() && cur.names.back() == "-")
      return renErr("- must be followed by a variable name");
    flush();
    RenPat p;
    if (auto e = renParseWord(w, isNew, p)) return e;
    out.push_back(p);
  }
  if (!cur.names.empty() && cur.names.back() == "-")
    return renErr("- must be followed by a variable name");
  flush();
  return std::nullopt;
}

bool renIsWild(const RenEl& e) { return e.kind != RenEl::Str && e.kind != RenEl::Eq; }
bool renIsNum(const RenEl& e) { return e.kind == RenEl::NumV || e.kind == RenEl::NumF; }

std::string renPadNum(const std::string& digits, int width) {
  size_t k = 0;
  while (k + 1 < digits.size() && digits[k] == '0') ++k;
  std::string v = digits.substr(k);
  while (static_cast<int>(v.size()) < width) v = "0" + v;
  return v;
}

// Matching one variable name against an old pattern, the way rename defines it.
// The fixed-length elements (text, ?, (##)) are taken off both ends first; then a
// `#` takes the longest run of digits it can and gives back two at a time when
// the rest does not match; a `*` takes the longest piece it can (shorter only
// when the rest does not match), and when a `#` follows it, the digits at the end
// of the first `len` characters of the name are given back to that `#`.
// `pieces` holds the text each element matched (every element, in order).
struct RenMatch {
  const std::string* name = nullptr;
  size_t i0 = 0, i1 = 0;  // characters still to match: [i0, i1)
  size_t j0 = 0, j1 = 0;  // pattern elements still to match: [j0, j1)
  std::vector<std::string> pieces;
};
enum class RenMr { Success, Failure, Irrel };

void renPost(RenMatch& m, bool left, size_t len) {
  if (left) {
    m.pieces[m.j0] = m.name->substr(m.i0, len);
    m.i0 += len;
    ++m.j0;
  } else {
    --m.j1;
    m.pieces[m.j1] = m.name->substr(m.i1 - len, len);
    m.i1 -= len;
  }
}

bool renIsDigit(char c) { return c >= '0' && c <= '9'; }

bool renMatchStep(RenMatch& m, const std::vector<RenEl>& els);

// 1 done and matched, 0 done and not matched, -1 not done
int renDone(const RenMatch& m) {
  if (m.j0 >= m.j1) return m.i0 < m.i1 ? 0 : 1;
  return -1;
}

RenMr renMatchFixed(RenMatch& m, const std::vector<RenEl>& els, bool left) {
  const RenEl& e = els[left ? m.j0 : m.j1 - 1];
  const size_t avail = m.i1 - m.i0;
  if (e.kind == RenEl::Str) {
    const size_t len = e.text.size();
    if (avail < len) return RenMr::Failure;
    if (m.name->compare(left ? m.i0 : m.i1 - len, len, e.text) != 0) return RenMr::Failure;
    renPost(m, left, len);
    return RenMr::Success;
  }
  if (e.kind == RenEl::NumF) {
    const size_t len = static_cast<size_t>(e.width);
    if (avail < len || m.name->size() < len) return RenMr::Failure;
    // on the right the digits tested are the name's last `len` characters
    const size_t at = left ? m.i0 : m.name->size() - len;
    for (size_t k = 0; k < len; ++k)
      if (!renIsDigit((*m.name)[at + k])) return RenMr::Failure;
    renPost(m, left, len);
    return RenMr::Success;
  }
  if (e.kind == RenEl::Qm) {
    if (m.i0 == m.i1) return RenMr::Failure;
    renPost(m, left, 1);
    return RenMr::Success;
  }
  return RenMr::Irrel;
}

RenMr renMatchNumV(RenMatch& m, const std::vector<RenEl>& els, bool left) {
  if (els[left ? m.j0 : m.j1 - 1].kind != RenEl::NumV) return RenMr::Irrel;
  long long len = 0;
  if (left) {
    size_t i = m.i0;
    while (i < m.i1 && renIsDigit((*m.name)[i])) ++i;
    len = static_cast<long long>(i - m.i0);
  } else {
    size_t i = m.i1;
    while (i > m.i0 && renIsDigit((*m.name)[i - 1])) --i;
    len = static_cast<long long>(m.i1 - i);
  }
  const RenMatch hold = m;
  for (; len > 0; --len) {
    renPost(m, left, static_cast<size_t>(len));
    if (renMatchStep(m, els)) return RenMr::Success;
    m = hold;
    --len;  // the longest run, then two digits fewer at each try
  }
  return RenMr::Failure;
}

RenMr renMatchStarLeft(RenMatch& m, const std::vector<RenEl>& els) {
  if (els[m.j0].kind != RenEl::Star) return RenMr::Irrel;
  long long len = static_cast<long long>(m.i1 - m.i0);
  if (m.j1 - m.j0 == 1) {
    renPost(m, true, static_cast<size_t>(len));
    return RenMr::Success;
  }
  const bool nextIsNum = els[m.j0 + 1].kind == RenEl::NumV;
  const RenMatch hold = m;
  for (; len >= 0; --len) {
    renPost(m, true, static_cast<size_t>(len));
    if (renMatchStep(m, els)) {
      if (nextIsNum) {
        const std::string head = m.name->substr(0, static_cast<size_t>(len));
        long long keep = static_cast<long long>(head.size());
        while (keep > 0 && renIsDigit(head[static_cast<size_t>(keep - 1)])) --keep;
        if (keep < len) {
          m = hold;
          renPost(m, true, static_cast<size_t>(keep));
          if (!renMatchStep(m, els)) return RenMr::Failure;
        }
      }
      return RenMr::Success;
    }
    m = hold;
  }
  return RenMr::Failure;
}

bool renMatchStep(RenMatch& m, const std::vector<RenEl>& els) {
  while (true) {
    int r = renDone(m);
    if (r >= 0) return r == 1;
    while (m.i1 > m.i0 && m.j1 > m.j0) {
      const RenMr mr = renMatchFixed(m, els, true);
      if (mr == RenMr::Failure) return false;
      if (mr == RenMr::Irrel) break;
    }
    while (m.i1 > m.i0 && m.j1 > m.j0) {
      const RenMr mr = renMatchFixed(m, els, false);
      if (mr == RenMr::Failure) return false;
      if (mr == RenMr::Irrel) break;
    }
    r = renDone(m);
    if (r >= 0) return r == 1;
    const size_t left = m.j1 - m.j0;
    RenMr mr = RenMr::Failure;
    if (left == 1) {
      if (els[m.j0].kind == RenEl::NumV) mr = renMatchNumV(m, els, true);
      else if (els[m.j0].kind == RenEl::Star) mr = renMatchStarLeft(m, els);
      if (mr == RenMr::Failure) return false;
      return renDone(m) == 1;
    }
    if (left == 2) {
      if (els[m.j0].kind == RenEl::NumV) mr = renMatchNumV(m, els, true);
      else if (els[m.j0 + 1].kind == RenEl::NumV) mr = renMatchNumV(m, els, false);
    } else if (els[m.j0].kind == RenEl::Star) {
      if (els[m.j1 - 1].kind == RenEl::Star) mr = renMatchStarLeft(m, els);
      else if (els[m.j1 - 1].kind == RenEl::NumV) mr = renMatchNumV(m, els, false);
    } else if (els[m.j0].kind == RenEl::NumV) {
      mr = renMatchNumV(m, els, true);
    }
    if (mr != RenMr::Success) return false;
  }
}

// The text each wildcard element of `els` matched in `name`, or nullopt.
std::optional<std::vector<std::string>> renMatchName(const std::vector<RenEl>& els,
                                                     const std::string& name) {
  RenMatch m;
  m.name = &name;
  m.i1 = name.size();
  m.j1 = els.size();
  m.pieces.assign(els.size(), std::string());
  if (!renMatchStep(m, els)) return std::nullopt;
  std::vector<std::string> wild;
  for (size_t k = 0; k < els.size(); ++k)
    if (renIsWild(els[k])) wild.push_back(m.pieces[k]);
  return wild;
}

}  // namespace

// The group form. Returns the blocks to append (an error, notes) after doing the renames.
static ojson renameGroupForm(const std::string& lhsIn, const RenOpts& o, SessionState& session) {
  ojson out = ojson::array();
  std::string lhs = lhsIn, oldArg, newArg;
  if (!renGetArg(lhs, &oldArg) || !renGetArg(lhs, &newArg)) return renErr("parentheses unbalanced");
  if (!strutil::trim(lhs).empty() || strutil::trim(oldArg).empty() || strutil::trim(newArg).empty())
    return renErr("syntax error");
  if (o.dryrun)
    return ojson(eval::unsupportedOption(
        "dryrun", "rename's dryrun report is not implemented in this version of PIET."));
  std::vector<RenPat> oldp, newp;
  if (auto e = renParseList(oldArg, false, oldp)) return *e;
  if (auto e = renParseList(newArg, true, newp)) return *e;
  if (oldp.empty() || newp.empty()) return renErr("syntax error");
  while (newp.size() < oldp.size()) newp.push_back(newp.back());
  if (newp.size() != oldp.size()) return renErr("too many new patterns for the old ones");
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  std::vector<std::string> names;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) names.push_back(info.name);
  } else if (Datastore* rds = session.datastore()) {
    names = rds->names();
  }
  std::vector<std::pair<std::string, std::string>> toren;
  for (size_t pi = 0; pi < oldp.size(); ++pi) {
    RenPat& op = oldp[pi];
    RenPat& np = newp[pi];
    // the old names this pattern selects, each with the text of every old wildcard
    std::vector<std::pair<std::string, std::vector<std::string>>> sel;
    std::vector<RenEl> oldWild;
    if (op.isList) {
      std::vector<std::string> typed;
      for (size_t i = 0; i < op.names.size(); ++i) {
        if (op.names[i] == "-") continue;
        typed.push_back(op.names[i]);
      }
      if (auto err = handlers::resolveByKeyNames(session, typed)) return *err;
      size_t t = 0;
      for (size_t i = 0; i < op.names.size(); ++i) {
        if (op.names[i] == "-") continue;
        const std::string nm = typed[t++];
        if (i >= 2 && op.names[i - 1] == "-") {
          // a range: every variable from the one before `-` to this one, in data order
          const std::string a = sel.back().first;
          auto ia = std::find(names.begin(), names.end(), a);
          auto ib = std::find(names.begin(), names.end(), nm);
          if (ia == names.end() || ib == names.end() || ib < ia)
            return renErr("variable range " + a + "-" + nm + " invalid", 198);
          for (auto it = ia + 1; it <= ib; ++it) sel.push_back({*it, {}});
          continue;
        }
        sel.push_back({nm, {}});
      }
    } else {
      // The candidates are the variables the pattern names as a varlist (`*` any
      // text, `?` one character, `#` one or more, (##) that many); none is r(111).
      std::string re = "^";
      bool filter = false;
      for (const auto& e : op.els) {
        switch (e.kind) {
          case RenEl::Str:
            for (char c : e.text) {
              if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) re += '\\';
              re += c;
            }
            break;
          case RenEl::Star: re += ".*"; break;
          case RenEl::Qm: re += "."; break;
          case RenEl::NumV: re += "..*"; filter = true; break;
          case RenEl::NumF: re += ".{" + std::to_string(e.width) + "}"; filter = true; break;
          default: break;
        }
        if (renIsWild(e)) oldWild.push_back(e);
      }
      re += "$";
      const std::regex rx(re);
      std::vector<std::string> cand;
      for (const auto& nm : names)
        if (std::regex_match(nm, rx)) cand.push_back(nm);
      if (cand.empty()) return renErr("variable " + op.original + " not found", 111);
      // A pattern that starts with * or ? and holds no # leaves out the variables
      // whose names start with `_` (_merge, _est_*).
      const bool skipUnderscore =
          !filter && !op.els.empty() &&
          (op.els.front().kind == RenEl::Star || op.els.front().kind == RenEl::Qm);
      for (const auto& nm : cand) {
        if (skipUnderscore && !nm.empty() && nm[0] == '_') continue;
        std::optional<std::vector<std::string>> pieces = renMatchName(op.els, nm);
        if (!pieces) continue;  // only a # pattern can fail here: the name is left out
        sel.push_back({nm, *pieces});
      }
    }
    if (o.sort) {
      int numField = -1;
      for (size_t k = 0; k < oldWild.size(); ++k)
        if (renIsNum(oldWild[k])) { numField = static_cast<int>(k); break; }
      std::stable_sort(sel.begin(), sel.end(), [&](const auto& a, const auto& b) {
        if (numField >= 0) {
          const double x = std::stod(a.second[numField]), y = std::stod(b.second[numField]);
          if (x != y) return x < y;
        }
        return a.first < b.first;
      });
    }
    if (np.isList) {
      std::vector<std::string> nn;
      for (const auto& w : np.names)
        if (w != "-") nn.push_back(w);
      if (std::find(np.names.begin(), np.names.end(), "-") != np.names.end())
        return renErr("- not allowed in newname");
      if (o.renumber || o.addnumber)
        return renErr(std::string("missing # specifier in newname\n    You specified option ") +
                      (o.addnumber ? "addnumber" : "renumber") + ".");
      if (nn.size() != sel.size())
        return renErr(std::to_string(nn.size()) + " new variable name" + (nn.size() == 1 ? "" : "s") +
                      " but " + std::to_string(sel.size()) + " old variable name" +
                      (sel.size() == 1 ? "" : "s"));
      for (size_t i = 0; i < sel.size(); ++i) toren.push_back({sel[i].first, nn[i]});
      continue;
    }
    // link the new pattern's wildcards to the old pattern's
    int extra = -1;  // addnumber: the #-type element that is not linked
    if (o.addnumber) {
      for (size_t k = 0; k < np.els.size(); ++k)
        if (renIsNum(np.els[k])) extra = static_cast<int>(k);
      if (extra < 0) return renErr("missing # specifier in newname\n    You specified option addnumber.");
      np.els[extra].numbered = true;
    }
    if (o.renumber) {
      int nNum = 0;
      for (auto& e : np.els)
        if (renIsNum(e)) { e.numbered = true; ++nNum; }
      if (nNum == 0) return renErr("missing # specifier in newname\n    You specified option renumber.");
      if (nNum > 1) return renErr("too many # specifiers in newname with option renumber");
    }
    int next = 0;
    for (size_t k = 0; k < np.els.size(); ++k) {
      RenEl& e = np.els[k];
      if (!renIsWild(e) || static_cast<int>(k) == extra) continue;
      int j;
      if (e.sub > 0) j = e.sub - 1;
      else j = next++;
      if (j >= static_cast<int>(oldWild.size()))
        return renErr("too many wildcards in newname " + np.original + " for oldname " + op.original);
      const RenEl& oe = oldWild[j];
      if (e.kind == RenEl::Qm && oe.kind != RenEl::Qm)
        return renErr("? in newname incompatible with the wildcard in oldname");
      if (renIsNum(e) && !e.numbered && !renIsNum(oe))
        return renErr("# in newname incompatible with the wildcard in oldname");
      e.link = j;
    }
    long long seq = o.from;
    for (const auto& [nm, pieces] : sel) {
      std::string nn;
      for (const auto& e : np.els) {
        switch (e.kind) {
          case RenEl::Str: nn += e.text; break;
          case RenEl::Eq: nn += nm; break;
          case RenEl::Dot: break;
          case RenEl::Star:
          case RenEl::Qm: nn += pieces[e.link]; break;
          case RenEl::NumV: nn += e.numbered ? std::to_string(seq) : pieces[e.link]; break;
          case RenEl::NumF:
            nn += renPadNum(e.numbered ? std::to_string(seq) : pieces[e.link], e.width);
            break;
        }
      }
      ++seq;
      toren.push_back({nm, nn});
    }
  }
  // the mapping as a whole
  for (const auto& [from, to] : toren)
    if (!strutil::isValidStataIdentifier(to) || to.size() > 32)
      return renErr("1 new variable name invalid\n    You attempted to rename " + from + " to " + to +
                    ".  That is an invalid Stata variable name.");
  {
    std::map<std::string, int> co, cn;
    for (const auto& [from, to] : toren) { ++co[from]; ++cn[to]; }
    for (const auto& [n, c] : co)
      if (c > 1) return renErr("1 existing variable specified repeatedly in oldname");
    for (const auto& [n, c] : cn)
      if (c > 1) return renErr("1 new name specified repeatedly in newname");
  }
  if (toren.empty()) {
    // every candidate of a # pattern was left out: nothing to rename, rc 0
    ojson note;
    note["type"] = "display";
    note["class"] = "note";
    note["text"] = "  (nothing to rename)";
    out.push_back(note);
    if (o.test) {
      const std::string sp = "\x01s:";
      session.eval.rStrings.set(sp + "n", "0");
      session.eval.rStrings.set(sp + "intermediary", "0");
    }
    return out;
  }
  std::vector<std::pair<std::string, std::string>> moves;
  for (const auto& m : toren)
    if (m.first != m.second) moves.push_back(m);
  if (moves.empty()) {
    if (!toren.empty()) {
      ojson note;
      note["type"] = "display";
      note["class"] = "note";
      note["text"] = "  (all newnames==oldnames)";
      out.push_back(note);
    }
    return out;
  }
  for (const auto& [from, to] : moves) {
    const bool movedAway = std::any_of(moves.begin(), moves.end(), [&](const auto& m) { return m.first == to; });
    if (!movedAway && std::find(names.begin(), names.end(), to) != names.end())
      return renErr("1 variable already exists\n    You requested that variable " + from +
                    " be renamed " + to + ".  " + to + " is already an existing variable in the data.",
                    110);
  }
  std::set<std::string> all;
  for (const auto& [from, to] : moves) { all.insert(from); all.insert(to); }
  const bool intermediary = all.size() != 2 * moves.size();
  if (o.test) {
    const std::string sp = "\x01s:";
    session.eval.rStrings.set(sp + "n", std::to_string(moves.size()));
    session.eval.rStrings.set(sp + "intermediary", intermediary ? "1" : "0");
    for (size_t i = 0; i < moves.size(); ++i)
      session.eval.rStrings.set(sp + "r" + std::to_string(i + 1), moves[i].first + " " + moves[i].second);
    return out;
  }
  auto doRename = [&](const std::string& a, const std::string& b) -> std::optional<ojson> {
    ojson r = handlers::runRename(a, b, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
    return std::nullopt;
  };
  if (intermediary) {
    for (size_t i = 0; i < moves.size(); ++i)
      if (auto e = doRename(moves[i].first, "__gren_" + std::to_string(i))) return *e;
    for (size_t i = 0; i < moves.size(); ++i)
      if (auto e = doRename("__gren_" + std::to_string(i), moves[i].second)) return *e;
  } else {
    for (const auto& [from, to] : moves)
      if (auto e = doRename(from, to)) return *e;
  }
  if (o.r) {
    std::string olds, news;
    for (const auto& [from, to] : moves) {
      olds += (olds.empty() ? "" : " ") + from;
      news += (news.empty() ? "" : " ") + to;
    }
    session.eval.rValues.set("n", static_cast<double>(moves.size()));
    session.eval.rStrings.set("newnames", news);
    session.eval.rStrings.set("oldnames", olds);
  }
  return out;
}

bool route_data_names(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  if (cmd == "rename" || cmd == "ren") {
    if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
      // rename.ado reads no if/in: "syntax error", r(198) (vs-stata bank 8026de9d0f85092f)
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error",
          "syntax error\n    Syntax is\n        rename  oldname    newname   [, renumber[(#)] "
          "addnumber[(#)] sort ...]\n        rename (oldnames) (newnames) [, renumber[(#)] "
          "addnumber[(#)] sort ...]\n        rename  oldnames              , "
          "{upper|lower|proper}",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // Two plain, different names and nothing else is the classic one-to-one rename
    // (handled at the end of this arm); everything else -- a comma, parentheses,
    // wildcards, one name, three -- is the group syntax (renameGroupForm above), with
    // the case form (lower / upper / proper) next.
    {
      const std::string spec = rawArgsAfterCmd(parsed.raw);
      std::vector<std::string> ftoks;
      {
        std::istringstream in(spec);
        for (std::string w; in >> w;) ftoks.push_back(w);
      }
      const bool fast = ftoks.size() == 2 && strutil::isValidStataIdentifier(ftoks[0]) &&
                        strutil::isValidStataIdentifier(ftoks[1]) && ftoks[0] != ftoks[1];
      if (!fast) {
        std::string lhs = spec, ops;
        int depth = 0;
        bool inQ = false;
        for (size_t i = 0; i < spec.size(); ++i) {
          const char c = spec[i];
          if (c == '"') inQ = !inQ;
          if (inQ) continue;
          if (c == '(') ++depth;
          else if (c == ')') --depth;
          else if (c == ',' && depth == 0) {
            lhs = spec.substr(0, i);
            ops = spec.substr(i + 1);
            break;
          }
        }
        RenOpts ro;
        if (auto err = renParseOptions(ops, ro)) {
          blocks.push_back(*err);
          return true;
        }
        if (ro.r) eval::replaceRClass(session.eval);
        if (!(ro.lower || ro.upper || ro.proper)) {
          ojson res = renameGroupForm(lhs, ro, session);
          if (res.is_array()) {
            for (auto& b : res) blocks.push_back(b);
          } else {
            blocks.push_back(res);
          }
          if (ro.r && !(res.is_object() && res.value("type", std::string()) == "error"))
            eval::declareRPosted(session.eval, "rename", {"n", "newnames", "oldnames"});
          return true;
        }
      }
    }
    // ------------------------------------------------------------------
    // B4-W3b: the GROUP form `rename _all, lower` / `rename *, upper`.
    //
    // Routed BEFORE the wildcard reject below, because `*` is exactly the
    // spelling the corpus uses and the reject is about the PATTERN form
    // (`rename v* w*`), which this is not: there is no target pattern here at
    // all, only a case rule applied to every name.
    bool renLower = false, renUpper = false, renProper = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "lower" && (!v.isBool || v.boolValue)) renLower = true;
      if (k == "upper" && (!v.isBool || v.boolValue)) renUpper = true;
      if (k == "proper" && (!v.isBool || v.boolValue)) renProper = true;
    }
    if (renLower || renUpper || renProper) {
      if (static_cast<int>(renLower) + static_cast<int>(renUpper) + static_cast<int>(renProper) > 1) {
        blocks.push_back(ojson(eval::errorBlock(
            "bad_input", "rename: lower, upper and proper are alternatives — specify one.")));
        return true;
      }
      if (!session.hasData()) {
        blocks.push_back(ojson(eval::noDataLoaded()));
        return true;
      }
      std::vector<std::string> names;
      if (const auto* card = session.shadowVarlistIfCurrent()) {
        for (const auto& info : *card) names.push_back(info.name);
      } else if (Datastore* rds = session.datastore()) {
        names = rds->names();
      }
      // rename.ado's case form: `rename oldnames, lower|upper|proper`, the
      // old names a varlist -- names, abbreviations, wildcards, optionally in
      // parentheses (aer_w2 w2_data_io.log:832-848 ren1-ren7: `rename ab,
      // upper` renames abc; `rename *Y, proper` renames XY).
      std::vector<std::string> selected;
      {
        std::string spec = rawArgsAfterCmd(parsed.raw);
        bool inQ = false;
        for (size_t i = 0; i < spec.size(); ++i) {
          if (spec[i] == '"') inQ = !inQ;
          if (spec[i] == ',' && !inQ) { spec = spec.substr(0, i); break; }
        }
        for (char& c : spec)
          if (c == '(' || c == ')') c = ' ';
        std::istringstream iss(spec);
        std::string tok;
        auto glob = [](const std::string& pat, const std::string& str) {
          // * any run, ? one character, ~ one character too (Stata varlist).
          std::function<bool(size_t, size_t)> m = [&](size_t i, size_t j) -> bool {
            if (i == pat.size()) return j == str.size();
            if (pat[i] == '*') {
              for (size_t k = j; k <= str.size(); ++k)
                if (m(i + 1, k)) return true;
              return false;
            }
            if (j < str.size() && (pat[i] == '?' || pat[i] == '~' || pat[i] == str[j]))
              return m(i + 1, j + 1);
            return false;
          };
          return m(0, 0);
        };
        while (iss >> tok) {
          std::vector<std::string> hit;
          if (tok == "_all") {
            hit = names;
          } else if (tok.find_first_of("*?~") != std::string::npos) {
            for (const auto& nm : names)
              if (glob(tok, nm)) hit.push_back(nm);
          } else if (std::find(names.begin(), names.end(), tok) != names.end()) {
            hit.push_back(tok);
          } else {
            for (const auto& nm : names)
              if (nm.size() > tok.size() && nm.compare(0, tok.size(), tok) == 0) hit.push_back(nm);
            if (hit.size() > 1) {
              blocks.push_back(ojson(eval::errorBlock("engine_error", tok + " ambiguous abbreviation",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(111)")));
              return true;
            }
          }
          if (hit.empty()) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + tok + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return true;
          }
          for (const auto& h : hit)
            if (std::find(selected.begin(), selected.end(), h) == selected.end()) selected.push_back(h);
        }
      }
      auto recase = [&](std::string t) {
        for (size_t i = 0; i < t.size(); ++i) {
          char& c = t[i];
          const bool up = renUpper || (renProper && i == 0);
          c = up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                 : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return t;
      };
      // The collision check runs over the WHOLE mapping before a single rename
      // is issued. Two old names onto one new name is r(198) "new name
      // specified repeatedly" (ren7); a new name that is another variable
      // outside the set is r(110) "variable already exists" (ren3).
      std::map<std::string, std::string> target;  // newname -> the name claiming it
      std::vector<std::pair<std::string, std::string>> moves;
      for (const auto& nm : selected) {
        const std::string nn = recase(nm);
        auto it = target.find(nn);
        if (it != target.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 new name specified repeatedly in newname", std::nullopt,
              std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        target.emplace(nn, nm);
        if (nn != nm) moves.emplace_back(nm, nn);
      }
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end() &&
            std::find(selected.begin(), selected.end(), to) == selected.end()) {
          blocks.push_back(ojson(eval::errorBlock(
              "engine_error", "1 variable already exists: " + to, std::nullopt, std::nullopt,
              std::nullopt, "r(110)")));
          return true;
        }
      }
      if (moves.empty()) {
        ojson note;
        note["type"] = "display";
        note["class"] = "note";
        note["text"] = "(all newnames==oldnames)";
        blocks.push_back(note);
        return true;
      }
      // A rename whose TARGET is another variable's current name would be
      // rejected by runRename's own exists-guard even though the mapping as a
      // whole is a valid permutation (`A`,`b` -> `a`,`B` under upper/lower).
      // Two passes through a reserved prefix keeps every intermediate name
      // unique and never collides with a real one (`__b4ren_` is not a legal
      // Stata name a user could hold).
      std::vector<std::string> renamed;
      bool viaTemp = false;
      for (const auto& [from, to] : moves) {
        if (std::find(names.begin(), names.end(), to) != names.end()) viaTemp = true;
      }
      auto doRename = [&](const std::string& from, const std::string& to) -> bool {
        ojson r = handlers::runRename(from, to, session);
        if (r.is_object() && r.value("type", std::string()) == "error") {
          blocks.push_back(r);
          return false;
        }
        return true;
      };
      if (viaTemp) {
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename(moves[i].first, "__b4ren_" + std::to_string(i))) return true;
        }
        for (size_t i = 0; i < moves.size(); ++i) {
          if (!doRename("__b4ren_" + std::to_string(i), moves[i].second)) return true;
        }
      } else {
        for (const auto& [from, to] : moves) {
          if (!doRename(from, to)) return true;
        }
      }
      for (const auto& [from, to] : moves) renamed.push_back(to);
      ojson note;
      note["type"] = "display";
      note["class"] = "note";
      note["text"] = "(" + std::to_string(renamed.size()) + " variable" +
                     (renamed.size() == 1 ? "" : "s") + " renamed)";
      blocks.push_back(note);
      return true;
    }
    // `replace` on the 1:1 form is accepted (aer_w2 w2b_dataio
    // rename_replace_opt `rename x n2012, replace` rc 0, w2b_dataio.log:720).
    // The OLD name may be an abbreviation ([U] 11.2), resolved by the rule
    // keep/drop and the by keys use (StataBE rn.do: `rename number_people_clicked
    // ppl` renames number_people_clicked_w3; 007's appendix tables).
    std::string oldName = parsed.varlist[0];
    if (oldName.find_first_of("*?-") == std::string::npos) {
      std::vector<std::string> one = {oldName};
      if (auto err = handlers::resolveByKeyNames(session, one)) {
        blocks.push_back(*err);
        return true;
      }
      oldName = one.front();
    }
    blocks.push_back(ojson(handlers::runRename(oldName, parsed.varlist[1], session)));
    return true;
  }


  if (cmd == "clonevar") {
    // `clonevar new = old` or `clonevar new old` (both forms accepted —
    // strip a literal '=' then split on whitespace, mirrors commands.py's
    // `" ".join(vl).replace("=", " ").split()`).
    // BUG-127 class: an optional storage-type prefix (`clonevar double x = y`)
    // used to make `double` the NEWVAR and silently discard `x` — strip it
    // through the same stripStorageType gen/egen already use.
    StorageTypeInfo cvInfo = stripStorageType(joinVarlist(parsed.varlist));
    // BUG-1120 review: a storage type where NEWVAR stands names nothing, as in
    // generate: `clonevar double = x` and `clonevar str5 = s` are "too few
    // variables specified" r(102) (StataBE 19.5).
    if (!cvInfo.typeToken.empty() && trimWs(cvInfo.expr).rfind('=', 0) == 0) {
      if (auto r = strutil::generateNameRefusal(
              strippedTypeWord(joinVarlist(parsed.varlist), cvInfo))) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", r->message, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                "r(" + std::to_string(r->rc) + ")")));
        return true;
      }
    }
    if (cvInfo.stringTyped) {
      blocks.push_back(ojson(eval::engineError(
          "clonevar: string storage types are not supported — PIET stores f64/factor")));
      return true;
    }
    std::string joined = cvInfo.expr;
    for (auto& c : joined) {
      if (c == '=') c = ' ';
    }
    std::istringstream iss(joined);
    std::vector<std::string> toks;
    std::string tok;
    while (iss >> tok) toks.push_back(tok);
    if (toks.empty()) {
      if (auto err = conditionAsNameError(parsed.ifExpr)) {
        blocks.push_back(*err);
        return true;
      }
    }
    if (toks.size() < 2) {
      blocks.push_back(
          ojson(eval::engineError("clonevar: syntax is `clonevar NEWVAR = OLDVAR`")));
      return true;
    }
    // BS-031(c): parsed.ifExpr was already correctly extracted by the
    // tokenizer (`clonevar inc2 = income if year==1982`) but never forwarded
    // to the handler at all — see runClonevar's own comment for the fix.
    // BUG-1096: `in` rides along too (it was refused by name).
    blocks.push_back(ojson(handlers::runClonevar(toks.front(), toks.back(), session, parsed.ifExpr,
                                                 parsed.inRange)));
    appendGenMissingNote(blocks, toks.front());  // clonevar.ado's own `gen` prints it
    return true;
  }


  // `format varlist %fmt` and `format %fmt varlist` ([D] format: "are the same
  // commands"; 069 writes `format %td c_dob`, 075 `format %12.0fc N`). The
  // tokenizer has no special handling for a `%`-prefixed token and the plain-
  // varlist expansion passes it through, so the %fmt is the LAST token or the
  // FIRST one. No %fmt at all is `format [varlist]`, which lists the formats.
  if (cmd == "format") {
    // A comma inside the format token is part of the format, not the start of
    // an option list: `%9,2f` (comma decimal point), `%tdDay,_Month_dd,_CCYY`
    // (a date mask), and `%9.2f,` (a trailing comma, which makes the format
    // invalid: r(120)). The tokenizer cut the line at that comma; the format is
    // taken back whole from the raw line (vs-stata bank 5e917e3336e02fe8,
    // 9822301cdd1860c3, bfe2543015c54582, f25f934536642595).
    std::vector<std::string> toksFmt = parsed.varlist;
    std::string gluedFmt;
    if (!parsed.opts.empty() && !toksFmt.empty() && !toksFmt.back().empty() &&
        toksFmt.back()[0] == '%') {
      const size_t at = parsed.raw.rfind(toksFmt.back() + ",");
      if (at != std::string::npos) {
        size_t end = at;
        while (end < parsed.raw.size() && !std::isspace(static_cast<unsigned char>(parsed.raw[end])))
          ++end;
        gluedFmt = parsed.raw.substr(at, end - at);
        toksFmt.back() = gluedFmt;
      }
    }
    if (gluedFmt.empty()) {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {})) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    std::vector<std::string> fmtVarlist;
    std::string fmt;
    const std::vector<std::string>& toks = toksFmt;
    size_t nPct = 0;
    for (const auto& t : toks)
      if (!t.empty() && t[0] == '%') ++nPct;
    if (nPct == 0) {
      fmtVarlist = toks;
    } else if (nPct == 1 && toks.back()[0] == '%' && toks.size() > 1) {
      fmt = toks.back();
      fmtVarlist.assign(toks.begin(), toks.end() - 1);
    } else if (nPct == 1 && toks.front()[0] == '%' && toks.size() > 1) {
      fmt = toks.front();
      fmtVarlist.assign(toks.begin() + 1, toks.end());
    } else if (nPct == 1 && toks.size() == 1) {
      // A format and no variable: r(100) "varlist required" (vs-stata bank
      // `format %9.2f`).
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist required", std::nullopt,
                                              std::nullopt, std::nullopt, "r(100)")));
      return true;
    } else {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "format: syntax is `format varlist %fmt` or `format %fmt varlist`",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // runFormat returns an empty array as the silent-success sentinel (Stata's
    // `format` prints nothing); pushing it as a block would make downstream
    // block processing call .value("type") on an array, which throws. Guard
    // it exactly like the other silent handlers (matrix/label/log arms).
    // A numeric display format names its digits after the point (`%9g` is no
    // format), and only f and g take the comma suffix: `format price %9g` and
    // `format price %10.2ec` are r(120) "invalid %format" (vs-stata bank
    // 7f870b6678eef902, acc0d173e3394509).
    {
      static const std::regex reNoPoint(R"(^%-?0?[0-9]+[efg]c?$)", std::regex::icase);
      static const std::regex reEc(R"(^%-?0?[0-9]+\.[0-9]+ec$)", std::regex::icase);
      if (std::regex_match(fmt, reNoPoint) || std::regex_match(fmt, reEc)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid %format", std::nullopt,
                                                std::nullopt, std::nullopt, "r(120)")));
        return true;
      }
    }
    ojson fr = handlers::runFormat(fmtVarlist, fmt, session);
    if (!(fr.is_array() && fr.empty())) blocks.push_back(fr);
    return true;
  }


  // `label` with no subcommand (`label , zzabbrprobe`, `la , ...`) is Stata's
  // r(198) "invalid syntax" (vs-stata bank e0e5148faf4b97e2, 76dc539e088c4dca).
  if (cmd == "label" && parsed.varlist.empty()) {
    blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                            std::nullopt, std::nullopt, "r(198)")));
    return true;
  }
  if (cmd == "label" && !parsed.varlist.empty()) {
    // BUG-524: canonicalise the subcommand ONCE, here, against the floor
    // table (resolveLabelSub above); every branch below compares the full
    // spelling. Below a floor is Stata's "invalid syntax" r(198)
    // (label_d_too_short, label_va_too_short) and the data are untouched; so
    // is a token that is not a prefix of any documented subcommand (measured
    // in the vs-stata bank, below).
    //
    // This resolution now runs BEFORE the generic option-vocabulary reject
    // below (BUG-527): `label language NAME, new` needs the resolved
    // subcommand to intercept `, new` on its own terms (oracle rc 0), and
    // that decision has to be made before the generic net answers rc 198
    // for it.
    const std::string subTyped = lower(parsed.varlist[0]);
    std::string sub;
    switch (resolveLabelSub(subTyped, &sub)) {
      case LabelSubMatch::kResolved:
        break;
      case LabelSubMatch::kTooShort:
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                                "label " + subTyped, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      case LabelSubMatch::kUnknown:
        // A word that is no label subcommand is r(198) "invalid syntax" too
        // (vs-stata bank 43b4a61857d277c8 `label zznotasub`, 08071978c27981ca
        // `label delete lb`).
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax",
                                                "label " + subTyped, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
    }
    // BUG-527: `label language` takes its own options (new, copy, delete,
    // rename); kVocabLabel still lists new/rename/delete as unimplemented for
    // every OTHER subcommand.
    // BUG-336 (batch 2): known set measured from the handler body. `label define`
    // reads its own options by word from the raw tail (add/modify/replace/nofix,
    // each abbreviable: `, a` is add, lb_add_a), so it is not netted here; `label
    // values` takes nofix (lb_val_nofix); other label subcommands take none.
    // label copy's fromframe()/toframe() ([D] label) copy between frames, which
    // this build's frame layer does not carry value labels for: refused by name.
    if (sub == "copy") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, {"replace"},
                                                {"fromframe", "toframe"})) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (sub != "define" && sub != "language") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabel.known,
                                                kVocabLabel.unimplemented)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (sub == "variable") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label variable: missing varname")));
        return true;
      }
      std::string varname = parsed.varlist[1];
      // Text is everything after the varname token in the raw line, quotes stripped.
      // BUG-105/WS-K: typed spelling, not the literal — see the `save` arm's
      // comment. `labe variable x "t"` threw std::out_of_range here.
      // BUG-251/WS-Q: this one searched for the command word only to slice
      // FROM it — i.e. to strip leading whitespace that eval::tokenize() has
      // already stripped. `raw.substr(npos)` throws unconditionally, with no
      // wrap-around to hide it. trimWs() is the whole of what it wanted.
      std::string rawArgs = trimWs(parsed.raw);
      // strip "label variable VARNAME" prefix (3 tokens) — split on whitespace,
      // keep the 4th+ as text (mirrors commands.py's maxsplit=3).
      size_t p = 0;
      for (int k = 0; k < 3 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      // `label variable varname ["label"]` and nothing else: a second word before
      // the quoted label, or a qualifier, is r(198) "invalid syntax" (vs-stata
      // bank 788bc09ca62dd054 `label variable price mpg "two"`, ef1c17581b07352f
      // `label variable price "x" if foreign`).
      if (parsed.ifExpr.has_value() || parsed.inRange.has_value() ||
          (!text.empty() && text.front() != '"' && text.front() != '`' && text.front() != '\'' &&
           text.find('"') != std::string::npos) ||
          (text.size() > 1 && text.front() == '"' && text.find('"', 1) != std::string::npos &&
           !trimWs(text.substr(text.find('"', 1) + 1)).empty())) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      // EM9: an outer compound pair `"..."' is stripped too (`label var x `"the "x"
      // var"'`, w2af_macro_text_file.log:1560; w2ao lb_var_lab_cq log:124).
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);
      } else if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') ||
                                      (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
      }
      // The varname abbreviates (lb_var_abbrev: `label variable miss_age` labels
      // miss_age_PAP); a name that is not a variable is r(111) (lb_var_absent);
      // the label keeps its first 80 characters (lb_var_long, log:190).
      if (session.hasData()) {
        std::vector<std::string> one{varname};
        if (auto err = handlers::resolveByKeyNames(session, one)) {
          blocks.push_back(*err);
          return true;
        }
        Datastore* lds = session.datastore();
        if (one.size() != 1 || lds == nullptr || !lds->has(one.front())) {
          blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + varname +
                                                                      " not found",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(111)")));
          return true;
        }
        varname = one.front();
      }
      text = variableLabelText(text);
      ojson lv = handlers::runLabelVariable(varname, text, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "values") {
      if (parsed.varlist.size() < 2) {
        blocks.push_back(ojson(eval::engineError("label values: requires varname [lblname]")));
        return true;
      }
      // LBL-SEMANTICS: `label values VARLIST [LBLNAME|.]` -- the LAST token is the
      // set (`.` or none detaches) and every token before it a variable
      // (w2ao_label_family lb_val_multi `label values timecat fl db to`: all three
      // carry `to`); an absent variable is r(111) (:782 lb_val_absent) and a
      // string one r(181) "may not label strings" (:768 lb_val_str), each before
      // anything is attached.
      std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
      std::optional<std::string> lblname;
      if (vars.size() >= 2) {
        lblname = vars.back();
        vars.pop_back();
        if (*lblname == ".") lblname.reset();
      }
      bool nofix = false;
      for (const auto& [k, v] : parsed.opts)
        if (k == "nofix") nofix = true;
      if (session.hasData()) {
        loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(vars), session);
        if (ex.isError) {
          blocks.push_back(ex.error);
          return true;
        }
        vars = ex.tokens;
        if (auto err = handlers::resolveByKeyNames(session, vars)) {
          blocks.push_back(*err);
          return true;
        }
        Datastore* lds = session.datastore();
        for (const auto& v : vars) {
          if (lds == nullptr || !lds->has(v)) {
            blocks.push_back(ojson(eval::errorBlock("engine_error", "variable " + v + " not found",
                                                    std::nullopt, std::nullopt, std::nullopt,
                                                    "r(111)")));
            return true;
          }
        }
        if (lblname.has_value()) {
          for (const auto& v : vars) {
            if (lds->dtype(v) == "factor") {
              blocks.push_back(ojson(eval::errorBlock("engine_error", "may not label strings",
                                                      std::nullopt, std::nullopt, std::nullopt,
                                                      "r(181)")));
              return true;
            }
          }
        }
      }
      for (const auto& v : vars) {
        ojson lv = handlers::runLabelValues(v, lblname, session, nofix);
        if (!(lv.is_array() && lv.empty())) {
          blocks.push_back(lv);
          return true;
        }
      }
      return true;
    }
    if (sub == "define") {
      // BUG-105/WS-K: runLabelDefine anchors on `^label\s+define\s+`.
      // BUG-524: ... so the typed subcommand (`def`, `de`) is canonicalised in
      // the raw line too, or `label def yn2 0 "no"` would fail its own anchor.
      ojson lv = handlers::runLabelDefine(
          rawWithCanonicalLabelSub(rawWithCanonicalCmd(parsed.raw, cmdTyped, cmd), subTyped, sub),
          session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "list") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      // The words are label names: one that is no name is r(198) "X invalid
      // name" before any is looked up (vs-stata bank 6b6a7e7e66c80849 `label
      // list lb 1`).
      for (const auto& nm : names) {
        if (nm.find_first_of("*?") == std::string::npos && !strutil::isValidStataIdentifier(nm)) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", nm + " invalid name",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
      }
      ojson ll = ojson(handlers::runLabelList(names, session));
      if (ll.is_array()) {
        for (auto& b : ll) blocks.push_back(b);
      } else {
        blocks.push_back(std::move(ll));
      }
      return true;
    }
    if (sub == "data") {
      // Stata's documented behaviour: label data sets a dataset label of up to
      // 80 characters, and with no label given it removes the existing one.
      // session.dataLabel is the field the .dta writer already
      // carries (piet/session_state.h); this is its first in-session
      // mutator, so `label data` no longer stops a do-file (14 corpus uses in
      // 5 papers). Not an oracle arm -- the definition is the help file's.
      // Text is everything after the subcommand token, quotes stripped (the
      // `label variable` branch's own slicing, one token earlier).
      std::string rawArgs = trimWs(parsed.raw);
      size_t p = 0;
      for (int k = 0; k < 2 && p != std::string::npos; ++k) {
        p = rawArgs.find_first_of(" \t", p);
        if (p != std::string::npos) p = rawArgs.find_first_not_of(" \t", p);
      }
      std::string text = (p == std::string::npos) ? "" : trimWs(rawArgs.substr(p));
      if (eval::startsCompound(text, 0) && eval::compoundSpanEnd(text, 0) == text.size()) {
        text = text.substr(2, text.size() - 4);  // EM9, as `label variable`
      } else if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
      }
      // A dataset label holds 80 characters: a longer text is cut there, with
      // the note Stata prints (vs-stata bank 476490665286d96f).
      {
        size_t chars = 0, cut = text.size();
        for (size_t i = 0; i < text.size(); ++i) {
          if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
          if (chars == 80) {
            cut = i;
            break;
          }
          ++chars;
        }
        if (cut < text.size()) {
          text.resize(cut);
          ojson note;
          note["type"] = "display";
          note["text"] = "note: label truncated to 80 characters";
          blocks.push_back(note);
        }
      }
      session.dataLabel = text;
      return true;  // silent success, as `label variable` is
    }
    if (sub == "language") {
      // LBL-LANGUAGE: the whole subcommand (bare r(), new [copy], switch, delete,
      // rename) is handlers::runLabelLanguage; its options are its own.
      std::vector<std::string> args(parsed.varlist.begin() + 1, parsed.varlist.end());
      ojson lv = handlers::runLabelLanguage(args, parsed.opts, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    if (sub == "dir") {
      blocks.push_back(ojson(handlers::runLabelDir(session)));
      return true;
    }
    if (sub == "drop") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      if (names.empty()) {
        blocks.push_back(ojson(eval::engineError("label drop: specify at least one label name")));
        return true;
      }
      ojson lv = handlers::runLabelDrop(names, session);
      if (!(lv.is_array() && lv.empty())) blocks.push_back(lv);
      return true;
    }
    const bool replaceOpt = std::any_of(parsed.opts.begin(), parsed.opts.end(),
                                        [](const auto& kv) { return kv.first == "replace"; });
    if (sub == "copy" || sub == "rename") {
      std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
      if (names.size() != 2) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      for (const auto& nm : names) {
        if (!strutil::isValidStataIdentifier(nm)) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", nm + " invalid name",
                                                  std::nullopt, std::nullopt, std::nullopt,
                                                  "r(198)")));
          return true;
        }
      }
      pushEnvResult(blocks, handlers::runLabelCopyRename(sub == "rename", names[0], names[1],
                                                         replaceOpt, session));
      return true;
    }
    if (sub == "save") {
      // The words are read quote-aware from the typed text, so a quoted filename
      // with blanks (`using "my labels"`) is one word, as Stata reads it.
      std::vector<std::string> words;
      {
        std::string rest = strutil::trim(parsed.argText);
        while (!rest.empty()) {
          auto [tok, more] = eval::scanOneToken(rest);
          words.push_back(tok);
          rest = strutil::trim(more);
        }
        if (!words.empty()) words.erase(words.begin());  // the subcommand word
      }
      auto u = std::find(words.begin(), words.end(), "using");
      if (u == words.end() || u + 1 == words.end()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "using required", std::nullopt,
                                                std::nullopt, std::nullopt, "r(100)")));
        return true;
      }
      std::vector<std::string> names(words.begin(), u);
      std::string file = *(u + 1);
      if (u + 2 != words.end()) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      pushEnvResult(blocks, handlers::runLabelSave(names, file, replaceOpt, session));
      return true;
    }
    // BUG-524: what is left resolved to a documented subcommand PIET has no
    // store for. A recognised feature, refused by NAME -- never
    // accepted-and-ignored.
    blocks.push_back(ojson(eval::unsupportedCommand(
        "label " + sub, std::nullopt, std::nullopt, "piet_gap",
        eval::UnsupportedCommandKind::kNotImplemented)));
    return true;
  }


  // P1-W2-0: `labelbook [LBLNAME...]` — a top-level command (not a `label`
  // subcommand), so parsed.varlist holds the optional label-set names
  // directly (no subcommand-token stripping, unlike the `label list` arm
  // above).
  if (cmd == "labelbook") {
    // BUG-336 (batch 2): known set measured from the handler body -- runLabelbook takes no options.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabelbook.known, kVocabLabelbook.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // The names are value-label names: one that is no label set is r(111)
    // "value label NAME not found" (vs-stata bank `labelbook zznotalabel`), and
    // a word that is no name r(198) "X invalid name" (`labelbook *lbl`).
    for (const auto& nm : parsed.varlist) {
      if (!strutil::isValidStataIdentifier(nm)) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", nm + " invalid name", std::nullopt,
                                                std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      if (session.labelDefs.count(nm) == 0) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "value label " + nm + " not found",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(111)")));
        return true;
      }
    }
    // The report and the r() lists (names, gaps, numeric, blanks, null, nuniq,
    // nuniq_sh, notused) are the handler's.
    handlers::LabelbookOptions lbOpt;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "alpha") lbOpt.alpha = true;
      else if (k == "detail") lbOpt.detail = true;
      else if (k == "problems") lbOpt.problems = true;
      else if (k == "length" || k == "list") {
        long long n = 0;
        try {
          n = std::stoll(strutil::trim(v.strValue));
        } catch (const std::exception&) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", k + "() invalid", std::nullopt,
                                                  std::nullopt, std::nullopt, "r(198)")));
          return true;
        }
        if (k == "length") lbOpt.length = static_cast<int>(std::max<long long>(std::min<long long>(n, 100000), -1));
        else lbOpt.list = n;
      }
    }
    ojson lbBlk = ojson(handlers::runLabelbook(parsed.varlist, session, lbOpt));
    blocks.push_back(std::move(lbBlk));
    return true;
  }
  return false;
}

}
