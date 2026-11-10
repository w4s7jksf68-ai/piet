// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro selftest: see macro_modules.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "../dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macros_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {


// ---------------------------------------------------------------------------
// try_capture_definition



// The corpus every EXPRLAYER drift guard below runs over. Deliberately wider
// than the benchmark: the whole point of a hand-written twin is that it must
// agree on the shapes the benchmark never produces.
const std::vector<std::string>& exprLayerCorpus() {
  static const std::vector<std::string> k = {
      // plain arithmetic (reSafeArith territory)
      "1+1", "0 + 1", "3.14*2", "2e3", "  12 / 4  ", "(1+2)^3", "1E-9", "-5", "",
      " ", "\t\n", "1\n+2",
      // extended (letters, quotes, comparisons)
      "sqrt(4)", "upper(\"abc\")", "cond(1>0,1,0)", "a<b", "x!=y", "mod(17,5)",
      "substr(\"benchmark1\",1,7)", "1,2", "'q'", "x_1", "log(2)+exp(-1)",
      // BUG-206: `%` is in the EXTENDED alphabet and NOT the basic one, so each
      // of these must answer ext=1 / basic=0 in both the scan and the regex.
      // `1%2` moved here FROM the outside-both group below when the character was
      // admitted — left in the corpus rather than deleted, because the boundary it
      // probes (a bare `%` as an operator) is exactly the one the widening had to
      // be measured against, and it is now an ext case rather than a rejected one.
      "1%2", "5 % 3", "string(42, \"%9.0g\")", "\"50%\"", "%", "strpos(\"50%\",\"%\")",
      "%9.0g", "upper(\"x%y\")",
      // characters OUTSIDE both whitelists
      "a[1]", "a{1}", "1;2", "x@y", "a\\b", "a#b", "a&b", "a|b", "a:b", "a$b",
      "`x'", "a?b", "1~2",
      // r()/e() shapes, both real and decoy
      "r(mean)", "`r(mean)'", "scalar(bp_s)", "subinstr(\"a\",\"b\",\"c\",1)",
      "e(N)", "true(1)", "size(x)", "myr(3)", "r (mean)", "_r(x)", "xe(1)",
      "e(b)", "e(V)", "1+r(N)", "sqrt(r(N))", "counter(3)",
      // scalar-command shapes
      "scalar x = 1", "scalar define x = 1", "scalar  define   x  =  1",
      "SCALAR X = 1", "Scalar define = 3", "scalar define = 3", "scalar x=1",
      "scalar x =", "scalar x =  ", "scalar x = \n", "scalar x", "scalar",
      "scalar drop x", "scalar drop x y", "scalar drop _all", "SCALAR DROP x",
      "scalar  drop   x  ", "scalar drop", "scalar drop ", "scalardrop x",
      "scalar\tdrop\tx", "scalar\ndrop\nx", "scalar list", "scalar dir",
      "scalar x = scalar(y) + 1", "scalar 1x = 2", "scalar _x = 2",
      "scalar x = a\nb", "scalar define define = 1",
      // Unicode names (lane M): UTF-8 names in every shape above. Each must
      // answer ext=1 / basic=0, and the scans must agree with their regex twins
      // (which carry the same `\x80-\xff` widening) name for name -- the
      // mixed-script `دخل_2020` is one name, never `دخل` + `_2020`.
      "ملاحظات[1]", "قيمة + 1", "upper(\"مرحبا\")", "دخل_2020 * 2", "x_ق", "ق1",
      "scalar(سعر)", "scalar(سعر) + 1", "scalar( سعر )", "مscalar(y)", "xscalar(y)",
      "scalar سعر = 5", "scalar define قيمة = 1", "SCALAR سعر=1", "scalar 1ق = 2",
      "scalar _ق = 2", "scalar drop سعر", "y ملاحظات", "yملاحظات", "ملاحظاتy", "y+سعر",
      "سعر + 1", "سعرx", "xسعر", "سعر(1)", "دخل_2020 + _2020", "_2020", "_2020y",
      "lnسعر", "log(سعر)",
  };
  return k;
}

}

