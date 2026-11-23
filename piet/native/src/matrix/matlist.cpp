// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/results.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cclass.h"  // resolveCValue (c(linesize))
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "matrix_command_errors.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace matrix_command_detail;

namespace {

// ---------------------------------------------------------------------------
// Option vocabularies -- NAMESPACE SCOPE, not function-local statics.
//
// The registry is consulted by the DISPATCHER (canonicalizeForCommand, before
// the handler runs). A function-local static would only be constructed the
// first time the handler is entered, i.e. AFTER the one call that needed it, so
// the very first `svmat A, name(col)` in a session would canonicalize against
// an empty registry and land in the handler still spelled "name". Namespace
// scope registers at static-init time, which is what the dispatcher's ordering
// requires. registerOptVocab is documented safe there (function-local static
// registry, no cross-TU init order).

// matlist's declared-but-unimplemented set is large on purpose: every one of
// these changes the RENDERING, so accepting one silently would print a table
// that is not the table the user asked for.
const eval::OptVocab& kMatlistVocab = eval::registerOptVocab(
    {"matlist"},
    {{"format", "title", "names", "nonames", "noblank", "twidth", "cspec", "rspec", "lines",
      "border", "left", "right", "rightindent", "showcoleq", "roweqonly", "coleqonly",
      "aligncolnames", "nohalf", "nodotz", "underscore", "linesize", "rowtitle", "rowtitleleft",
      "tindent", "keepcoleq", "colorcoleq", "noheader", "bottomline"},
     {"debug", "roweqastxt"},
     // LINes() and ROWtitle() abbreviations that also prefix LINESIZE() or
     // ROWTITLELEFT / ROWEQONLY, which take no abbreviation ([P] matlist syntax).
     {{"lin", "lines"}, {"line", "lines"}, {"row", "rowtitle"}, {"rowt", "rowtitle"},
      {"rowti", "rowtitle"}, {"rowtit", "rowtitle"}, {"rowtitl", "rowtitle"}}});

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte label stays aligned.
std::string rjust(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

}  // namespace

// ---------------------------------------------------------------------------
// matlist

namespace {

// The display layer's own spec grammar (stata_format.h documents why each layer
// keeps its own regex and shares only the renderer): a FULL match, so `%td`,
// `%9s` and anything else non-numeric is refused by name instead of being
// handed to a numeric renderer that would answer something plausible for it.
bool parseNumericFmt(const std::string& raw, fmt::StataFmtSpec* sp) {
  static const std::regex re(R"(^%(-)?(0)?([0-9]+)?(?:\.([0-9]+))?([fgeFGE])$)");
  std::smatch m;
  if (!std::regex_match(raw, m, re)) return false;
  // A four-digit cap before std::stoi, not after: stoi THROWS on
  // `format(%99999999999.2f)`, and that exception has no catcher between here
  // and the dispatcher, so an unbounded width is a crash from user input rather
  // than a diagnostic. No Stata display format is wider than four digits.
  if (m[3].str().size() > 4 || m[4].str().size() > 4) return false;
  sp->leftJustify = m[1].matched;
  sp->zeroFill = m[2].matched;
  sp->hasWidth = m[3].matched;
  sp->width = m[3].matched ? std::stoi(m[3].str()) : 0;
  sp->prec = m[4].matched ? std::stoi(m[4].str()) : 0;
  sp->conv = static_cast<char>(std::tolower(static_cast<unsigned char>(m[5].str()[0])));
  return true;
}

std::string renderCell(double v, const fmt::StataFmtSpec& sp, size_t width) {
  std::string s;
  if (auto miss = eval::stataMissingToken(v)) {
    // Missing is its token at every width (`.`, or `.a` for a matrix cell that
    // holds one, BUG-1399) -- renderValue takes a finite double and has no
    // missing concept, so the check cannot be pushed down into it.
    s = *miss;
  } else if (std::optional<std::string> r = fmt::renderValue(v, sp)) {
    s = *r;
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    s = buf;
  }
  // padToField rather than a bare right-justify: right-justified IS its default,
  // so the ordinary `%9.0g` column comes out exactly as specified, and the one
  // documented exception (the `-` flag) is honoured instead of dropped. The net
  // ACCEPTS `format(%-9.2f)`, so ignoring the flag would be a silently different
  // table. A widthless spec has nothing for it to pad to, hence the column's own
  // width there.
  return sp.hasWidth ? fmt::padToField(s, sp) : rjust(s, width);
}

// ---------------------------------------------------------------------------
// matlist's general layout ([P] matlist): every table is a set of column
// separators 0..c+1 (spaces before, a vertical line or none, spaces after)
// and row separators 0..r+1 (a horizontal line or none). The display styles
// -- lines(), border(), left(), right(), format(), twidth() -- only choose
// those settings; cspec()/rspec() give them directly. Columns too wide for
// the line size are shown in blocks. In a text log a box's T pieces on the top
// and bottom lines are `-`, its left and right T pieces `|`, crossings and
// corners `+`. StataBE 19.5, vs-stata bank 69a93c15, 7429a7f3, fee28e3f,
// 5284d2c2, 5923ff21, 175f129f, 68cd8bec, f996f842, eb36a7d0, 6c247d6e,
// 94d1da05, 70af00d8, 43c712fc, 45cb6f2b, 4f69032b, c6cbf229, ae4a45f9,
// ae9b3d28, c5369b8b, bd181db4, 8b749133, b1602223, 69972e5a.
struct StyledOpts {
  bool showRows = true, showCols = true, coleqonly = false, roweqonly = false;
  std::string showcoleq = "first", aligncolnames = "ralign";
  bool keepcoleq = false, noblank = false, nohalf = false, nodotz = false, underscore = false;
  bool rightindent = false, bottomline = false, rowtitleleft = false;
  int linesize = 80, twidth = 12, left = 0, right = 0;
  std::optional<int> tindent;
  std::string title, rowtitle, format = "%9.0g", lines, cspec, rspec;
  bool border = false;
  std::string border2;
  bool hasLeft = false, hasRight = false, hasTwidth = false, hasFormat = false;
};

bool kwAbbrev(const std::string& w, const char* full, size_t shortest) {
  const std::string f(full);
  return w.size() >= shortest && w.size() <= f.size() && f.compare(0, w.size(), w) == 0;
}

std::string lalignTo(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}
std::string centerTo(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  if (n >= w) return s;
  const size_t l = (w - n) / 2;
  return std::string(l, ' ') + s + std::string(w - n - l, ' ');
}
std::string alignTo(const std::string& how, const std::string& s, size_t w) {
  if (how == "lalign") return lalignTo(s, w);
  if (how == "center") return centerTo(s, w);
  if (how == "rcenter") {  // the odd space goes on the left
    const size_t n = cecon::ident::codePointCount(s);
    if (n >= w) return s;
    const size_t l = (w - n + 1) / 2;
    return std::string(l, ' ') + s + std::string(w - n - l, ' ');
  }
  return rjust(s, w);
}
std::string hline(int n) { return n > 0 ? std::string(static_cast<size_t>(n), '-') : std::string(); }
std::string sp(int n) { return n > 0 ? std::string(static_cast<size_t>(n), ' ') : std::string(); }
std::string abbrevName(const std::string& s, size_t n) {
  if (cecon::ident::codePointCount(s) <= n) return s;
  return n > 4 ? s.substr(0, n - 2) + "~" + s.back() : s.substr(0, n);
}
std::string subUnderscore(std::string s) {
  for (char& ch : s)
    if (ch == '_') ch = ' ';
  return s;
}

// One separator of cspec(): `[o#]{&||}[o#]`, the defaults by whether it is a line.
std::optional<ojson> sepSpec(std::string& in, int& b, int& s, int& a, int b0, int a0, int b1, int a1) {
  const auto next = [&](bool take) {
    size_t i = 0;
    while (i < in.size() && in[i] == ' ') ++i;
    size_t j = i;
    if (j < in.size() && (in[j] == '|' || in[j] == '&')) ++j;
    else
      while (j < in.size() && in[j] != ' ' && in[j] != '|' && in[j] != '&') ++j;
    std::string tok = in.substr(i, j - i);
    if (take) in = in.substr(j);
    return tok;
  };
  std::optional<int> bv, av;
  std::string tok = next(true);
  const auto num = [](const std::string& t) -> std::optional<int> {
    if (t.size() < 2 || t[0] != 'o') return std::nullopt;
    for (size_t k = 1; k < t.size(); ++k)
      if (!std::isdigit(static_cast<unsigned char>(t[k]))) return std::nullopt;
    return std::stoi(t.substr(1));
  };
  if (!tok.empty() && tok[0] == 'o') {
    bv = num(tok);
    if (!bv.has_value()) return aerErrorRc("'" + tok.substr(1) + "' found where integer expected", 7);
    tok = next(true);
  }
  if (tok != "&" && tok != "|") return aerErrorRc("& or | expected, " + tok + " found", 198);
  s = tok == "|" ? 1 : 0;
  const std::string peek = next(false);
  if (!peek.empty() && peek[0] == 'o') {
    next(true);
    av = num(peek);
    if (!av.has_value()) return aerErrorRc("'" + peek.substr(1) + "' found where integer expected", 7);
  }
  b = bv.value_or(s ? b1 : b0);
  a = av.value_or(s ? a1 : a0);
  return std::nullopt;
}

// One column of cspec(): `[w#] [b|i|s] [c|e|r|t] [L|R|C] %fmt`, up to the
// next separator. Font and colour do not reach a text log.
std::optional<ojson> colSpec(std::string& in, std::string& fmtOut, int& wcol, std::string& align) {
  size_t stop = in.find_first_of("o|&");
  std::string spec = in.substr(0, stop);
  in = stop == std::string::npos ? std::string() : in.substr(stop);
  std::istringstream iss(spec);
  for (std::string tok; iss >> tok;) {
    if (tok[0] == '%') fmtOut = tok;
    else if (tok[0] == 'w') {
      try {
        size_t pos = 0;
        wcol = std::stoi(tok.substr(1), &pos);
        if (pos + 1 != tok.size()) throw std::invalid_argument("w");
      } catch (const std::exception&) {
        return aerErrorRc("'" + tok.substr(1) + "' found where integer expected", 7);
      }
    } else if (tok == "L") align = "lalign";
    else if (tok == "R") align = "ralign";
    else if (tok == "C") align = "center";
    else if (tok == "s" || tok == "b" || tok == "i" || tok == "t" || tok == "r" || tok == "e" || tok == "c") {
    } else {
      return aerErrorRc("option cspec() invalid; unknown input " + tok, 198);
    }
  }
  return std::nullopt;
}

std::optional<ojson> renderStyled(const eval::EvalMatrix& M, const StyledOpts& o, std::string& outText) {
  const int r = static_cast<int>(M.data.size());
  const int c = r > 0 ? static_cast<int>(M.data[0].size()) : 0;
  const bool showRows = o.showRows, showCols = o.showCols;
  const int c1 = showRows ? 0 : 1, r1 = showCols ? 0 : 1;
  std::vector<std::string> req(static_cast<size_t>(r)), rnm(static_cast<size_t>(r)),
      ceq(static_cast<size_t>(c)), cnm(static_cast<size_t>(c));
  const auto split = [](const std::string& full, std::string& eq, std::string& nm) {
    const size_t colon = full.find(':');
    eq = colon == std::string::npos ? "_" : full.substr(0, colon);
    if (eq.empty()) eq = "_";
    nm = colon == std::string::npos ? full : full.substr(colon + 1);
  };
  bool showRowEq = false, showColEq = false;
  for (int i = 0; i < r; ++i) {
    std::string full = static_cast<size_t>(i) < M.rownames.size() ? matrixNameAsListed(M.rownames[i]) : "";
    if (full.empty()) full = "r" + std::to_string(i + 1);
    split(full, req[i], rnm[i]);
    showRowEq = showRowEq || req[i] != "_";
  }
  for (int j = 0; j < c; ++j) {
    std::string full = static_cast<size_t>(j) < M.colnames.size() ? matrixNameAsListed(M.colnames[j]) : "";
    if (full.empty()) full = "c" + std::to_string(j + 1);
    split(full, ceq[j], cnm[j]);
    showColEq = showColEq || ceq[j] != "_";
  }
  if (o.keepcoleq && !showColEq)
    return aerErrorRc("column equations not found; option keepcoleq invalid", 198);
  bool loweronly = false;
  if (r == c && !o.nohalf) {
    loweronly = true;
    for (int i = 0; i < r && loweronly; ++i)
      for (int j = 0; j < i && loweronly; ++j) {
        const double a = M.data[i][j], b = M.data[j][i];
        loweronly = a == b || (std::isnan(a) && std::isnan(b));
      }
  }
  std::vector<int> bsep(c + 2, 0), sep(c + 2, 0), asep(c + 2, 0), wcol(c + 1, 0), Wcol(c + 1, 0);
  std::vector<int> rsep(r + 2, 0);
  std::vector<fmt::StataFmtSpec> fmts(c + 1);
  std::vector<std::string> align(c + 1, "ralign");
  int linesize = o.linesize;
  bool rightindent = o.rightindent;
  const bool specMode = !o.cspec.empty() || !o.rspec.empty();
  if (specMode) {
    const auto notAllowed = [](const char* name) {
      return aerErrorRc(std::string("option ") + name + " not allowed with rspec() and cspec()", 198);
    };
    if (!o.lines.empty()) return notAllowed("lines()");
    if (o.hasFormat) return notAllowed("format()");
    if (o.hasTwidth) return notAllowed("twidth()");
    if (o.hasLeft) return notAllowed("left()");
    if (o.hasRight) return notAllowed("right()");
    if (o.border || !o.border2.empty()) return notAllowed("border");
    if (o.rightindent) return notAllowed("rightindent");
    if (o.rspec.empty()) return aerErrorRc("rspec() required with cspec()", 198);
    if (o.cspec.empty()) return aerErrorRc("cspec() required with rspec()", 198);
    std::string cs = o.cspec;
    int d[4] = {0, 0, 0, 1};
    if (showRows) {
      if (auto e = sepSpec(cs, bsep[0], sep[0], asep[0], d[0], d[1], d[2], d[3])) return e;
      std::string f0;
      if (auto e = colSpec(cs, f0, wcol[0], align[0])) return e;
      static const std::regex reS(R"(^%(-)?([0-9]+)s$)");
      std::smatch sm;
      if (!std::regex_match(f0, sm, reS)) return aerErrorRc("invalid %format " + f0, 120);
      wcol[0] = std::max(wcol[0], std::stoi(sm.str(2)));
      d[0] = d[1] = d[2] = d[3] = 1;
    }
    for (int j = 1; j <= c; ++j) {
      if (auto e = sepSpec(cs, bsep[j], sep[j], asep[j], d[0], d[1], d[2], d[3])) return e;
      std::string fj;
      if (auto e = colSpec(cs, fj, wcol[j], align[j])) return e;
      if (fj.empty() || !parseNumericFmt(fj, &fmts[j]))
        return aerErrorRc("matlist: cspec() column format " + (fj.empty() ? std::string("(none)") : fj) +
                              " is not implemented in this version of PIET",
                          198);
      wcol[j] = std::max(wcol[j], static_cast<int>(renderCell(1.0, fmts[j], 1).size()));
      d[0] = d[1] = d[2] = d[3] = 1;
    }
    if (auto e = sepSpec(cs, bsep[c + 1], sep[c + 1], asep[c + 1], 0, 0, 1, 0)) return e;
    if (!strutil::trim(cs).empty()) return aerErrorRc("extraneous input in cspec(): " + strutil::trim(cs), 198);
    std::string rs = o.rspec;
    for (int i = r1; i <= r + 1; ++i) {
      size_t k = 0;
      while (k < rs.size() && rs[k] == ' ') ++k;
      if (k >= rs.size()) return aerErrorRc("too few specifications in rspec()", 198);
      const char ch = rs[k];
      if (ch != '&' && ch != '-' && ch != '|')
        return aerErrorRc(std::string("invalid horizontal separator specification ") + ch, 198);
      rsep[i] = ch != '&';
      rs = rs.substr(k + 1);
    }
    if (!strutil::trim(rs).empty()) return aerErrorRc("extraneous input in rspec(): " + strutil::trim(rs), 198);
  } else {
    // lines(): one style.
    std::string lstyle;
    {
      std::istringstream iss(strutil::lower(o.lines));
      int n = 0;
      for (std::string w; iss >> w; ++n) {
        if (kwAbbrev(w, "cells", 2)) lstyle = "cells";
        else if (kwAbbrev(w, "coltotal", 4)) lstyle = "coltotal";
        else if (kwAbbrev(w, "columns", 2)) lstyle = "columns";
        else if (w == "eq") lstyle = "eq";
        else if (kwAbbrev(w, "none", 1)) lstyle = "none";
        else if (kwAbbrev(w, "oneline", 1)) lstyle = "oneline";
        else if (kwAbbrev(w, "rctotal", 3)) lstyle = "rctotal";
        else if (kwAbbrev(w, "rowtotal", 4)) lstyle = "rowtotal";
        else if (kwAbbrev(w, "rows", 1)) lstyle = "rows";
        else return aerErrorRc("option " + w + " not allowed", 198);
      }
      if (n > 1) return aerErrorRc("lines() expects a single lstyle specification", 198);
    }
    bool bTop = false, bBottom = false, bLeft = false, bRight = false;
    if (!o.border2.empty()) {
      std::istringstream iss(strutil::lower(o.border2));
      for (std::string w; iss >> w;) {
        if (kwAbbrev(w, "top", 1)) bTop = true;
        else if (kwAbbrev(w, "left", 1)) bLeft = true;
        else if (kwAbbrev(w, "right", 1)) bRight = true;
        else if (kwAbbrev(w, "bottom", 1)) bBottom = true;
        else if (kwAbbrev(w, "columns", 3)) bLeft = bRight = true;
        else if (kwAbbrev(w, "rows", 3)) bTop = bBottom = true;
        else if (w == "all") bTop = bBottom = bLeft = bRight = true;
        else if (kwAbbrev(w, "none", 1)) {
        } else return aerErrorRc("option " + w + " not allowed", 198);
      }
    } else if (o.border) {
      bTop = bBottom = bLeft = bRight = true;
    }
    if (bRight) rightindent = true;
    if (lstyle.empty()) lstyle = showRowEq || showColEq ? "eq" : "oneline";
    if (!parseNumericFmt(o.format, &fmts[0])) return aerErrorRc("invalid %format " + o.format, 120);
    if (showRows) wcol[0] = o.twidth;
    for (int j = 1; j <= c; ++j) {
      fmts[j] = fmts[0];
      wcol[j] = static_cast<int>(renderCell(1.0, fmts[j], 1).size());
    }
    bsep[c1] = o.left;
    sep[c1] = 0;
    asep[c1] = 0;
    for (int j = c1 + 1; j <= c; ++j) {
      bsep[j] = 1;
      sep[j] = 0;
      asep[j] = 1;
    }
    bsep[c + 1] = 1;
    sep[c + 1] = 0;
    asep[c + 1] = o.right;
    const int sr = showRows ? 1 : 0, sc = showCols ? 1 : 0;
    if (lstyle == "oneline") {
      asep[0] = 0;
      sep[1] = sr;
      rsep[1] = sc;
    } else if (lstyle == "rowtotal") {
      asep[0] = 0;
      sep[1] = sr;
      rsep[1] = sc;
      rsep[r] = 1;
    } else if (lstyle == "coltotal") {
      sep[1] = sr;
      rsep[1] = sc;
      sep[c] = 1;
    } else if (lstyle == "rctotal") {
      asep[0] = 0;
      sep[1] = sr;
      rsep[1] = sc;
      rsep[r] = 1;
      sep[c] = 1;
    } else if (lstyle == "cells") {
      for (int j = c1 + 1; j <= c; ++j) sep[j] = 1;
      for (int i = r1 + 1; i <= r; ++i) rsep[i] = 1;
    } else if (lstyle == "columns") {
      for (int j = c1 + 1; j <= c; ++j) sep[j] = 1;
      rsep[r1 + 1] = sc;
    } else if (lstyle == "rows") {
      for (int i = r1 + 1; i <= r; ++i) rsep[i] = 1;
      sep[c1 + 1] = sr;
    } else if (lstyle == "eq") {
      sep[c1 + 1] = sr;
      rsep[r1 + 1] = sc;
      for (int j = 2; j <= c; ++j)
        if (ceq[j - 1] != ceq[j - 2]) sep[j] = 1;
      for (int i = 2; i <= r; ++i)
        if (req[i - 1] != req[i - 2]) rsep[i] = 1;
    }
    if (bTop) rsep[r1] = 1;
    if (bBottom) rsep[r + 1] = 1;
    if (bLeft) {
      sep[c1] = 1;
      asep[c1] = 1;
    }
    if (bRight) {
      sep[c + 1] = 1;
      bsep[c + 1] = 1;
    }
  }
  if (o.bottomline) rsep[r + 1] = 1;
  if (showRows) Wcol[0] = asep[0] + wcol[0] + bsep[1];
  for (int j = 1; j <= c; ++j) Wcol[j] = asep[j] + wcol[j] + bsep[j + 1];
  if (!rightindent && !specMode) {
    Wcol[c] -= 1;
    linesize -= 1;
  }
  const auto ch = [&](const char* piece, int s) { return s ? std::string(piece) : std::string(); };
  // Names as shown.
  const int w0 = wcol[0];
  std::vector<std::string> rshow(static_cast<size_t>(r));
  for (int i = 0; i < r; ++i) {
    rshow[i] = abbrevName(rnm[i], static_cast<size_t>(std::max(w0, 0)));
    if (o.underscore) rshow[i] = subUnderscore(rshow[i]);
  }
  const auto eqShown = [](const std::string& eq, int w) {
    if (w > 4) return eq == "_" ? std::string() : abbrevName(eq, static_cast<size_t>(w));
    return eq.substr(0, static_cast<size_t>(std::max(w, 0)));
  };
  std::ostringstream out;
  if (!o.noblank) out << "\n";
  if (!o.title.empty()) {
    const int ind = o.tindent.value_or(0);
    out << sp(ind) << o.title << "\n\n";
  }
  int j2 = 0;
  while (j2 < c) {
    if (j2 > 0) out << "\n";
    const int j1 = j2 + 1;
    j2 = j1;
    int jwidth = bsep[c1] + sep[c1] + Wcol[j1] + sep[c + 1] + asep[c + 1];
    if (showRows) jwidth += Wcol[0] + sep[1];
    if (!o.keepcoleq) {
      bool more = j2 < c && jwidth < linesize;
      while (more) {
        const int p = j2 + 1;
        const int wp = jwidth + sep[p] + Wcol[p];
        if (wp <= linesize) {
          j2 = p;
          jwidth = wp;
          more = j2 < c;
        } else {
          more = false;
        }
      }
    } else {
      bool more = j2 < c && jwidth < linesize;
      while (more) {
        const int p = j2 + 1;
        const int wp = jwidth + sep[p] + Wcol[p];
        if (wp <= linesize && ceq[j1 - 1] == ceq[p - 1]) {
          j2 = p;
          jwidth = wp;
          more = j2 < c;
        } else {
          more = false;
        }
      }
      more = j2 < c && jwidth < linesize;
      while (more) {
        const int j3 = j2 + 1;
        int j4 = j3;
        while (j4 <= c && ceq[j3 - 1] == ceq[j4 - 1]) ++j4;
        --j4;
        int wp = jwidth;
        for (int j = j3; j <= j4; ++j) wp += sep[j] + Wcol[j];
        if (wp < linesize) {
          j2 = j4;
          jwidth = wp;
          more = j2 < c;
        } else {
          more = false;
        }
      }
    }
    const auto vbar = [&](int j) { return ch("|", j == j2 ? sep[c + 1] : sep[j + 1]); };
    const auto ruleLine = [&](const char* L, const char* C, const char* R) {
      out << sp(bsep[c1]) << ch(L, sep[c1]);
      if (showRows) out << hline(Wcol[0]) << ch(C, sep[c1 + 1]);
      for (int j = j1; j < j2; ++j) out << hline(Wcol[j]) << ch(C, sep[j + 1]);
      out << hline(Wcol[j2]) << ch(R, sep[c + 1]) << "\n";
    };
    if (rsep[r1]) ruleLine("+", "-", "+");
    const auto leadBlank = [&] {
      out << sp(bsep[c1]) << ch("|", sep[c1]);
      if (showRows) out << sp(Wcol[0]) << ch("|", sep[c1 + 1]);
    };
    if ((showCols && showColEq) || o.coleqonly) {
      leadBlank();
      if (o.showcoleq == "each") {
        for (int j = j1; j <= j2; ++j)
          out << sp(asep[j]) << rjust(eqShown(ceq[j - 1], wcol[j]), static_cast<size_t>(wcol[j]))
              << sp(bsep[j + 1]) << vbar(j);
      } else if (o.showcoleq == "first") {
        std::string hold;
        bool first = true;
        for (int j = j1; j <= j2; ++j) {
          if (first || ceq[j - 1] != hold) {
            out << sp(asep[j]) << lalignTo(eqShown(ceq[j - 1], wcol[j]), static_cast<size_t>(wcol[j]))
                << sp(bsep[j + 1]);
            hold = ceq[j - 1];
            first = false;
          } else {
            out << sp(Wcol[j]);
          }
          out << vbar(j);
        }
      } else {  // combined, lcombined, rcombined
        const std::string how = o.showcoleq == "lcombined"   ? "lalign"
                                : o.showcoleq == "rcombined" ? "ralign"
                                                             : "rcenter";
        std::string hold = ceq[j1 - 1];
        int eqw = wcol[j1], beq = asep[j1];
        for (int j = j1 + 1; j <= j2; ++j) {
          if (ceq[j - 1] == hold) {
            eqw += asep[j] + sep[j] + bsep[j + 1] + wcol[j];
          } else {
            out << sp(beq) << alignTo(how, eqShown(hold, eqw), static_cast<size_t>(eqw)) << sp(bsep[j])
                << ch("|", sep[j]);
            hold = ceq[j - 1];
            eqw = wcol[j];
            beq = asep[j];
          }
        }
        const std::string last = eqw > 4 ? abbrevName(hold, static_cast<size_t>(eqw))
                                         : hold.substr(0, static_cast<size_t>(eqw));
        out << sp(beq) << alignTo(how, last, static_cast<size_t>(eqw)) << sp(bsep[c + 1])
            << ch("|", sep[c + 1]);
      }
      out << "\n";
    }
    if (showCols && !o.coleqonly) {
      out << sp(bsep[c1]) << ch("|", sep[c1]);
      if (showRows) {
        const std::string rt = o.rowtitle.substr(0, static_cast<size_t>(std::max(w0, 0)));
        out << sp(asep[0])
            << (o.rowtitleleft ? lalignTo(rt, static_cast<size_t>(w0)) : rjust(rt, static_cast<size_t>(w0)))
            << sp(bsep[1]) << ch("|", sep[c1 + 1]);
      }
      for (int j = j1; j <= j2; ++j) {
        std::string ab = wcol[j] > 4 ? abbrevName(cnm[j - 1], static_cast<size_t>(wcol[j]))
                                     : cnm[j - 1].substr(0, static_cast<size_t>(wcol[j]));
        if (o.underscore) ab = subUnderscore(ab);
        out << sp(asep[j]) << alignTo(o.aligncolnames, ab, static_cast<size_t>(wcol[j])) << sp(bsep[j + 1])
            << vbar(j);
      }
      out << "\n";
    }
    std::string reqhold;
    bool haveHold = false;
    const int lowi = loweronly ? j1 : 1;
    for (int i = lowi; i <= r; ++i) {
      const bool showh = !loweronly ? (rsep[i] && (i != 1 || showCols))
                                    : (i == lowi ? (rsep[1] && showCols) : rsep[i] != 0);
      if (showh) ruleLine("|", "+", "|");
      if ((showRows && showRowEq) || o.roweqonly) {
        if (!haveHold || req[i - 1] != reqhold) {
          std::string ab = w0 > 4 ? eqShown(req[i - 1], w0) : req[i - 1].substr(0, static_cast<size_t>(w0));
          out << sp(bsep[c1]) << ch("|", sep[c1]) << lalignTo(ab, static_cast<size_t>(Wcol[0]))
              << ch("|", sep[c1 + 1]);
          for (int j = j1; j < j2; ++j) out << sp(Wcol[j]) << ch("|", sep[j + 1]);
          out << sp(Wcol[j2]) << ch("|", sep[c + 1]) << "\n";
          reqhold = req[i - 1];
          haveHold = true;
        }
      }
      out << sp(bsep[c1]) << ch("|", sep[c1]);
      if (showRows) {
        const std::string nm = o.roweqonly && w0 > 4 ? std::string(" ") : rshow[i - 1];
        out << sp(asep[0]) << rjust(nm, static_cast<size_t>(w0)) << sp(bsep[1]) << ch("|", sep[c1 + 1]);
      }
      for (int j = j1; j <= j2; ++j) {
        const double v = M.data[i - 1][j - 1];
        std::string text;
        if ((o.nodotz && eval::stataExtendedMissingKind(v).value_or(0) == 26) || (loweronly && j > i))
          text = sp(wcol[j]);
        else
          text = renderCell(v, fmts[j], static_cast<size_t>(wcol[j]));
        out << sp(asep[j]) << alignTo(align[j], text, static_cast<size_t>(wcol[j])) << sp(bsep[j + 1])
            << vbar(j);
      }
      out << "\n";
    }
    if (rsep[r + 1]) ruleLine("+", "-", "+");
  }
  outText = out.str();
  return std::nullopt;
}


}  // namespace

ojson runMatlist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kMatlistVocab.known,
                                            kMatlistVocab.unimplemented)) {
    return ojson(*err);
  }

  // EXP comes off the RAW line, not off parsed.varlist: the expression may be
  // `e(b)`, whose parentheses the varlist tokenizer has no reason to keep in one
  // piece, and re-joining split tokens would be a guess about how they were
  // split. findUnquotedComma is the same option-boundary scan the tokenizer
  // itself uses, so `format(%9.3f)`'s inner text can never be mistaken for the
  // boundary.
  std::string rest = strutil::trim(parsed.raw);
  const size_t sp0 = rest.find_first_of(" \t");
  rest = (sp0 == std::string::npos) ? std::string() : strutil::trim(rest.substr(sp0));
  const long long comma = eval::findUnquotedComma(rest);
  if (comma >= 0) rest = rest.substr(0, static_cast<size_t>(comma));
  const std::string expr = strutil::trim(rest);
  if (expr.empty()) return aerError("matlist: no matrix specified");

  eval::EvalMatrix owned;
  const eval::EvalMatrix* mat = nullptr;
  // e(<name>) / r(<name>), not just e(b)/e(V). The old spelling was `([bV])`,
  // which was right while b and V were the only e-matrices anything posted;
  // pca posts seven (Ev, L, C, Psi, means, sds, components) and sktest posts
  // r(table), and under the old regex every one of them was unreachable -- the
  // command would publish a matrix that no syntax could name.
  // Parsed by hand, not a regex: the name may be Unicode (bytes >= 0x80 are
  // identifier bytes, core/ident.h), which a narrow-char bracket range cannot
  // express portably.
  char cls = 0;
  std::string name;
  {
    size_t p = 0;
    if (expr.size() > 2 && (expr[0] == 'e' || expr[0] == 'r') && expr[1] == '(') {
      p = 2;
      while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
      const size_t b = p;
      if (p < expr.size() && cecon::ident::isIdentStartByte(expr[p])) {
        while (p < expr.size() && cecon::ident::isIdentByte(expr[p])) ++p;
        name = expr.substr(b, p - b);
        while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
        if (p + 1 == expr.size() && expr[p] == ')') cls = expr[0];
        else name.clear();
      }
    }
  }
  if (cls != 0) {
    if (cls == 'e') {
      // eb-order T-P1: the one resolver `matrix N = e(NAME)` uses
      // (EvalSession::resolveEMatrix), so `matlist e(b)` and the copied matrix
      // cannot disagree. Its second copy of the coeftable synthesis (with a
      // diagonal e(V)) is gone. A refusal carries the resolver's rc: r(301)
      // with no e() at all (w2g_ebmat.log:2238 e15c_fresh_matlist_eb), r(111)
      // for a name the live e() does not have -- never the coarse rc 1.
      int rc = 111;
      std::string why;
      std::optional<eval::EvalMatrix> resolved = session.eval.resolveEMatrix(name, &rc, &why);
      // A live e() with no e(b)/e(V): rc 0, nothing to list (w2g_ebmat.log:2096
      // e15c_matlist_eb after pca; w2v_estpost_ematrix.log ep13_matlist_eb).
      if (!resolved.has_value() && session.eval.eMatrixAbsentFromLiveE(name))
        resolved = eval::EvalMatrix{};
      if (!resolved.has_value()) return aerErrorRc("matlist: " + why, rc);
      owned = std::move(*resolved);
      mat = &owned;
    } else if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, name)) {
      mat = posted;
    } else {
      return aerErrorRc("matlist: matrix " + expr + " not found", 111);
    }
  } else {
    mat = session.eval.findMatrix(expr);
    if (mat == nullptr && !strutil::isValidStataIdentifier(expr)) {
      // A matrix expression (`matlist A*2`, `matlist A'*A`; vs-stata bank
      // daa80616) is listed as its value: evaluated as `matrix define` would,
      // into a scratch matrix that is dropped again.
      const std::string tmp = "__piet_matlist_expr";
      ojson blocks = dispatch("matrix " + tmp + " = " + expr, session);
      for (const auto& blk : blocks)
        if (blk.is_object() && blk.value("type", std::string()) == "error") return blk;
      if (const eval::EvalMatrix* v = session.eval.findMatrix(tmp)) owned = *v;
      (void)dispatch("capture matrix drop " + tmp, session);
      mat = &owned;
    }
    if (mat == nullptr) return aerErrorRc("matlist: matrix " + expr + " not found", 111);
  }

  std::string fmtRaw = "%9.0g";
  std::string title;
  std::string namesArg = "all";
  bool nonames = false;
  bool namesGiven = false;
  bool noblank = false;
  int twidth = 12;
  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "format") {
      if (v.isBool || v.strValue.empty()) return aerError("matlist: option format() requires a %fmt");
      fmtRaw = strutil::trim(v.strValue);
    } else if (k == "title") {
      if (v.isBool) return aerError("matlist: option title() requires a string");
      title = strutil::trim(v.strValue);
      if (title.size() >= 2 && title.front() == '"' && title.back() == '"') {
        title = title.substr(1, title.size() - 2);
      }
    } else if (k == "names") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("matlist: option names() requires all, rows, columns or none");
      }
      namesArg = strutil::lower(strutil::trim(v.strValue));
      namesGiven = true;
      // `names(col)` is columns (StataBE 19.5, vs-stata bank 6fa5507cff15aba2).
      for (const char* full : {"rows", "columns"}) {
        const std::string f(full);
        if (namesArg.size() >= 3 && namesArg.size() < f.size() && f.compare(0, namesArg.size(), namesArg) == 0)
          namesArg = f;
      }
    } else if (k == "nonames") {
      nonames = true;
    } else if (k == "noblank") {
      noblank = true;
    } else if (k == "twidth") {
      if (v.isBool) return aerError("matlist: option twidth() requires a number");
      try {
        twidth = std::stoi(strutil::trim(v.strValue));
      } catch (const std::exception&) {
        // StataBE 19.5: r(121) (vs-stata bank 19ece66bccf3f088 `twidth(abc)`).
        return aerErrorRc("twidth() invalid -- invalid number", 121);
      }
      if (twidth < 1 || twidth > 200) {
        return aerError("matlist: twidth(" + strutil::trim(v.strValue) +
                        ") is out of range; twidth() must be between 1 and 200");
      }
    }
  }

  // `nonames` with `names()` is r(198) (StataBE 19.5, vs-stata bank
  // 4225e65f6c428e60); a format that is no %format at all is r(120)
  // `invalid %format zz` (0db4ff4206355e0f).
  if (nonames && namesGiven) return aerErrorRc("names and names() are exclusive", 198);
  if (fmtRaw.empty() || fmtRaw[0] != '%') return aerErrorRc("invalid %format " + fmtRaw, 120);
  if (namesArg != "all" && namesArg != "rows" && namesArg != "columns" && namesArg != "none") {
    return aerError("matlist: names(" + namesArg + ") is not one of all, rows, columns, none",
                    "syntax_error");
  }
  fmt::StataFmtSpec spec;
  if (!parseNumericFmt(fmtRaw, &spec)) {
    // A `%td`/`%9s` here is not a rendering nicety: matlist renders NUMBERS, and
    // silently falling back to the default would print a different table from
    // the one that was asked for, with no sign that the format was dropped.
    return aerError("matlist: format(" + fmtRaw +
                        ") is not a numeric display format (%w.Pf, %w.Pg or %w.Pe)",
                    "syntax_error");
  }
  const size_t cellWidth = spec.hasWidth && spec.width > 0 ? static_cast<size_t>(spec.width) : 9;

  const bool showRows = !nonames && (namesArg == "all" || namesArg == "rows");
  const bool showCols = !nonames && (namesArg == "all" || namesArg == "columns");

  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  // A 0x0 matrix (`matrix b = e(b)` after estpost/pca, or e(b) itself there) lists
  // nothing at rc 0 (w2g_ebmat.log:2096 e15c_matlist_eb; w2v_estpost_ematrix.log
  // ep13_matlist_eb, ep15c_matlist_eb) -- it used to be an rc-1 error.
  if (nrows == 0 || ncols == 0) {
    ojson empty;
    empty["type"] = "echo";
    empty["text"] = "";
    return empty;
  }

  // Every layout but one of factor-variable or time-series names goes through
  // the general layout (renderStyled); those names keep the default layout
  // below, which draws their operator line, and refuse the layout options.
  bool opNames = false;
  for (const auto* stripe : {&mat->rownames, &mat->colnames})
    for (const auto& full : *stripe) {
      const std::string nm = full.substr(full.find(':') == std::string::npos ? 0 : full.find(':') + 1);
      opNames = opNames || nm.find_first_of(".#") != std::string::npos;
    }
  StyledOpts so;
  so.showRows = showRows;
  so.showCols = showCols;
  so.noblank = noblank;
  so.title = title;
  so.format = fmtRaw;
  so.twidth = twidth;
  if (auto ls = resolveCValue("linesize", session); ls.has_value() && !ls->isString) {
    try {
      so.linesize = std::stoi(ls->text);
    } catch (...) {
    }
  }
  bool layoutOption = false;
  for (const auto& [k, v] : parsed.opts) {
    const std::string val = v.isBool ? std::string() : strutil::trim(v.strValue);
    const auto intArg = [&](int lo, int hi, int& dst) -> std::optional<ojson> {
      long long n = 0;
      try {
        size_t pos = 0;
        const double d = std::stod(val, &pos);
        if (pos != val.size()) throw std::invalid_argument("n");
        if (d != std::floor(d)) return aerErrorRc(k + "() invalid -- invalid numlist has noninteger elements", 126);
        n = static_cast<long long>(d);
      } catch (const std::exception&) {
        return aerErrorRc(k + "() invalid -- invalid number", 121);
      }
      if (n < lo || n > hi)
        return aerErrorRc(k + "() invalid -- invalid numlist has elements outside of allowed range", 125);
      dst = static_cast<int>(n);
      return std::nullopt;
    };
    if (k == "format" || k == "title" || k == "names" || k == "nonames" || k == "noblank" ||
        k == "noheader" || k == "colorcoleq") {
      if (k == "format") so.hasFormat = true;
      continue;
    }
    if (k == "twidth") {
      so.hasTwidth = true;
      continue;
    }
    layoutOption = true;
    if (k == "cspec") so.cspec = val;
    else if (k == "rspec") so.rspec = val;
    else if (k == "lines") so.lines = val;
    else if (k == "border") {
      if (v.isBool) so.border = true;
      else so.border2 = val.empty() ? std::string("none") : val;
    } else if (k == "left") {
      so.hasLeft = true;
      if (auto e = intArg(0, 1 << 20, so.left)) return *e;
    } else if (k == "right") {
      so.hasRight = true;
      if (auto e = intArg(0, 1 << 20, so.right)) return *e;
    } else if (k == "tindent") {
      int t = 0;
      if (auto e = intArg(0, 1 << 20, t)) return *e;
      so.tindent = t;
    } else if (k == "linesize") {
      if (auto e = intArg(40, 255, so.linesize)) return *e;
    } else if (k == "rightindent") so.rightindent = true;
    else if (k == "showcoleq") {
      const std::string w = strutil::lower(val);
      if (w.empty() || kwAbbrev(w, "first", 1)) so.showcoleq = "first";
      else if (kwAbbrev(w, "each", 1)) so.showcoleq = "each";
      else if (kwAbbrev(w, "combined", 1)) so.showcoleq = "combined";
      else if (kwAbbrev(w, "lcombined", 1)) so.showcoleq = "lcombined";
      else if (kwAbbrev(w, "rcombined", 1)) so.showcoleq = "rcombined";
      else return aerErrorRc("option " + w + " not allowed", 198);
    } else if (k == "aligncolnames") {
      const std::string w = strutil::lower(val);
      if (w.empty() || kwAbbrev(w, "ralign", 1)) so.aligncolnames = "ralign";
      else if (kwAbbrev(w, "center", 1)) so.aligncolnames = "center";
      else if (kwAbbrev(w, "lalign", 1)) so.aligncolnames = "lalign";
      else return aerErrorRc("option " + w + " not allowed", 198);
    } else if (k == "roweqonly") so.roweqonly = true;
    else if (k == "coleqonly") so.coleqonly = true;
    else if (k == "nohalf") so.nohalf = true;
    else if (k == "nodotz") so.nodotz = true;
    else if (k == "underscore") so.underscore = true;
    else if (k == "rowtitle") {
      so.rowtitle = val;
      if (so.rowtitle.size() >= 2 && so.rowtitle.front() == '"' && so.rowtitle.back() == '"')
        so.rowtitle = so.rowtitle.substr(1, so.rowtitle.size() - 2);
    } else if (k == "rowtitleleft") so.rowtitleleft = true;
    else if (k == "keepcoleq") so.keepcoleq = true;
    else if (k == "bottomline") so.bottomline = true;
  }
  if (so.coleqonly && !showCols)
    return aerErrorRc("coleqonly may not be combined with names(rows), names(none) or nonames", 198);
  if (so.roweqonly && !showRows)
    return aerErrorRc("roweqonly may not be combined with names(columns), names(none) or nonames", 198);
  if (opNames && layoutOption)
    return aerErrorRc("matlist: these layout options with factor-variable or time-series row or "
                      "column names are not implemented in this version of PIET",
                      198);
  if (!opNames) {
    std::string text;
    if (auto e = renderStyled(*mat, so, text)) return *e;
    ojson styled;
    styled["type"] = "display";
    styled["text"] = text;
    return styled;
  }

  // StataBE 19.5's default layout (BUG-1412; 2026-11-03 oracle, line for line):
  //
  //                | e1                   | e2
  //                |         x          y |         z
  //   -------------+----------------------+----------
  //   q            |                      |
  //              a |         1          2 |         3
  //   -------------+----------------------+----------
  //   w            |                      |
  //              b |         4          5 |         6
  //
  // - A column is one blank, the cell (the format's width w, 9 for %9.0g), one
  //   blank; the first has no leading blank without row names. The row-name
  //   field is twidth() wide plus one blank, then `|`.
  // - A column-equation change puts a `|` before the column, on every line.
  //   With any equation, a header line carries each group's equation once,
  //   left-aligned in the first column's w; with any row equation, each run
  //   of rows with one equation opens with a line holding its equation (blank
  //   for none) and a separator line comes before every run but the first.
  // - A factor/operator name puts its `op.` (or the part up to the last `#`)
  //   on a line above, right-aligned to end one place past the cell, `bn`
  //   dropped. Names are cut as abbrev() cuts them, to w (columns, equations)
  //   or twidth (rows).
  // - Separator lines and the blank stretch of an equation line stop one
  //   place short of the last column's right edge.
  // - A symmetric matrix prints its lower triangle.
  const auto abbrev = [](const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return n > 4 ? s.substr(0, n - 2) + "~" + s.back() : s.substr(0, n);
  };
  const auto splitEq = [](std::string full, std::string* eq) {
    const size_t colon = full.find(':');
    eq->clear();
    if (colon != std::string::npos) {
      *eq = full.substr(0, colon);
      full = full.substr(colon + 1);
    }
    if (*eq == "_") eq->clear();  // Stata's name for "no equation" among named ones
    return full;
  };
  const size_t tw = static_cast<size_t>(twidth);
  const size_t w = cellWidth;
  std::vector<std::string> req(nrows), rname(nrows), ceq(ncols), cname(ncols), cop(ncols);
  bool anyRowEq = false, anyColEq = false, anyOp = false;
  for (size_t i = 0; i < nrows; ++i) {
    std::string full = i < mat->rownames.size() ? matrixNameAsListed(mat->rownames[i]) : "";
    if (full.empty()) full = "r" + std::to_string(i + 1);
    rname[i] = abbrev(splitEq(full, &req[i]), tw);
    anyRowEq = anyRowEq || !req[i].empty();
  }
  for (size_t j = 0; j < ncols; ++j) {
    std::string full = j < mat->colnames.size() ? matrixNameAsListed(mat->colnames[j]) : "";
    if (full.empty()) full = "c" + std::to_string(j + 1);
    std::string name = splitEq(full, &ceq[j]);
    anyColEq = anyColEq || !ceq[j].empty();
    size_t cut = name.rfind('#');
    if (cut == std::string::npos) cut = name.rfind('.');
    if (cut != std::string::npos && cut > 0 && cut + 1 < name.size()) {
      cop[j] = abbrev(name.substr(0, cut), w) + name[cut];
      name = name.substr(cut + 1);
      anyOp = true;
    }
    cname[j] = abbrev(name, w);
  }
  bool lowerOnly = nrows == ncols;
  for (size_t i = 0; i < nrows && lowerOnly; ++i) {
    for (size_t j = 0; j < i && lowerOnly; ++j) {
      const double a = mat->data[i][j], b = mat->data[j][i];
      lowerOnly = a == b || (std::isnan(a) && std::isnan(b));
    }
  }

  const auto lead = [&](size_t j) -> size_t { return j == 0 && !showRows ? 0 : 1; };
  const auto full = [&](size_t j) { return lead(j) + w + 1; };
  const auto fill = [&](size_t j) { return full(j) - (j + 1 == ncols ? 1 : 0); };
  const auto bar = [&](size_t j) {
    return j + 1 < ncols && anyColEq && ceq[j + 1] != ceq[j] ? "|" : "";
  };
  const auto ljust = [](const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
  };
  const std::string headLead = showRows ? std::string(tw + 1, ' ') + "|" : std::string();
  std::ostringstream out;
  const auto separator = [&] {
    if (showRows) out << std::string(tw + 1, '-') << "+";
    for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), '-') << (*bar(j) ? "+" : "");
    out << "\n";
  };

  if (!noblank) out << "\n";
  if (!title.empty()) out << title << "\n\n";
  if (showCols && anyColEq) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (j == 0 || ceq[j] != ceq[j - 1])
        out << std::string(lead(j), ' ') << ljust(abbrev(ceq[j], w), w) << ' ';
      else
        out << std::string(fill(j), ' ');
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols && anyOp) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (cop[j].empty()) out << std::string(full(j), ' ');
      else out << std::string(lead(j), ' ') << rjust(cop[j], w + 1);
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j)
      out << std::string(lead(j), ' ') << rjust(cname[j], w) << ' ' << bar(j);
    out << "\n";
  }
  for (size_t i = 0; i < nrows; ++i) {
    const bool newGroup = anyRowEq && (i == 0 || req[i] != req[i - 1]);
    if (i == 0 ? showCols : newGroup) separator();
    if (newGroup && showRows) {
      out << ljust(abbrev(req[i], tw), tw) << " |";
      for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), ' ') << bar(j);
      out << "\n";
    }
    if (showRows) out << rjust(rname[i], tw) << " |";
    for (size_t j = 0; j < ncols; ++j) {
      if (lowerOnly && j > i) {
        out << std::string(full(j), ' ');
      } else {
        const double v = j < mat->data[i].size() ? mat->data[i][j]
                                                 : std::numeric_limits<double>::quiet_NaN();
        out << std::string(lead(j), ' ') << renderCell(v, spec, w) << ' ';
      }
      out << bar(j);
    }
    out << "\n";
  }

  ojson block;
  block["type"] = "display";  // output, not the command echo (BUG-1505, runReturnList)
  block["text"] = out.str();
  return block;
}

}  // namespace piet::handlers