namespace piet::eval {
using namespace macro_detail;


bool selftestSafeArithScanMatchesRegex(std::string* firstDisagreement) {
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    const bool reBasic = std::regex_match(c, m, reSafeArith());
    const bool reExt = std::regex_match(c, m, reSafeArithExt());
    if (reBasic != isSafeArith(c) || reExt != isSafeArithExt(c)) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=(" + (reBasic ? "1" : "0") +
                             (reExt ? "1" : "0") + ") scan=(" + (isSafeArith(c) ? "1" : "0") +
                             (isSafeArithExt(c) ? "1" : "0") + ")";
      }
      return false;
    }
  }
  return true;
}


bool selftestLocalRefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kExtra = {
      "`x'", "`acc' + `i'", "local acc = `acc' + `i'", "`_u'", "`1'", "`12'", "`x",
      "``x''", "`'", "``'", "`x`y''", "a`b'c", "`9x'", "`x9'", "`x-y'", "`x y'",
      "no ticks here", "`", "``", "`\"a b\"'", "`=2+3'", "`r(mean)'", "`s(k)'",
      "`x' `y'", "``", "`0'", "`x_1'", "`X'", "`_'",
      // Unicode names (lane M)
      "`م'", "`ملاحظات'", "`دخل_2020'", "`ق1'", "`1ق'", "`x م'", "``م''", "`م",
  };
  auto one = [&](const std::string& c) {
    std::smatch m;
    const bool re = std::regex_search(c, m, reLocalRef());
    const bool scan = hasLocalMacroRef(c);
    if (re != scan) {
      if (firstDisagreement) {
        *firstDisagreement = "line=[" + c + "] regex=" + (re ? "yes" : "no") + " scan=" +
                             (scan ? "yes" : "no");
      }
      return false;
    }
    return true;
  };
  for (const auto& c : exprLayerCorpus()) {
    if (!one(c)) return false;
  }
  for (const auto& c : kExtra) {
    if (!one(c)) return false;
  }
  return true;
}


bool selftestBarePrefilterIsStrict(std::string* firstDisagreement) {
  // A pre-filter is sound iff it never says "no" to text the regex matches.
  // (Saying "yes" where the regex does not match only costs a wasted scan.)
  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;
    if (std::regex_search(c, m, reRRef()) && !hasBareRParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reRRef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reERef()) && !hasBareEParen(c)) {
      if (firstDisagreement) *firstDisagreement = "reERef matched but guard said no: [" + c + "]";
      return false;
    }
    if (std::regex_search(c, m, reRRefDelim()) && c.find("`r(") == std::string::npos) {
      if (firstDisagreement) {
        *firstDisagreement = "reRRefDelim matched but guard said no: [" + c + "]";
      }
      return false;
    }
  }
  return true;
}


bool selftestScalarScansMatchRegex(std::string* firstDisagreement) {
  static const std::regex reScalarDropRx(R"(^scalar\s+drop\s+(.+)$)", std::regex::icase);
  // Unicode names (lane M): `\b` is ECMAScript's ASCII word boundary, under
  // which an Arabic byte is a NON-word character -- so the twin spells the
  // boundary as the identifier class itself (group 1 = the character before),
  // which is what the scan's isIdentChar test is.
  static const std::regex reScalarCallRx(
      R"((^|[^A-Za-z0-9_\x80-\xff])scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");

  // A session with a couple of scalars so the substitutions have something to
  // find, plus one name that also occurs as a function call (`scalar(...)`) so
  // the (?!\s*\() lookahead is exercised on both arms.
  EvalSession s;
  s.scalars.set("bp_s", "12.5");
  s.scalars.set("y", "7");
  s.scalars.set("log", "99");  // shadows a function name on purpose
  s.scalars.set("سعر", "3");  // Unicode names (lane M)
  s.scalars.set("_2020", "99");  // the ASCII tail of the mixed-script `دخل_2020`

  for (const auto& c : exprLayerCorpus()) {
    std::smatch m;

    // scalar( ... ) substitution
    const std::string viaRegex = regexSubFn(reScalarCallRx, c, [&s](const std::smatch& mm) {
      const std::string* v = s.scalars.find(mm.str(2));
      return mm.str(1) + (v ? valueRefText(*v) : mm.str(0).substr(mm.length(1)));
    });
    const std::string viaScan = substituteScalarCalls(c, s);
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar() subst [" + c + "] regex=[" + viaRegex + "] scan=[" +
                             viaScan + "]";
      }
      return false;
    }

    // bare-name substitution, once per stored scalar
    for (const auto& [sn, sv] : s.scalars.items()) {
      // Identifier-class boundaries for the same reason as reScalarCallRx
      // (every name in this session begins and ends with an identifier byte).
      std::regex reName("(^|[^A-Za-z0-9_\\x80-\\xff])" + regexEscape(sn) +
                        "(?![A-Za-z0-9_\\x80-\\xff])(?!\\s*\\()");
      std::string value = sv;
      const std::string bRegex =
          regexSubFn(reName, c, [&value](const std::smatch& mm) { return mm.str(1) + value; });
      const std::string bScan = substituteBareName(c, sn, sv);
      if (bRegex != bScan) {
        if (firstDisagreement) {
          *firstDisagreement = "bare-name subst name=" + sn + " [" + c + "] regex=[" + bRegex +
                               "] scan=[" + bScan + "]";
        }
        return false;
      }
    }

    // scalar drop / scalar define recognisers
    const bool dropRe = std::regex_match(c, m, reScalarDropRx);
    const std::string dropReRest = dropRe ? m.str(1) : std::string();
    std::string dropScanRest;
    const bool dropScan = matchScalarDrop(c, dropScanRest);
    if (dropRe != dropScan || (dropRe && dropReRest != dropScanRest)) {
      if (firstDisagreement) {
        *firstDisagreement = "scalar drop [" + c + "] regex=" +
                             (dropRe ? "MATCH(" + dropReRest + ")" : "no") + " scan=" +
                             (dropScan ? "MATCH(" + dropScanRest + ")" : "no");
      }
      return false;
    }

    const bool defRe = std::regex_match(c, m, reScalarDef());
    const std::string defReName = defRe ? m.str(1) : std::string();
    const std::string defReRest = defRe ? m.str(2) : std::string();
    std::string defScanName, defScanRest;
    const bool defScan = matchScalarDef(c, defScanName, defScanRest);
    if (defRe != defScan || (defRe && (defReName != defScanName || defReRest != defScanRest))) {
      if (firstDisagreement) {
        *firstDisagreement =
            "scalar def [" + c + "] regex=" +
            (defRe ? "MATCH(" + defReName + "|" + defReRest + ")" : "no") + " scan=" +
            (defScan ? "MATCH(" + defScanName + "|" + defScanRest + ")" : "no");
      }
      return false;
    }
  }
  return true;
}


bool selftestExpansionSemantics(std::string* firstFailure) {
  // The S32-class parity corpus. Each leg is (setup-free) input -> expected
  // outcome of expandMacros on a session seeded below. These assert TODAY's
  // behavior, whatever it is — the point is that no performance change may move
  // any of them, not that any particular one is Stata-perfect.
  EvalSession s;
  s.locals.set("x", "5");
  s.locals.set("y", "mpg");
  s.locals.set("inner", "x");
  s.locals.set("empty", "");
  s.locals.set("withdollar", "$G");
  s.locals.set("1", "first");
  s.globals.set("G", "gval");
  s.rValues.set("mean", 3.5);
  s.rStrings.set("lbl", "hello");
  s.eScalars.set("N", 42.0);

  struct Leg {
    const char* in;
    bool wantError;
    const char* want;  // meaningful only when !wantError
  };
  static const Leg kLegs[] = {
      // plain reference, and a line with none at all
      {"display `x'", false, "display 5"},
      {"summarize mpg", false, "summarize mpg"},
      // an EMPTY macro substitutes nothing (not a space, not a quote)
      {"display [`empty']", false, "display []"},
      // nested: `\`inner'' resolves through two passes
      {"display ``inner''", false, "display 5"},
      // a macro whose VALUE carries a $global is expanded on the next pass
      {"display `withdollar'", false, "display gval"},
      // positional macro
      {"display `1'", false, "display first"},
      // `=expr' inline arithmetic, including one that references a macro
      {"display `=2+3'", false, "display 5"},
      {"display `=sqrt(16)'", false, "display 4"},
      // global forms, braced and bare
      {"display $G", false, "display gval"},
      {"display ${G}", false, "display gval"},
      // r() numeric and r() string (the string store wins)
      {"display r(mean)", false, "display 3.5"},
      {"display `r(lbl)'", false, "display hello"},
      // e() scalar out of the posted store
      {"display e(N)", false, "display 42.0"},
      // BUG-294: an UNDEFINED macro expands to the EMPTY STRING at rc 0 —
      // Stata 19.5 IC, measured (`display "[`nosuchlocal']"` -> `[]`, _rc 0;
      // the global forms identically). These two rows used to pin the opposite
      // (`true` = error), which is what stopped a do-file dead one line after a
      // deliberately-captured failed definition.
      {"display `nosuch'", false, "display "},
      {"display $NOSUCH", false, "display "},
      // a bare backtick that is not a reference is copied through
      {"display `", false, "display `"},
      {"display a`b", false, "display a`b"},
      // compound double-quotes are unwrapped to plain quotes
      {"display `\"a b\"'", false, "display \"a b\""},
      // an identifier ending in r/e followed by '(' is NOT a class reference
      {"display subinstr(\"a\",\"b\",\"c\",1)", false, "display subinstr(\"a\",\"b\",\"c\",1)"},
      {"display size(3)", false, "display size(3)"},
      {"scalar q = scalar(bp_s) + 1", false, "scalar q = scalar(bp_s) + 1"},
  };

  for (const auto& leg : kLegs) {
    ExpandResult r = expandMacros(leg.in, s);
    if (leg.wantError) {
      if (!r.isError) {
        if (firstFailure) *firstFailure = std::string("[") + leg.in + "] expected error, got [" +
                                          r.line + "]";
        return false;
      }
      continue;
    }
    if (r.isError || r.line != leg.want) {
      if (firstFailure) {
        *firstFailure = std::string("[") + leg.in + "] want [" + leg.want + "] got " +
                        (r.isError ? std::string("ERROR") : "[" + r.line + "]");
      }
      return false;
    }
  }
  return true;
}


bool selftestMacroDefScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      // ordinary forms
      "local q 5", "local acc = `acc' + `i'", "local x", "local x=3", "local x  =  3",
      "local mylist \"alpha beta\"", "local _u 1", "local x9 hello", "local ++ctr",
      "global G 7", "global G = 2+3", "global _g x", "global g",
      // case variants (Stata accepts them)
      "LOCAL Q 5", "Local q 5", "GLOBAL G 7", "gLoBaL g 1",
      // whitespace shapes
      "local\tq\t5", "local\t \tq 5", "local   q   5   ", "local\nq 5", "global\vg 1",
      // non-matches
      "localq 5", "local", "local ", "local  ", "local 1x 3", "local -x 3", "local `k' 5",
      // BUG-923: positional locals match `local`, never `global`
      "local 1 alpha", "local 12 \"b c\"", "local 1=3", "local 12abc 1", "local 1_ 2", "global 1 a",
      "globalg 1", "global", "global 9z 1", "scalar a = 1", "display 1", "",
      // the newline boundary: `.` cannot cross one, so the tail decides
      "local q a\nb", "global g a\nb", "local q 5\n",
      // name/rest boundaries
      "local q:word count x", "local q:", "local q,", "local q)", "global g:",
      // Unicode names (lane M): local/global names are UTF-8 too
      "local م \"abc\"", "global ج \"gval\"", "local دخل_2020 = 5", "global ق1 x",
      "local 1ق 3", "local مx=1", "LOCAL م 1", "global ٥ 3", "global €x 1", "local €x 3",
  };
  for (const auto& c : kCases) {
    for (int which = 0; which < 2; ++which) {
      const char* kw = which == 0 ? "local" : "global";
      const size_t kwLen = which == 0 ? 5 : 6;
      const std::regex& re = which == 0 ? reLocalDef() : reGlobalDef();
      std::smatch rm;
      // The `^global` regex cannot say "a Unicode LETTER"; matchMacroDef adds
      // globalNameStartOk on top of it, so the twin applies the same filter.
      const bool reOk = std::regex_match(c, rm, re) && (which == 0 || globalNameStartOk(rm.str(1)));
      std::string sName, sRest;
      const bool scanOk = matchMacroDef(c, kw, kwLen, sName, sRest);
      bool agree = (reOk == scanOk);
      if (agree && reOk) agree = (rm.str(1) == sName && rm.str(2) == sRest);
      if (!agree) {
        if (firstDisagreement) {
          *firstDisagreement = std::string(kw) + " | line=[" + c + "] regex=" +
                               (reOk ? "MATCH(" + rm.str(1) + "|" + rm.str(2) + ")" : "no") +
                               " scan=" + (scanOk ? "MATCH(" + sName + "|" + sRest + ")" : "no");
        }
        return false;
      }
    }
  }
  return true;
}

}
