// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// estimation_glm.cpp -- X2-GLM: `glm` and `margins` after it, fitted in-process.
//
// The estimator is the engine's own TU (src/estimators/glm.cpp, linked into
// piet_core the way columnar.cpp is); this file is the PIET half: the option and
// family/link grammar, the design matrix (plain, c., i. and i.() terms), the
// sample, the e() results, the iteration log and header, and the delta-method
// `margins, dydx() [post]` the engine's margins cannot answer for a glm.
//
// Oracle: tests/eval/golden/aer_w2/w2_glm_lifecycle.{do,log} (21 arms) and the
// first capture tests/eval/golden/aer_estim2_glm_oracle.log (gaussian/identity,
// offset/exposure). The method, and why a non-converged `iterate(k)` answer
// matches Stata bit-for-bit only if the optimizer's path is Stata's, is in
// src/estimators/glm.h. The family()/link() grammar, the domain checks and their
// exit codes follow Stata's documented behaviour ([R] glm, help glm) and black-box
// StataBE runs.
//
// COVERED, and nothing else: gaussian/identity, binomial (Bernoulli, m = 1)
// with logit or probit, poisson/log, gamma/log and nbinomial #k (alpha FIXED,
// default 1) with log; vce(oim) (the default) and vce(cluster var); aweights.
// Every other family, link, vce and weight glm accepts is refused BY NAME
// (refused:glm_*), never approximated -- a refusal is honest, a GLM fitted under
// a different family or VCE is the silent wrong answer.
#include "core/distributions.h"  // z_critical: glm level(#)
#include "piet/handlers/estimation.h"
#include "piet/handlers/postestimation.h"
#include "piet/handlers/vce_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "estimators/glm.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "estimator_command_support.h"  // e3::installEsampleMask
#include "statistics_command_support.h"  // estimWriteColumn: score()

namespace piet::handlers {
// engine/inrange_filter.cpp (declared at each caller, as linear_estimation.cpp does)
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
    const std::optional<std::string>&, const std::optional<std::string>&, std::string*);
}  // namespace piet::handlers

namespace piet::handlers {

namespace est_detail {
// Upper tail of chi2(df): the regularized upper incomplete gamma Q(df/2, x/2)
// (series below a + 1, continued fraction above).
double chi2UpperTailM4(double df, double x) {
    if (!(x > 0)) return 1.0;
    const double a = df / 2.0, z = x / 2.0, gln = std::lgamma(a);
    if (z < a + 1.0) {
        double ap = a, sum = 1.0 / a, del = sum;
        for (int n = 0; n < 1000; ++n) {
            ap += 1.0;
            del *= z / ap;
            sum += del;
            if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
        }
        return 1.0 - sum * std::exp(-z + a * std::log(z) - gln);
    }
    double b = z + 1.0 - a, c = 1.0 / 1e-300, dd = 1.0 / b, h = dd;
    for (int i = 1; i < 1000; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        dd = an * dd + b;
        if (std::fabs(dd) < 1e-300) dd = 1e-300;
        c = b + an / c;
        if (std::fabs(c) < 1e-300) c = 1e-300;
        dd = 1.0 / dd;
        const double del = dd * c;
        h *= del;
        if (std::fabs(del - 1.0) < 1e-16) break;
    }
    return std::exp(-z + a * std::log(z) - gln) * h;
}
}  // namespace est_detail


using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// Option vocabulary. `known` is every key this handler reads; `unimplemented`
// is the rest of glm's documented options (plus the ml maximize options it
// accepts that the oracle does not cover), so a real option reads as
// "valid Stata, not implemented" rather than as a typo. `fam(nb)` is the
// corpus spelling: `fam` resolves to `family` by the unique-prefix rule.
const eval::OptVocab& kVocabGlm = eval::registerOptVocab(
    {"glm"},
    {{"family", "link", "vce", "iterate", "tolerance", "ltolerance", "nrtolerance", "offset",
      "exposure", "nolog", "log", "trace", "showstep", "showtolerance", "cluster", "eform", "level", "robust",
      "from", "lnoffset", "noconstant", "asis", "ml", "score", "fisher", "nodots", "scale", "mu", "init",
      "vfactor", "difficult", "technique", "gradient", "hessian", "noheader"},
     {"search", "irls", "notable", "nodisplay", "disp",
      "constraints", "collinear", "coeflegend", "nocnsreport"},
     {}});

// margins after glm: the two options the covered form uses; the rest of
// margins' syntax is real Stata this path does not implement.
// at()/atmeans and a marginlist are answered by the margins grid (runMarginsGrid).
const std::unordered_set<std::string> kGlmMarginsKnown = {"dydx", "post", "at", "atmeans"};
const std::unordered_set<std::string> kGlmMarginsUnimplemented = {
    "over",       "within",     "predict",  "expression", "eyex",
    "dyex",   "eydx",      "vce",        "level",      "noestimcheck", "asobserved",
    "contrast", "pwcompare", "grand",    "nose",       "coeflegend", "noatlegend",
    "asbalanced", "emptycells", "noweights", "continuous", "nochainrule", "mcompare"};

// The e() entries glm posts outside its estimation block (see handlers.h).
const char* const kGlmEMatrices[] = {"b", "V", "ilog", "gradient", "Cns", "V_modelbased"};
const char* const kGlmEStrings[] = {"varfunct", "linkt", "opt", "technique", "a"};

ojson errRc(const std::string& message, int rc) {
    return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(rc) + ")"));
}
ojson syntaxErr(const std::string& message) {
    return ojson(eval::engineError(message, "syntax_error"));
}
ojson refuse(const std::string& cause) { return ojson(eval::refusedCommand("glm", cause)); }

ojson displayBlock(const std::string& text) {
    ojson b;
    b["type"] = "display";
    b["text"] = text;
    return b;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}
std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// Stata `%w.0g`, right-justified into w.
std::string gfmt(double v, int w) {
    std::string s = fmt::general(v, w, 0);
    if (static_cast<int>(s.size()) < w) s.insert(0, static_cast<size_t>(w) - s.size(), ' ');
    return s;
}
// Stata `%10.0gc` for a count: an integer with thousands separators.
std::string countFmt(double v) {
    std::string d = std::to_string(static_cast<long long>(std::llround(v)));
    std::string out;
    int c = 0;
    for (auto it = d.rbegin(); it != d.rend(); ++it) {
        if (c > 0 && c % 3 == 0 && *it != '-') out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++c;
    }
    if (out.size() < 10) out.insert(0, 10 - out.size(), ' ');
    return out;
}
// Pad `s` so the next character lands in 1-based column `col`.
void toCol(std::string& s, size_t col) {
    if (s.size() + 1 < col) s.append(col - 1 - s.size(), ' ');
}

// b' inv(V) b for a symmetric positive definite q x q V (row-major), by Cholesky;
// nullopt when V is not positive definite.
std::optional<double> waldQuadForm(const std::vector<double>& V, const std::vector<double>& b, int q) {
    const size_t n = static_cast<size_t>(q);
    std::vector<double> L(n * n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            double sum = V[i * n + j];
            for (size_t k = 0; k < j; ++k) sum -= L[i * n + k] * L[j * n + k];
            if (i == j) {
                if (!(sum > 0.0)) return std::nullopt;
                L[i * n + i] = std::sqrt(sum);
            } else {
                L[i * n + j] = sum / L[j * n + j];
            }
        }
    }
    double out = 0.0;
    std::vector<double> y(n, 0.0);
    for (size_t i = 0; i < n; ++i) {  // L y = b; b' inv(V) b = y'y
        double sum = b[i];
        for (size_t k = 0; k < i; ++k) sum -= L[i * n + k] * y[k];
        y[i] = sum / L[i * n + i];
        out += y[i] * y[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// The model specification, kept on the estimation block (key "glm_model") so
// `margins` rebuilds exactly the fitted design from the fit-time frame.

struct Term {
    bool factor = false;
    std::string var;          // resolved column name (a main effect)
    std::vector<Term> atoms;  // an interaction (`i.t#c.d`): its parts, two or more
};

struct FamilySpec {
    cecon::GlmFamily fam = cecon::GlmFamily::Gaussian;
    double alpha = 1.0;
    std::string alphaText = "1";  // e(a): the nbinomial argument as typed
    std::string varfunct, varfuncf;
    std::string varfunc;  // e(varfunc): glm's variance-function program (glim_v#)
    // parseFamily only rejects what is wrong with the family's own arguments. A
    // valid family this build does not fit waits here until parseLink has judged
    // link(): an error in link() outranks a refusal of the family.
    std::string heldRefusal;
    bool wordUnknown = false;      // the first token named no family at all
    const char* tag = "gaussian";  // the family's word in glm_family_link_<tag>_<link>
    // family(binomial #|varname): the denominator, as typed (e(m)), and either a
    // positive constant or a variable resolved once the data is open.
    std::string mText = "1";
    double mConst = 1.0;
    std::string mVar;
};
struct LinkSpec {
    cecon::GlmLinkFn link = cecon::GlmLinkFn::Identity;
    std::string linkt, linkf;
    std::string link_prog;  // e(link): glm's link program (glim_l#)
    std::string power;      // e(power): the link's power, "." when it has none
    double p = 1.0;         // the power of link(power #) / link(opower #)
};

// Resolve a variable name: exact match, else Stata's unique-abbreviation rule.
bool resolveVar(const Datastore& ds, const std::string& raw, std::string& out, ojson& err) {
    if (ds.has(raw)) {
        out = raw;
        return true;
    }
    std::string hit;
    int hits = 0;
    for (const auto& n : ds.names()) {
        if (n.size() > raw.size() && n.compare(0, raw.size(), raw) == 0) {
            hit = n;
            ++hits;
        }
    }
    if (hits == 1) {
        out = hit;
        return true;
    }
    err = errRc(hits > 1 ? raw + " ambiguous abbreviation" : "variable " + raw + " not found", 111);
    return false;
}

// Split the (whitespace-tokenized) varlist back into terms, keeping `i.(a b)`
// whole -- the tokenizer splits it at the space.
std::vector<std::string> rawTerms(const std::vector<std::string>& varlist) {
    std::string joined;
    for (const auto& t : varlist) joined += (joined.empty() ? "" : " ") + t;
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : joined) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if ((c == ' ' || c == '\t') && depth == 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// An interaction ([U] 11.4.3): `a#b` is one term, `a##b##c` the full
// factorial. Inside an interaction a bare name is a factor (`i.`), `c.x` is
// continuous. Stata's order for A##B##C is (A##B)##C = A, B, A#B, C, A#C, B#C,
// A#B#C with a repeated term dropped: `tt##c.d##c.d` (069
// _2_3_effect_on_productivity.do:472) is tt, d, tt#d, d#d, tt#d#d.
bool parseInteraction(const Datastore& ds, const std::string& raw, std::vector<Term>& out,
                      ojson& err) {
    const bool full = raw.find("##") != std::string::npos;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= raw.size()) {
        const size_t q = raw.find('#', p);
        const std::string part = raw.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (q == std::string::npos) {
            parts.push_back(part);
            break;
        }
        parts.push_back(part);
        p = q + 1;
        if (full) {
            if (q + 1 >= raw.size() || raw[q + 1] != '#') {
                err = refuse("glm_factor_interaction_mixed");  // `a#b##c`: not covered
                return false;
            }
            p = q + 2;
        }
    }
    std::vector<Term> atoms;
    for (const auto& part : parts) {
        if (part.empty() || part.find_first_of("*?~-()") != std::string::npos) {
            err = part.empty() ? syntaxErr("invalid syntax") : refuse("glm_factor_interaction");
            return false;
        }
        const std::string low = lower(part);
        Term t;
        t.factor = true;
        std::string name = part;
        if (low.rfind("c.", 0) == 0) {
            t.factor = false;
            name = part.substr(2);
        } else if (low.rfind("i.", 0) == 0) {
            name = part.substr(2);
        } else if (part.find('.') != std::string::npos) {
            err = refuse("glm_varlist_operator");
            return false;
        }
        if (!resolveVar(ds, name, t.var, err)) return false;
        atoms.push_back(t);
    }
    std::vector<std::vector<Term>> terms;
    if (!full) {
        terms.push_back(atoms);
    } else {
        for (const auto& a : atoms) {
            const size_t n0 = terms.size();
            terms.push_back({a});
            for (size_t i = 0; i < n0; ++i) {
                std::vector<Term> c = terms[i];
                c.push_back(a);
                terms.push_back(std::move(c));
            }
        }
    }
    std::set<std::vector<std::string>> seen;
    for (const auto& tv : terms) {
        std::vector<std::string> key;
        for (const auto& a : tv) key.push_back((a.factor ? "i." : "c.") + a.var);
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second) continue;
        if (tv.size() == 1) {
            out.push_back(tv[0]);
        } else {
            Term t;
            t.atoms = tv;
            out.push_back(t);
        }
    }
    return true;
}

// buildDesign omits every interaction cell with a factor at its base level,
// which is Stata's design only when the interaction's lower-order terms are in
// the model, as `##` puts them. A `#` term with a factor whose sub-terms (the
// term with one part dropped) are not all present -- `i.t#c.d` alone estimates
// 0.t#c.d and 1.t#c.d -- is refused by name: no oracle arm pins its cells.
bool hierarchicalInteractions(const std::vector<Term>& terms, ojson& err) {
    auto keyOf = [](const std::vector<Term>& parts) {
        std::vector<std::string> k;
        for (const auto& a : parts) k.push_back((a.factor ? "i." : "c.") + a.var);
        std::sort(k.begin(), k.end());
        return k;
    };
    std::set<std::vector<std::string>> present;
    for (const auto& t : terms) present.insert(t.atoms.empty() ? keyOf({t}) : keyOf(t.atoms));
    for (const auto& t : terms) {
        bool anyFactor = false;
        for (const auto& a : t.atoms) anyFactor = anyFactor || a.factor;
        if (!anyFactor) continue;
        for (size_t drop = 0; drop < t.atoms.size(); ++drop) {
            std::vector<Term> sub;
            for (size_t a = 0; a < t.atoms.size(); ++a)
                if (a != drop) sub.push_back(t.atoms[a]);
            if (!present.count(keyOf(sub))) {
                err = refuse("glm_factor_interaction");
                return false;
            }
        }
    }
    return true;
}

// One raw term -> terms, or a refusal. Covered: `x`, `c.x`, `i.x`, `i.(x y)`, and
// the interactions parseInteraction reads.
bool parseTerm(const Datastore& ds, const std::string& raw, std::vector<Term>& out, ojson& err) {
    if (raw.find('#') != std::string::npos) return parseInteraction(ds, raw, out, err);
    if (raw.find_first_of("*?~-") != std::string::npos) {
        err = refuse("glm_varlist_wildcard");
        return false;
    }
    const std::string low = lower(raw);
    const size_t dot = raw.find('.');
    if (dot == std::string::npos) {
        Term t;
        if (!resolveVar(ds, raw, t.var, err)) return false;
        out.push_back(t);
        return true;
    }
    const std::string op = low.substr(0, dot);
    const std::string rest = raw.substr(dot + 1);
    if (op == "c") {
        Term t;
        if (!resolveVar(ds, rest, t.var, err)) return false;
        out.push_back(t);
        return true;
    }
    if (op == "i") {
        std::vector<std::string> names;
        if (!rest.empty() && rest.front() == '(') {
            if (rest.back() != ')') {
                err = syntaxErr("invalid syntax");
                return false;
            }
            names = splitWs(rest.substr(1, rest.size() - 2));
        } else {
            names.push_back(rest);
        }
        if (names.empty()) {
            err = syntaxErr("invalid syntax");
            return false;
        }
        for (const auto& n : names) {
            Term t;
            t.factor = true;
            if (!resolveVar(ds, n, t.var, err)) return false;
            out.push_back(t);
        }
        return true;
    }
    // ib#./ibn./b#./o. and the time-series operators: valid Stata, not covered.
    err = refuse("glm_varlist_operator");
    return false;
}

// ---------------------------------------------------------------------------
// The family() and link() words, as measured on StataBE (every prefix of every
// word was run). Each option's text is whitespace-split; the first token is the
// word, and a word matches a spelling when it is that spelling cut down to no fewer
// than `shortest` characters.

struct Spelling {
    const char* word;
    size_t shortest;
    int meaning;
};

template <size_t N>
int lookUpSpelling(const Spelling (&table)[N], const std::string& typed) {
    const std::string w = lower(typed);
    for (const Spelling& sp : table) {
        const std::string full(sp.word);
        if (w.size() >= sp.shortest && w.size() <= full.size() && full.compare(0, w.size(), w) == 0)
            return sp.meaning;
    }
    return -1;
}

enum FamilyWord { kFamGaussian, kFamInverse, kFamBinomial, kFamPoisson, kFamNegBin, kFamGamma };
const Spelling kFamilySpellings[] = {
    {"gaussian", 3, kFamGaussian},  {"normal", 1, kFamGaussian}, {"igaussian", 2, kFamInverse},
    {"inormal", 2, kFamInverse},    {"ivg", 3, kFamInverse},     {"binomial", 1, kFamBinomial},
    {"bernoulli", 1, kFamBinomial}, {"poisson", 1, kFamPoisson}, {"nbinomial", 2, kFamNegBin},
    {"gamma", 3, kFamGamma},
};

enum LinkWord {
    kLnkIdentity, kLnkLog, kLnkLogit, kLnkProbit, kLnkCloglog, kLnkPower, kLnkOpower,
    kLnkNegBin, kLnkLoglog, kLnkLogc, kLnkReciprocal, kLnkPowerM2
};
// `log` sits above `logit`: the three letters are the log link, while one, two or
// four letters of `logit` mean logit.
const Spelling kLinkSpellings[] = {
    {"identity", 1, kLnkIdentity}, {"log", 3, kLnkLog},          {"logit", 1, kLnkLogit},
    {"probit", 1, kLnkProbit},     {"cloglog", 1, kLnkCloglog},  {"power", 3, kLnkPower},
    {"opower", 3, kLnkOpower},     {"nbinomial", 2, kLnkNegBin}, {"loglog", 4, kLnkLoglog},
    {"logc", 4, kLnkLogc},         {"reciprocal", 1, kLnkReciprocal},
};

// A Stata numeric literal: optional sign, digits with an optional point (a leading
// point is fine), optional exponent. `inf`, `nan` and hexadecimal are not numbers.
bool readNumericToken(const std::string& tok, double& value) {
    size_t at = 0;
    const auto digitRun = [&tok, &at]() {
        const size_t from = at;
        while (at < tok.size() && std::isdigit(static_cast<unsigned char>(tok[at]))) ++at;
        return at - from;
    };
    if (at < tok.size() && (tok[at] == '+' || tok[at] == '-')) ++at;
    size_t mantissaDigits = digitRun();
    if (at < tok.size() && tok[at] == '.') {
        ++at;
        mantissaDigits += digitRun();
    }
    if (mantissaDigits == 0) return false;
    if (at < tok.size() && (tok[at] == 'e' || tok[at] == 'E')) {
        ++at;
        if (at < tok.size() && (tok[at] == '+' || tok[at] == '-')) ++at;
        if (digitRun() == 0) return false;
    }
    if (at != tok.size()) return false;
    value = std::strtod(tok.c_str(), nullptr);
    return std::isfinite(value);
}

// e(varfuncf) of a negative binomial shows alpha to 4 decimal places in %g style,
// not the typed text (`0.50` shows as `.5`, `1e-5` as `0`).
std::string alphaInVarianceText(double alpha) {
    const double shown = alpha < 1e10 ? std::floor(alpha * 10000.0 + 0.5) / 10000.0 : alpha;
    return trim(gfmt(shown, 9));
}

bool parseFamily(const std::string& rawIn, FamilySpec& fs, ojson& err) {
    const std::vector<std::string> tokens = splitWs(rawIn);
    const int word = tokens.empty() ? kFamGaussian : lookUpSpelling(kFamilySpellings, tokens[0]);
    if (word < 0) {
        fs.wordUnknown = true;
        fs.heldRefusal = "glm_family_unrecognized";
        return true;
    }
    const auto tooMany = [&tokens, &rawIn, &err]() {
        if (tokens.size() <= 2) return false;
        err = syntaxErr("family(" + trim(rawIn) + ") invalid");
        return true;
    };
    const auto notPositive = [&err](const std::string& canonical, const std::string& arg) {
        err = syntaxErr(arg + " in family(" + canonical + " " + arg + ") invalid");
        return false;
    };
    double argValue = 0.0;
    switch (word) {
        case kFamGaussian:
            fs.fam = cecon::GlmFamily::Gaussian;
            fs.varfunct = "Gaussian";
            fs.varfunc = "glim_v1";
            fs.varfuncf = "1";
            fs.tag = "gaussian";
            return true;
        case kFamInverse:
            fs.fam = cecon::GlmFamily::InverseGaussian;
            fs.varfunct = "Inverse Gaussian";
            fs.varfunc = "glim_v5";
            fs.varfuncf = "u^3";
            fs.tag = "igaussian";
            return true;
        case kFamPoisson:
            fs.fam = cecon::GlmFamily::Poisson;
            fs.varfunct = "Poisson";
            fs.varfunc = "glim_v3";
            fs.varfuncf = "u";
            fs.tag = "poisson";
            return true;
        case kFamGamma:
            fs.fam = cecon::GlmFamily::Gamma;
            fs.varfunct = "Gamma";
            fs.varfunc = "glim_v4";
            fs.varfuncf = "u^2";
            fs.tag = "gamma";
            return true;
        case kFamBinomial: {
            if (tooMany()) return false;
            fs.fam = cecon::GlmFamily::Bernoulli;
            fs.varfunct = "Bernoulli";
            fs.varfunc = "glim_v2";
            fs.varfuncf = "u*(1-u)";
            fs.tag = "bernoulli";
            if (tokens.size() < 2 || tokens[1] == "1") return true;
            if (readNumericToken(tokens[1], argValue)) {
                if (argValue < 1.0) return notPositive("binomial", tokens[1]);  // glm: "# < 1 invalid"
                fs.mConst = argValue;
            } else {
                fs.mVar = tokens[1];  // a variable name, resolved against the data
            }
            // A denominator other than 1 is the binomial (glim_v2's title and
            // variance u*(1-u/m), m as typed: bank 435c8f90 `family(binomial fatal)`).
            fs.mText = tokens[1];
            fs.varfunct = "Binomial";
            fs.varfuncf = "u*(1-u/" + tokens[1] + ")";
            fs.tag = "binomial";
            return true;
        }
        default: {  // kFamNegBin
            if (tooMany()) return false;
            fs.fam = cecon::GlmFamily::NegBinomial;
            fs.varfunct = "Neg. Binomial";
            fs.varfunc = "glim_v6";
            fs.tag = "nbinomial";
            if (tokens.size() == 2) {
                const std::string& arg = tokens[1];
                if (arg == "ml") {  // the keyword is case-sensitive; `ML` is r(7) below
                    fs.heldRefusal = "glm_family_nbinomial_ml";
                    return true;
                }
                if (!readNumericToken(arg, argValue)) {
                    err = errRc("'" + arg + "' found where number expected", 7);
                    return false;
                }
                if (argValue <= 0.0) return notPositive("nbinomial", arg);
                fs.alpha = argValue;
                fs.alphaText = arg;
            }
            fs.varfuncf = "u+(" + alphaInVarianceText(fs.alpha) + ")u^2";
            return true;
        }
    }
}

// power and opower rewrite these numeric values to another link; every other value
// stays in the (refused) power family.
// (glm's MapLink: power 0 is the log link, -1 the reciprocal, -2 glim_l10;
// power 1 stays the power link with p = 1.)
int linkAfterPowerArgument(int word, double power) {
    if (word == kLnkPower) {
        if (power == 0.0) return kLnkLog;
        if (power == -1.0) return kLnkReciprocal;
        if (power == -2.0) return kLnkPowerM2;
        return kLnkPower;
    }
    return power == 0.0 ? kLnkLogit : kLnkOpower;
}

int defaultLinkOf(cecon::GlmFamily family) {
    switch (family) {
        case cecon::GlmFamily::Gaussian: return kLnkIdentity;
        case cecon::GlmFamily::Bernoulli: return kLnkLogit;
        case cecon::GlmFamily::Gamma: return kLnkReciprocal;
        case cecon::GlmFamily::InverseGaussian: return kLnkPowerM2;
        default: return kLnkLog;  // poisson, nbinomial
    }
}

// Second half of the grammar, and the arbiter between the two options: errors in
// link() come first, then the incomplete-specification error, and only then the
// refusals, family before link before family-by-link.
bool parseLink(const std::string& rawIn, const FamilySpec& fs, LinkSpec& ls, ojson& err) {
    const std::vector<std::string> tokens = splitWs(rawIn);
    int word = -1;
    bool wordUnknown = false;
    if (tokens.empty()) {
        word = defaultLinkOf(fs.fam);
    } else {
        word = lookUpSpelling(kLinkSpellings, tokens[0]);
        wordUnknown = word < 0;
    }
    double power = 0.0;
    if (word == kLnkPower || word == kLnkOpower) {
        if (tokens.size() > 2) {
            err = syntaxErr("link(" + trim(rawIn) + ") invalid");
            return false;
        }
        if (tokens.size() < 2 || !readNumericToken(tokens[1], power)) {
            err = syntaxErr(std::string("invalid # in link(") +
                            (word == kLnkPower ? "power" : "opower") + " #)");
            return false;
        }
        word = linkAfterPowerArgument(word, power);
    }
    if (fs.wordUnknown && tokens.empty()) {
        err = syntaxErr("incomplete specification of family() and link()");
        return false;
    }
    if (!fs.heldRefusal.empty()) {
        err = refuse(fs.heldRefusal);
        return false;
    }
    if (wordUnknown) {
        err = refuse("glm_link_unrecognized");
        return false;
    }
    // The link programs' titles and formulas (glim_l01..glim_l12), with the
    // binomial denominator written in when it is not 1 (bank 435c8f90
    // `family(binomial fatal)`: e(linkf) ln(u/(fatal-u))); e(power) is MapLink's.
    const bool hasM = fs.mText != "1";
    const std::string m = fs.mText;
    // power / opower: the argument as typed (MapLink returns the token itself,
    // and glim_l11 / glim_l12 write it into the formula: link(power .5) is u^(.5))
    const std::string powTyped = tokens.size() >= 2 ? tokens[1] : std::string();
    switch (word) {
        case kLnkIdentity:
            ls.link = cecon::GlmLinkFn::Identity;
            ls.linkt = "Identity";
            ls.link_prog = "glim_l01";
            ls.power = "1";
            ls.linkf = hasM ? "u/" + m : "u";
            break;
        case kLnkLog:
            ls.link = cecon::GlmLinkFn::Log;
            ls.linkt = "Log";
            ls.link_prog = "glim_l03";
            ls.power = "0";
            ls.linkf = hasM ? "ln(u/" + m + ")" : "ln(u)";
            break;
        case kLnkLogit:
            ls.link = cecon::GlmLinkFn::Logit;
            ls.linkt = "Logit";
            ls.link_prog = "glim_l02";
            ls.power = "0";
            ls.linkf = "ln(u/(" + m + "-u))";
            break;
        case kLnkProbit:
            ls.link = cecon::GlmLinkFn::Probit;
            ls.linkt = "Probit";
            ls.link_prog = "glim_l08";
            ls.power = ".";
            ls.linkf = hasM ? "invnorm(u/" + m + ")" : "invnorm(u)";
            break;
        case kLnkCloglog:
            ls.link = cecon::GlmLinkFn::Cloglog;
            ls.linkt = "Complementary log\xE2\x80\x93log";  // with an en dash (bank 3034ee28)
            ls.link_prog = "glim_l07";
            ls.power = ".";
            ls.linkf = hasM ? "ln(-ln(1-u/" + m + "))" : "ln(-ln(1-u))";
            break;
        case kLnkLoglog:
            ls.link = cecon::GlmLinkFn::Loglog;
            ls.linkt = "Log-log";
            ls.link_prog = "glim_l06";
            ls.power = ".";
            ls.linkf = hasM ? "-ln(-ln(u/" + m + "))" : "-ln(-ln(u))";
            break;
        case kLnkLogc:
            ls.link = cecon::GlmLinkFn::Logc;
            ls.linkt = "Log complement";
            ls.link_prog = "glim_l05";
            ls.power = ".";
            ls.linkf = hasM ? "ln(1-u/" + m + ")" : "ln(1-u)";
            break;
        case kLnkReciprocal:
            ls.link = cecon::GlmLinkFn::Reciprocal;
            ls.linkt = "Reciprocal";
            ls.link_prog = "glim_l09";
            ls.power = "-1";
            ls.linkf = hasM ? m + "/u" : "1/u";
            break;
        case kLnkPowerM2:
            ls.link = cecon::GlmLinkFn::PowerM2;
            ls.linkt = "Power(-2)";
            ls.link_prog = "glim_l10";
            ls.power = "-2";
            ls.linkf = "1/(u^2)";
            break;
        case kLnkPower:
            ls.link = cecon::GlmLinkFn::Power;
            ls.linkt = "Power";
            ls.link_prog = "glim_l11";
            ls.p = power;
            ls.power = powTyped;
            ls.linkf = hasM ? "(u/" + m + ")^(" + ls.power + ")" : "u^(" + ls.power + ")";
            break;
        case kLnkOpower:
            ls.link = cecon::GlmLinkFn::OPower;
            ls.linkt = "Odds power";
            ls.link_prog = "glim_l12";
            ls.p = power;
            ls.power = powTyped;
            ls.linkf = hasM ? "((u/(" + m + "-u))^(" + ls.power + ")-1)/(" + ls.power + ")"
                            : "((u/(1-u))^(" + ls.power + ")-1)/(" + ls.power + ")";
            break;
        default:  // kLnkNegBin
            ls.link = cecon::GlmLinkFn::NegBin;
            ls.linkt = "Neg. Binomial";
            ls.link_prog = "glim_l04";
            ls.power = ".";
            ls.linkf = "ln(u/(u+(1/" + fs.alphaText + ")))";
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Design construction, shared by the fit and by `margins`.

struct Design {
    int64_t n = 0;
    std::vector<double> X;          // column-major n*k (estimable columns only)
    int k = 0;
    std::vector<double> y, w, off;  // w empty = unweighted; off empty = none
    std::vector<int64_t> cl;        // empty = no cluster
    int64_t nClust = 0;
    std::vector<int64_t> rows;      // the frame rows of the sample, in order
    double sumW = 0.0;
    std::vector<double> m;          // binomial denominators; empty = 1
    // e(b) layout: every posted column, base levels included.
    std::vector<std::string> names;  // posted names ("x1", "1b.g", "2.g", "_cons")
    std::vector<int> estIndex;       // posted column -> estimable column, -1 = base
    // For margins: which posted columns belong to which term.
    struct TermCols {
        bool factor = false;
        std::string var;
        std::vector<double> levels;   // factor: every level, base first
        std::vector<int> postedCols;  // factor: one per level; continuous: one
    };
    std::vector<TermCols> terms;
};

struct ModelSpec {
    std::string depvar;
    std::vector<Term> terms;
    std::optional<std::string> ifExpr, inRange;
    std::string weightVar;   // aweight variable, "" = none
    std::string clusterVar;  // "" = OIM
    // vce(robust): the sandwich of the per-observation scores, i.e. the cluster
    // VCE with every observation its own cluster (StataBE bank 8aa87f1b `glm ...,
    // family(poisson) robust`: e() as cluster(sid)'s but vce robust, no
    // e(clustvar)/e(N_clust))
    bool robust = false;
    std::string offsetVar;   // offset() or exposure() variable, "" = none
    bool exposure = false;
    std::string wtype = "aw";  // the weight's kind: "aw" or "fw"
    bool noconstant = false;
    // family(binomial varname|#): the denominator variable, or the constant
    std::string mVar;
    double mConst = 1.0;
};

ojson specToJson(const ModelSpec& m) {
    ojson j;
    j["depvar"] = m.depvar;
    ojson t = ojson::array();
    for (const auto& x : m.terms) {
        ojson tj{{"var", x.var}, {"factor", x.factor}};
        if (!x.atoms.empty()) {
            ojson aj = ojson::array();
            for (const auto& a : x.atoms) aj.push_back(ojson{{"var", a.var}, {"factor", a.factor}});
            tj["atoms"] = aj;
        }
        t.push_back(tj);
    }
    j["terms"] = t;
    j["if"] = m.ifExpr ? ojson(*m.ifExpr) : ojson(nullptr);
    j["in"] = m.inRange ? ojson(*m.inRange) : ojson(nullptr);
    j["weight"] = m.weightVar;
    j["cluster"] = m.clusterVar;
    if (m.robust) j["robust"] = true;
    j["offset"] = m.offsetVar;
    j["exposure"] = m.exposure;
    if (m.wtype != "aw") j["wtype"] = m.wtype;
    if (m.noconstant) j["noconstant"] = true;
    if (!m.mVar.empty()) j["binomial_m_var"] = m.mVar;
    if (m.mConst != 1.0) j["binomial_m"] = m.mConst;
    return j;
}
ModelSpec specFromJson(const ojson& j) {
    ModelSpec m;
    m.depvar = j.value("depvar", std::string());
    for (const auto& t : j.value("terms", ojson::array())) {
        Term x;
        x.var = t.value("var", std::string());
        x.factor = t.value("factor", false);
        for (const auto& a : t.value("atoms", ojson::array())) {
            Term at;
            at.var = a.value("var", std::string());
            at.factor = a.value("factor", false);
            x.atoms.push_back(at);
        }
        m.terms.push_back(x);
    }
    if (j.contains("if") && j["if"].is_string()) m.ifExpr = j["if"].get<std::string>();
    if (j.contains("in") && j["in"].is_string()) m.inRange = j["in"].get<std::string>();
    m.weightVar = j.value("weight", std::string());
    m.clusterVar = j.value("cluster", std::string());
    m.robust = j.value("robust", false);
    m.offsetVar = j.value("offset", std::string());
    m.exposure = j.value("exposure", false);
    m.wtype = j.value("wtype", std::string("aw"));
    m.noconstant = j.value("noconstant", false);
    m.mVar = j.value("binomial_m_var", std::string());
    m.mConst = j.value("binomial_m", 1.0);
    return m;
}

// Read one numeric column; a string (factor-coded) column is Stata's r(109).
bool readNumeric(const Datastore& ds, const std::string& name, std::vector<double>& out, ojson& err) {
    if (!ds.has(name)) {
        err = errRc("variable " + name + " not found", 111);
        return false;
    }
    const cecon::Column& c = ds.col(name);
    if (c.dtype != cecon::DType::F64) {
        err = errRc("string variables not allowed in varlist; " + name + " is a string variable", 109);
        return false;
    }
    const double* d = c.f64();
    if (d == nullptr) {
        err = errRc(name + ": column has no readable payload", 1);
        return false;
    }
    out.assign(d, d + ds.nRows());
    return true;
}

// Open the frame the model reads, restricted by if/in.
struct FrameScope {
    std::unique_ptr<Datastore> owned;
    Datastore* ds = nullptr;
    ojson err;
    // the fit's filtered copy keeps each row's 1-based position in the working
    // frame here, so e(sample) maps back onto it ("" = the frame itself)
    std::string indexVar;
};
FrameScope openFrame(SessionState& session, const std::string& basePath, const ModelSpec& m,
                     bool live) {
    FrameScope f;
    const bool wantFilter = (m.ifExpr && !m.ifExpr->empty()) || (m.inRange && !m.inRange->empty());
    try {
        if (wantFilter) {
            InRangeFilterResult r = live ? applyIfAndInFilterKeepingIndex(session, basePath, m.ifExpr, m.inRange,
                                                                          &f.indexVar)
                                         : applyIfAndInFilter(session, basePath, m.ifExpr, m.inRange);
            if (!r.ok) {
                f.err = inRangeRejectError(r, m.inRange.value_or(std::string()), "glm");
                return f;
            }
            if (r.dataPath != basePath) session.noteProjectionScratch(r.dataPath);
            f.owned = std::make_unique<Datastore>(r.dataPath);
            f.ds = f.owned.get();
            return f;
        }
        if (live) {
            f.ds = session.datastore();
        } else {
            f.owned = std::make_unique<Datastore>(basePath);
            f.ds = f.owned.get();
        }
    } catch (const CobraError& ce) {
        f.err = ojson(eval::engineError("glm: " + cobraErrorMessage(ce), ce.code));
    } catch (const std::exception& e) {
        f.err = errRc(std::string("glm: could not read the data -- ") + e.what(), 1);
    }
    if (f.ds == nullptr && f.err.is_null()) f.err = ojson(eval::noDataLoaded());
    return f;
}

bool buildDesign(const Datastore& ds, const ModelSpec& m, cecon::GlmFamily fam, Design& d,
                 ojson& err) {
    const int64_t nAll = ds.nRows();
    std::vector<double> y;
    if (!readNumeric(ds, m.depvar, y, err)) return false;
    // Every variable a term reads (an interaction reads each of its parts).
    std::map<std::string, std::vector<double>> varCols;
    for (const auto& term : m.terms) {
        std::vector<std::string> vs;
        if (term.atoms.empty()) vs.push_back(term.var);
        for (const auto& a : term.atoms) vs.push_back(a.var);
        for (const auto& v : vs)
            if (!varCols.count(v) && !readNumeric(ds, v, varCols[v], err)) return false;
    }
    std::vector<double> w, cl, off;
    if (!m.weightVar.empty() && !readNumeric(ds, m.weightVar, w, err)) return false;
    if (!m.clusterVar.empty() && !readNumeric(ds, m.clusterVar, cl, err)) return false;
    if (!m.offsetVar.empty() && !readNumeric(ds, m.offsetVar, off, err)) return false;
    std::vector<double> mv;
    if (!m.mVar.empty() && !readNumeric(ds, m.mVar, mv, err)) return false;

    std::vector<int64_t> rows;
    for (int64_t i = 0; i < nAll; ++i) {
        bool ok = std::isfinite(y[i]);
        for (auto it = varCols.begin(); ok && it != varCols.end(); ++it) ok = std::isfinite(it->second[i]);
        if (ok && !w.empty()) ok = std::isfinite(w[i]);
        if (ok && !cl.empty()) ok = std::isfinite(cl[i]);
        if (ok && !off.empty()) ok = std::isfinite(off[i]);
        if (ok && !mv.empty()) ok = std::isfinite(mv[i]);
        if (ok) rows.push_back(i);
    }
    const int64_t n = static_cast<int64_t>(rows.size());
    // An empty sample is r(2000), a one-row sample r(2001), as in Stata.
    if (n == 0) {
        err = errRc("no observations", 2000);
        return false;
    }
    if (n == 1) {
        err = errRc("insufficient observations", 2001);
        return false;
    }
    // Stata's depvar/exposure domain checks, with its exit codes.
    for (int64_t r : rows) {
        const double yi = y[r];
        if ((fam == cecon::GlmFamily::Poisson || fam == cecon::GlmFamily::NegBinomial ||
             fam == cecon::GlmFamily::Bernoulli) && yi < 0) {
            err = errRc("dependent variable " + m.depvar + " has negative values", 499);
            return false;
        }
        // glim_v2's checks: m > 0, then y <= m ("`y' > `m' in some cases")
        const double mi = mv.empty() ? m.mConst : mv[r];
        if (fam == cecon::GlmFamily::Bernoulli && !(mi > 0)) {
            err = errRc(m.mVar + " has nonpositive values", 499);
            return false;
        }
        if (fam == cecon::GlmFamily::Bernoulli && yi > mi) {
            err = errRc(m.depvar + " > " + (m.mVar.empty() ? std::string("1") : m.mVar) + " in some cases", 499);
            return false;
        }
        if (fam == cecon::GlmFamily::InverseGaussian && !(yi > 0)) {
            err = refuse("glm_igaussian_nonpositive_depvar");
            return false;
        }
        if (fam == cecon::GlmFamily::Gamma && !(yi > 0)) {
            err = refuse("glm_gamma_nonpositive_depvar");
            return false;
        }
        if (!w.empty() && !(w[r] > 0)) {
            err = refuse("glm_weight_nonpositive");
            return false;
        }
        if (m.exposure && !(off[r] > 0)) {
            err = errRc("exposure() must be greater than zero", 459);
            return false;
        }
    }

    Design out;
    out.n = n;
    out.rows = rows;
    out.y.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) out.y[i] = y[rows[i]];
    if (!w.empty()) {
        out.w.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            out.w[i] = w[rows[i]];
            out.sumW += out.w[i];
        }
    }
    if (!mv.empty() || m.mConst != 1.0) {
        out.m.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) out.m[i] = mv.empty() ? m.mConst : mv[rows[i]];
    }
    if (!off.empty()) {
        out.off.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) out.off[i] = m.exposure ? std::log(off[rows[i]]) : off[rows[i]];
    }
    if (!cl.empty()) {
        std::map<double, int64_t> ids;
        for (int64_t i = 0; i < n; ++i) ids.emplace(cl[rows[i]], 0);
        int64_t g = 0;
        for (auto& kv : ids) kv.second = g++;
        out.cl.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) out.cl[i] = ids[cl[rows[i]]];
        out.nClust = g;
    } else if (m.robust) {
        // every observation its own cluster: the engine's robust sandwich (which
        // counts a frequency-weighted row w_i times), no cluster ids
        out.nClust = n;
    }
    auto addCol = [&](const std::vector<double>& c, const std::string& name) {
        out.X.insert(out.X.end(), c.begin(), c.end());
        out.estIndex.push_back(out.k++);
        out.names.push_back(name);
    };
    // A factor's levels over the sample (452 for a negative or noninteger value).
    auto levelsOf = [&](const std::string& var, std::vector<double>& lvOut) {
        std::set<double> lv;
        const std::vector<double>& c = varCols.at(var);
        for (int64_t i = 0; i < n; ++i) {
            const double v = c[rows[i]];
            if (v < 0 || v != std::floor(v)) {
                err = errRc(var + ":  factor variables may not contain " +
                                std::string(v < 0 ? "negative" : "noninteger") + " values",
                            452);
                return false;
            }
            lv.insert(v);
        }
        lvOut.assign(lv.begin(), lv.end());
        return true;
    };
    auto levelText = [](double L) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f", L);
        return std::string(buf);
    };
    std::set<std::string> seen;
    for (size_t t = 0; t < m.terms.size(); ++t) {
        const Term& term = m.terms[t];
        if (!term.atoms.empty()) {
            // An interaction: one column per cell of its factor levels (first
            // factor slowest), each the product of its parts. A cell with a
            // factor at its base level is omitted: posted 0 with the Stata
            // spelling `0b.tt#co.d`, `1o.tt#0b.dcat` (w2v_margins_corpus F7/F7nb).
            std::string key;
            for (const auto& a : term.atoms) key += std::string(key.empty() ? "" : "#") + (a.factor ? "i." : "c.") + a.var;
            if (!seen.insert(key).second) {
                err = refuse("glm_collinear_regressors");
                return false;
            }
            const size_t A = term.atoms.size();
            std::vector<std::vector<double>> lv(A);
            for (size_t a = 0; a < A; ++a)
                if (term.atoms[a].factor && !levelsOf(term.atoms[a].var, lv[a])) return false;
            Design::TermCols tc;
            std::vector<size_t> idx(A, 0);
            while (true) {
                bool omitted = false;
                for (size_t a = 0; a < A; ++a) omitted = omitted || (term.atoms[a].factor && idx[a] == 0);
                std::string name;
                for (size_t a = 0; a < A; ++a) {
                    const Term& at = term.atoms[a];
                    std::string piece = at.factor ? levelText(lv[a][idx[a]]) + (idx[a] == 0 ? "b" : (omitted ? "o" : "")) + "." + at.var
                                                  : std::string(omitted ? "co." : "c.") + at.var;
                    name += (a == 0 ? "" : "#") + piece;
                }
                tc.postedCols.push_back(static_cast<int>(out.names.size()));
                if (omitted) {
                    out.names.push_back(name);
                    out.estIndex.push_back(-1);
                } else {
                    std::vector<double> c(static_cast<size_t>(n), 1.0);
                    for (size_t a = 0; a < A; ++a) {
                        const std::vector<double>& src = varCols.at(term.atoms[a].var);
                        for (int64_t i = 0; i < n; ++i) {
                            const double v = src[rows[i]];
                            c[i] *= term.atoms[a].factor ? (v == lv[a][idx[a]] ? 1.0 : 0.0) : v;
                        }
                    }
                    addCol(c, name);
                }
                size_t pos = A;
                bool done = true;
                while (pos > 0) {
                    --pos;
                    if (!term.atoms[pos].factor) continue;
                    if (++idx[pos] < lv[pos].size()) {
                        done = false;
                        break;
                    }
                    idx[pos] = 0;
                }
                if (done) break;
            }
            out.terms.push_back(tc);
            continue;
        }
        const std::string key = (term.factor ? "i." : "") + term.var;
        if (!seen.insert(key).second) {
            // A repeated regressor is collinear; Stata omits it (o.x). Not covered.
            err = refuse("glm_collinear_regressors");
            return false;
        }
        Design::TermCols tc;
        tc.factor = term.factor;
        tc.var = term.var;
        if (!term.factor) {
            std::vector<double> c(static_cast<size_t>(n));
            const std::vector<double>& src = varCols.at(term.var);
            for (int64_t i = 0; i < n; ++i) c[i] = src[rows[i]];
            tc.postedCols.push_back(static_cast<int>(out.names.size()));
            addCol(c, term.var);
        } else {
            std::vector<double> lv;
            if (!levelsOf(term.var, lv)) return false;
            const std::vector<double>& src = varCols.at(term.var);
            bool base = true;
            for (double L : lv) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.0f", L);
                tc.levels.push_back(L);
                tc.postedCols.push_back(static_cast<int>(out.names.size()));
                if (base) {
                    out.names.push_back(std::string(buf) + "b." + term.var);
                    out.estIndex.push_back(-1);
                    base = false;
                    continue;
                }
                std::vector<double> c(static_cast<size_t>(n));
                for (int64_t i = 0; i < n; ++i) c[i] = (src[rows[i]] == L) ? 1.0 : 0.0;
                addCol(c, std::string(buf) + "." + term.var);
            }
        }
        out.terms.push_back(tc);
    }
    if (!m.noconstant) addCol(std::vector<double>(static_cast<size_t>(n), 1.0), "_cons");
    if (out.k == 0) {
        err = errRc("no variables specified", 102);
        return false;
    }
    d = std::move(out);
    return true;
}

// Full posted b/V (base columns zero) from the estimable ones.
void expandPosted(const Design& d, const std::vector<double>& b, const std::vector<double>& V,
                  std::vector<double>& bp, std::vector<double>& Vp) {
    const size_t K = d.names.size();
    bp.assign(K, 0.0);
    Vp.assign(K * K, 0.0);
    for (size_t i = 0; i < K; ++i) {
        const int ei = d.estIndex[i];
        if (ei < 0) continue;
        bp[i] = b[static_cast<size_t>(ei)];
        for (size_t j = 0; j < K; ++j) {
            const int ej = d.estIndex[j];
            if (ej < 0) continue;
            Vp[i * K + j] = V[static_cast<size_t>(ei) * d.k + ej];
        }
    }
}

}  // namespace

// e(rank) as Stata posts it for glm: the rank of the FINAL e(V), i.e. rowsof(V) -
// diag0cnt(invsym(V)). With vce(cluster) and more
// parameters than clusters the robust V is rank-deficient (oracle
// aer_estim2_glm syn_B/syn_C: 42 columns, 8 clusters, e(rank) 1), and e(df),
// e(df_m), the (1/df) dispersions, AIC and BIC all follow that rank.
// invsym's drop rule is not documented; it was measured on StataBE (scratch
// x2-glm-native/rank/st): a column is dropped when its sweep pivot is
// <= 1e-9 x its original diagonal, or below an absolute 1e-19 floor (5e-20
// dropped, 1e-19 kept), whatever the matrix scale. Columns are swept
// largest-ABSOLUTE-pivot first: in natural order syn_G's IRLS V would keep a
// roundoff pivot (5.2e-9) and count 9, Stata posts 8; largest-relative-pivot
// first ties (1.0) on V = (1e-12*(1+1e-8), 1e-6 \ 1e-6, 1), sweeps the tiny
// column first and keeps the other at relative 1e-8 (rank 2), where Stata
// posts 1 (cal.log COLR|1e-8). Absolute-first reproduces all 116 calibration
// rows (cal/cal2/cal3.log) and e(rank) on all 24 V-bearing arms of
// aer_estim2_glm_oracle.log.
int64_t glmVceRank(const std::vector<double>& V, size_t k) {
    std::vector<double> A = V;
    std::vector<double> d0(k);
    std::vector<bool> left(k);
    for (size_t j = 0; j < k; ++j) {
        d0[j] = A[j * k + j];
        left[j] = d0[j] > 0.0;
    }
    int64_t rank = 0;
    for (;;) {
        size_t best = k;
        double bestPivot = 0.0;
        for (size_t j = 0; j < k; ++j) {
            if (!left[j]) continue;
            const double p = A[j * k + j];
            if (p <= 1e-9 * d0[j] || p < 1e-19) continue;
            if (best == k || p > bestPivot) {
                best = j;
                bestPivot = p;
            }
        }
        if (best == k) break;
        ++rank;
        left[best] = false;
        const double p = A[best * k + best];
        const std::vector<double> row(A.begin() + static_cast<std::ptrdiff_t>(best * k),
                                      A.begin() + static_cast<std::ptrdiff_t>(best * k + k));
        for (size_t i = 0; i < k; ++i)
            for (size_t j = 0; j < k; ++j) A[i * k + j] -= row[i] * row[j] / p;
    }
    return rank;
}

namespace {

void postEMatrix(SessionState& session, const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices) {
        if (kv.first == nm) {
            kv.second = std::move(m);
            return;
        }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
}
void dropEMatrix(SessionState& session, const std::string& nm) {
    auto& v = session.eval.eMatrices;
    v.erase(std::remove_if(v.begin(), v.end(), [&nm](const auto& kv) { return kv.first == nm; }),
            v.end());
}
eval::EvalMatrix rowVector(const std::vector<double>& v, const std::vector<std::string>& names) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {"y1"};
    m.colnames = names;
    return m;
}
eval::EvalMatrix square(const std::vector<double>& V, const std::vector<std::string>& names) {
    eval::EvalMatrix m;
    const size_t K = names.size();
    for (size_t i = 0; i < K; ++i) m.data.emplace_back(V.begin() + i * K, V.begin() + (i + 1) * K);
    m.rownames = names;
    m.colnames = names;
    return m;
}

constexpr double kZ975 = 1.959963984540054;  // invnormal(0.975)

ojson coefRow(const std::string& name, double b, double se) {
    ojson r;
    r["name"] = name;
    r["b"] = b;
    r["se"] = se;
    const double z = b / se;
    r["t"] = z;
    r["p"] = std::erfc(std::fabs(z) / std::sqrt(2.0));
    r["ci_lo"] = b - kZ975 * se;
    r["ci_hi"] = b + kZ975 * se;
    return r;
}

}  // namespace

// ===========================================================================
// glm

ojson runGlm(const eval::ParsedCommand& parsed, SessionState& session) {
    if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
    if (auto e = eval::rejectUnknownOptions(parsed.opts, kVocabGlm.known, kVocabGlm.unimplemented))
        return ojson(*e);
    if (parsed.varlist.empty()) return errRc("varlist required", 100);

    // ---- options ----
    std::string famRaw, linkRaw, vceRaw, offsetRaw, exposureRaw, clusterOptRaw;
    std::string lnoffsetRaw, initRaw, fromRaw, scoreRaw;
    bool sawVce = false, noLog = false, robustOpt = false;
    bool noConstant = false, noHeader = false, asisOpt = false, difficultOpt = false, showGradient = false, showHessian = false;
    double vfactor = 1.0;
    int scaleKind = -1;  // -1 default, 0 scale(x2), 1 scale(#)
    double scaleValue = 0.0;
    std::optional<double> levelPct;
    cecon::GlmOptions opt;
    auto num = [](const eval::OptValue& v, double& out) {
        if (v.isBool) return false;
        const std::string s = trim(v.strValue);
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return !s.empty() && end != s.c_str() && *end == '\0' && std::isfinite(out);
    };
    for (const auto& [k, v] : parsed.opts) {
        if (k == "family") famRaw = v.isBool ? std::string() : v.strValue;
        else if (k == "link") linkRaw = v.isBool ? std::string() : v.strValue;
        else if (k == "vce") {
            vceRaw = v.isBool ? std::string() : v.strValue;
            sawVce = true;
        } else if (k == "offset") offsetRaw = v.isBool ? std::string() : trim(v.strValue);
        else if (k == "exposure") exposureRaw = v.isBool ? std::string() : trim(v.strValue);
        else if (k == "nolog") noLog = true;
        else if (k == "level") {
            double lv = 0;
            if (!num(v, lv) || !(lv >= 10.0 && lv <= 99.99))
                return errRc("level() must be between 10 and 99.99 inclusive", 198);
            levelPct = lv;
        }
        // cluster(varname): glm's spelling of vce(cluster varname) (bank 138473a3
        // `glm fatal beertax unemp, family(poisson) cluster(sid)`). glm declares it
        // with one variable, and robust and eform as flags: a missing or extra argument
        // is a syntax error, r(198).
        else if (k == "cluster") {
            const std::string cv = v.isBool ? std::string() : trim(v.strValue);
            if (cv.empty()) return errRc("option cluster() incorrectly specified", 198);
            if (splitWs(cv).size() > 1) return errRc("too many variables specified", 103);
            clusterOptRaw = cv;
        } else if (k == "robust") {
            if (!v.isBool) return errRc("option robust() incorrectly specified", 198);
            robustOpt = v.boolValue;
        } else if (k == "eform") {
            if (!v.isBool) return errRc("option eform() incorrectly specified", 198);
        } else if (k == "iterate") {
            double it = 0;
            if (!num(v, it) || it != std::floor(it)) return syntaxErr("option iterate() incorrectly specified");
            if (it < 0) return syntaxErr("iterate() must be positive");
            opt.maxiter = static_cast<int>(it);
        } else if (k == "tolerance" || k == "ltolerance" || k == "nrtolerance") {
            double t = 0;
            if (!num(v, t) || t < 0) return syntaxErr("option " + k + "() incorrectly specified");
            // glm.ado: the log-likelihood tolerance lies in [0,1) -- `ltolerance(10)`
            // is r(198) "ltolerance() must be in [0,1)" (vs-stata bank 6362d6ebaf56d5ea).
            if (k == "ltolerance" && t >= 1) return syntaxErr("ltolerance() must be in [0,1)");
            (k == "tolerance" ? opt.tol : k == "ltolerance" ? opt.ltol : opt.nrtol) = t;
        }
        // lnoffset(varname): exposure()'s other spelling (glm.ado: `local lnoffset
        // `exposure''), e(offset) ln(varname)
        else if (k == "lnoffset") lnoffsetRaw = v.isBool ? std::string() : trim(v.strValue);
        else if (k == "noconstant") noConstant = true;
        else if (k == "noheader") noHeader = true;
        else if (k == "asis") asisOpt = true;
        // ml is the default optimization (Newton-Raphson through ml); nodots only
        // silences the replication dots of vce(bootstrap)/vce(jackknife), which
        // this fit never runs
        else if (k == "ml" || k == "nodots") {
            if (!v.isBool) return errRc("option " + k + "() not allowed", 198);
        } else if (k == "difficult") difficultOpt = true;
        else if (k == "gradient") showGradient = true;
        else if (k == "hessian") showHessian = true;
        else if (k == "technique") {
            const std::string t = lower(trim(v.isBool ? std::string() : v.strValue));
            // Newton-Raphson, glm's own (e(technique) nr); bhhh/dfp/bfgs walk another path
            if (t != "nr") {
                const std::vector<std::pair<std::string, eval::OptValue>> one = {{k, v}};
                if (auto e = eval::rejectUnknownOptions(one, {}, {"technique"})) return ojson(*e);
            }
        } else if (k == "vfactor") {
            double vf = 0;
            if (!num(v, vf)) return syntaxErr("option vfactor() incorrectly specified");
            if (!(vf > 0)) return syntaxErr("vfactor() must be positive");
            vfactor = vf;
        } else if (k == "fisher") {
            double f = 0;
            if (!num(v, f) || f != std::floor(f)) return syntaxErr("option fisher() incorrectly specified");
            if (!(f > 0)) return syntaxErr("fisher() must be positive");
            opt.fisher = static_cast<int>(f);
        } else if (k == "scale") {
            const std::string t = lower(trim(v.isBool ? std::string() : v.strValue));
            if (t == "x2") scaleKind = 0;
            else if (t == "dev") return syntaxErr("option scale(dev) allowed only with option irls");
            else {
                double sc = 0;
                if (!num(v, sc)) return syntaxErr("invalid scale()");
                if (!(sc > 0)) return syntaxErr("scale(#) must be positive");
                scaleKind = 1;
                scaleValue = sc;
            }
        } else if (k == "init" || k == "mu") {
            const std::string vn = v.isBool ? std::string() : trim(v.strValue);
            if (!initRaw.empty() && initRaw != vn) return syntaxErr("mu() and init() should use the same varname");
            initRaw = vn;
        } else if (k == "from") {
            fromRaw = v.isBool ? std::string() : trim(v.strValue);
        } else if (k == "score") {
            scoreRaw = v.isBool ? std::string() : trim(v.strValue);
        }
        // log / trace / showstep / showtolerance change only the iteration
        // display; the plain iteration log below is printed for all of them (the
        // trace's parameter/gradient dumps are text Stata's oracle marks "not pins").
    }
    if (!offsetRaw.empty() && !lnoffsetRaw.empty())
        return syntaxErr("only one of offset() or lnoffset() can be specified");
    if (!lnoffsetRaw.empty() && !exposureRaw.empty())
        return syntaxErr("only one of lnoffset() or exposure() can be specified");
    if (!lnoffsetRaw.empty()) exposureRaw = lnoffsetRaw;
    if (!offsetRaw.empty() && !exposureRaw.empty())
        return syntaxErr("only one of offset() or exposure() can be specified");

    FamilySpec fs;
    LinkSpec ls;
    ojson err;
    if (!parseFamily(famRaw, fs, err)) return err;
    if (!parseLink(linkRaw, fs, ls, err)) return err;
    opt.family = fs.fam;
    opt.link = ls.link;
    opt.alpha = fs.alpha;

    // ---- vce ----
    // vce(), cluster() and robust combine as Stata's vce parser combines them: with
    // vce(), cluster() is allowed only beside vce(cluster) of the same variable (another
    // one: "in conflict", r(100)) and vce(oim); beside vce(robust) it is "only one of",
    // r(198). robust is allowed beside vce(robust) and vce(cluster). Without vce(),
    // cluster() is vce(cluster), else robust is vce(robust).
    ModelSpec spec;
    std::string clusterOptVar;  // cluster() beside vce(cluster): checked once resolved
    if (sawVce) {
        const std::string t = lower(trim(vceRaw));
        std::string rest;
        const std::string canon = canonicalVceType(vceRaw, &rest);
        if (t == "oim") {
            // the default, spelled out; cluster() or robust beside it is not served
            if (!clusterOptRaw.empty()) return refuse("glm_vce_oim_cluster");
            if (robustOpt) return refuse("glm_vce_oim_robust");
        } else if (canon == "cluster") {
            const std::vector<std::string> cv = splitWs(rest);
            if (cv.empty()) return syntaxErr("option vce(cluster) requires a variable");
            if (cv.size() > 1) return refuse("glm_vce_multiway_cluster");
            spec.clusterVar = cv[0];
            clusterOptVar = clusterOptRaw;
        } else if (canon == "robust" && rest.empty()) {
            if (!clusterOptRaw.empty())
                return errRc("only one of options cluster() or vce(robust) is allowed", 198);
            spec.robust = true;
        } else if (!t.empty()) {
            return refuse("glm_vce_" + splitWs(t)[0]);
        }
    } else if (!clusterOptRaw.empty()) {
        spec.clusterVar = clusterOptRaw;
    } else if (robustOpt) {
        spec.robust = true;
    }

    // ---- weights ----
    if (parsed.weightType.has_value()) {
        const std::string wt = lower(*parsed.weightType);
        if (wt == "fw" || wt == "fweight") spec.wtype = "fw";
        else if (wt != "aw" && wt != "aweight") return refuse("glm_weight_" + wt);
    }
    spec.noconstant = noConstant;
    spec.mConst = fs.mConst;
    if (asisOpt && !(fs.fam == cecon::GlmFamily::Bernoulli && fs.mText == "1" && fs.mVar.empty()))
        return syntaxErr("option asis not allowed; option asis is only allowed with family(binomial) "
                         "with a denominator of 1");

    // ---- data + terms ----
    const std::string basePath = *session.currentHbin();
    Datastore* live = nullptr;
    try {
        live = session.datastore();
    } catch (const std::exception& e) {
        return errRc(std::string("glm: could not read the data -- ") + e.what(), 1);
    }
    if (live == nullptr) return ojson(eval::noDataLoaded());
    const std::vector<std::string> raw = rawTerms(parsed.varlist);
    // glm.ado counts the varlist: a depvar alone beside noconstant is r(100)
    if (noConstant && raw.size() == 1)
        return errRc("independent variables required with noconstant option", 100);
    {
        std::vector<Term> dep;
        if (!parseTerm(*live, raw[0], dep, err)) return err;
        if (dep.size() != 1 || dep[0].factor) return syntaxErr("depvar may not be a factor variable");
        spec.depvar = dep[0].var;
    }
    for (size_t i = 1; i < raw.size(); ++i)
        if (!parseTerm(*live, raw[i], spec.terms, err)) return err;
    if (!hierarchicalInteractions(spec.terms, err)) return err;
    auto resolveAux = [&](const std::string& rawName, std::string& out) {
        return resolveVar(*live, rawName, out, err);
    };
    if (!spec.clusterVar.empty() && !resolveAux(spec.clusterVar, spec.clusterVar)) return err;
    if (!clusterOptVar.empty()) {
        std::string other;
        if (!resolveAux(clusterOptVar, other)) return err;
        if (other != spec.clusterVar)
            return errRc("options cluster(clustvar) and vce(cluster clustvar) are in conflict", 100);
    }
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty() &&
        !resolveAux(trim(*parsed.weightVar), spec.weightVar))
        return err;
    if (!offsetRaw.empty() && !resolveAux(offsetRaw, spec.offsetVar)) return err;
    if (!fs.mVar.empty()) {
        if (!resolveAux(fs.mVar, spec.mVar)) return err;
        // family(binomial varname) is unabbreviated before its name is written into
        // e(m), the variance and the link formulas
        if (spec.mVar != fs.mVar) {
            fs.mVar = spec.mVar;
            fs.mText = spec.mVar;
            fs.varfuncf = "u*(1-u/" + spec.mVar + ")";
            if (!parseLink(linkRaw, fs, ls, err)) return err;
        }
    }
    std::string initVar;
    if (!initRaw.empty() && !resolveAux(initRaw, initVar)) return err;
    if (!exposureRaw.empty()) {
        if (!resolveAux(exposureRaw, spec.offsetVar)) return err;
        spec.exposure = true;
    }
    spec.ifExpr = parsed.ifExpr;
    spec.inRange = parsed.inRange;

    FrameScope frame = openFrame(session, basePath, spec, true);
    if (!frame.err.is_null()) return frame.err;
    Design d;
    if (!buildDesign(*frame.ds, spec, fs.fam, d, err)) return err;
    // each sample row's position in the working frame (the design's own rows
    // unless the fit read a filtered copy)
    std::vector<int64_t> headRows = d.rows;
    bool headRowsKnown = true;
    if (!frame.indexVar.empty()) {
        std::vector<double> idx;
        ojson ierr;
        headRowsKnown = readNumeric(*frame.ds, frame.indexVar, idx, ierr);
        for (size_t i = 0; headRowsKnown && i < d.rows.size(); ++i) {
            const double v = idx[static_cast<size_t>(d.rows[i])];
            if (!(v >= 1)) headRowsKnown = false;
            else headRows[i] = static_cast<int64_t>(v) - 1;
        }
    } else if ((spec.ifExpr && !spec.ifExpr->empty()) || (spec.inRange && !spec.inRange->empty())) {
        headRowsKnown = false;
    }
    // An m = 1 binomial whose outcome is not 0/1 is glim_v2's "Binomial" (the
    // variance written u*(1-u/1)), not the Bernoulli.
    if (fs.fam == cecon::GlmFamily::Bernoulli && fs.mText == "1") {
        bool binaryY = true;
        for (double yi : d.y) binaryY = binaryY && (yi == 0.0 || yi == 1.0);
        if (!binaryY) {
            fs.varfunct = "Binomial";
            fs.varfuncf = "u*(1-u/1)";
        }
    }

    // ---- fit ----
    opt.power = ls.p;
    opt.binom_m = d.m.empty() ? nullptr : d.m.data();
    const bool fweighted = !d.w.empty() && spec.wtype == "fw";
    if (fweighted) {
        for (double wi : d.w)
            if (wi != std::floor(wi)) return errRc("may not use noninteger frequency weights", 401);
        opt.fweight = d.w.data();
    }
    // scale(#) of a family whose dispersion is estimated fixes phi in every
    // evaluation ($SGLM_s1 = #); a scale-one family keeps 1 and has e(V) rescaled
    // after the fit (glm.ado `fixme`).
    const bool scaleOneFam = fs.fam == cecon::GlmFamily::Bernoulli || fs.fam == cecon::GlmFamily::Poisson ||
                             fs.fam == cecon::GlmFamily::NegBinomial;
    if (scaleKind == 1 && !scaleOneFam) opt.fixed_phi = scaleValue;
    std::vector<double> muStart;
    if (!initVar.empty()) {
        std::vector<double> col;
        if (!readNumeric(*frame.ds, initVar, col, err)) return err;
        for (int64_t r0 : d.rows) muStart.push_back(col[static_cast<size_t>(r0)]);
        opt.mu_start = muStart.data();
    }
    if (!fromRaw.empty()) {
        // from(matname[, copy]): by position with copy, else by the column names
        // (an equation stripe is read past; a missing name starts at 0)
        std::string mat = fromRaw, rest;
        const size_t comma = fromRaw.find(',');
        if (comma != std::string::npos) {
            mat = trim(fromRaw.substr(0, comma));
            rest = lower(trim(fromRaw.substr(comma + 1)));
        }
        const bool copy = rest == "copy";
        if (!rest.empty() && !copy && rest != "skip") return syntaxErr("from() invalid");
        const eval::EvalMatrix* fm = session.eval.findMatrix(mat);
        if (fm == nullptr || fm->data.size() != 1) return errRc("matrix " + mat + " not found", 111);
        const std::vector<double>& row = fm->data[0];
        std::vector<double> start(static_cast<size_t>(d.k), 0.0);
        if (copy) {
            if (row.size() != static_cast<size_t>(d.k)) return errRc("from():  wrong number of values", 503);
            start = row;
        } else {
            for (size_t i = 0; i < d.names.size(); ++i) {
                if (d.estIndex[i] < 0) continue;
                for (size_t c = 0; c < fm->colnames.size() && c < row.size(); ++c) {
                    std::string cn = fm->colnames[c];
                    const size_t colon = cn.find(':');
                    if (colon != std::string::npos) cn = cn.substr(colon + 1);
                    if (cn == d.names[i]) start[static_cast<size_t>(d.estIndex[i])] = row[c];
                }
            }
        }
        opt.b_start = start;
    }
    opt.robust = spec.robust;
    cecon::GlmResult r = cecon::glm_fit(d.X.data(), d.n, d.k, d.y.data(),
                                        d.w.empty() || fweighted ? nullptr : d.w.data(),
                                        d.off.empty() ? nullptr : d.off.data(),
                                        d.cl.empty() ? nullptr : d.cl.data(), opt);
    if (!r.ok) {
        if (r.error.find("singular (collinear") != std::string::npos)
            return refuse("glm_collinear_regressors");
        return errRc("glm: " + r.error, 430);
    }
    // `difficult` changes ml's step only where the Hessian is not negative
    // definite; this fit has no nonconcave stepping of its own, so such a path is
    // refused rather than reported as difficult's.
    if (difficultOpt && r.nonconcave) return refuse("glm_difficult_nonconcave");
    // ml's model test reads the V it posts; scale() and vfactor() repost e(V)
    // afterwards (glm.ado), so e(chi2) keeps the unscaled one.
    const std::vector<double> Vml = r.V;
    std::string scaleMsg;
    {
        double fix = 1.0;
        if (scaleKind == 0 && scaleOneFam) {
            fix = r.pearson_phi;  // $SGLM_ph after ml: the Pearson scale at the last evaluation
            scaleMsg = "square root of Pearson X2-based dispersion";
        } else if (scaleKind == 1 && (!scaleOneFam || scaleValue != 1.0)) {
            if (scaleOneFam) fix = scaleValue;
            scaleMsg = "dispersion equal to square root of " + trim(gfmt(scaleValue, 12));
        }
        fix *= vfactor;
        if (fix != 1.0) {
            for (double& v : r.V) v *= fix;
            for (double& v : r.V_oim) v *= fix;
        }
    }
    // N: the frequency-weighted count under fweights (bank 14061316 [fw=w]: e(N) 672)
    const double nObs = fweighted ? d.sumW : static_cast<double>(d.n);
    // score(newvar): the equation-level score (y-mu) dmu/(phi V(mu)) of each
    // sample observation (glim_lf), missing elsewhere
    if (!scoreRaw.empty()) {
        std::vector<std::string> sv = splitWs(scoreRaw);
        if (sv.size() != 1) return syntaxErr("score(): one new variable name is required");
        std::string nm = sv[0];
        if (!nm.empty() && nm.back() == '*') nm = nm.substr(0, nm.size() - 1) + "1";
        if (session.datastore() != nullptr && session.datastore()->has(nm))
            return errRc("variable " + nm + " already defined", 110);
        if (!headRowsKnown || headRows.size() != r.scores.size() || session.datastore() == nullptr)
            return refuse("glm_score_filtered");
        std::vector<double> col(static_cast<size_t>(session.datastore()->nRows()),
                                std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 0; i < headRows.size(); ++i)
            if (headRows[i] >= 0 && static_cast<size_t>(headRows[i]) < col.size())
                col[static_cast<size_t>(headRows[i])] = r.scores[i];
        ojson werr;
        if (!statcmd_detail::estimWriteColumn(session, nm, col, &werr, "double")) return werr;
        session.invalidateDatastore();
    }

    std::vector<double> bp, Vp;
    expandPosted(d, r.beta, r.V, bp, Vp);
    const size_t K = d.names.size();
    // Stata: rank from the posted V, df = N - rank,
    // df_m = rank - 1. e(phi) is the fitter's (N - k) Pearson scale and does not
    // follow (oracle syn_C: phi = Pearson/199 while e(df) = 239).
    const int64_t rank = glmVceRank(r.V, static_cast<size_t>(d.k));
    const double df = nObs - static_cast<double>(rank);
    // the sandwich fits: vce(cluster) and vce(robust), the latter one observation per cluster
    const bool clustered = !spec.clusterVar.empty() || spec.robust;
    const std::string crit = clustered ? "Log pseudolikelihood" : "Log likelihood";

    ojson blocks = ojson::array();
    if (!d.w.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(sum of wgt is %.16g)", d.sumW);
        blocks.push_back(displayBlock(buf));
    }
    {
        std::string text;
        if (!noLog && (showGradient || showHessian)) {
            // ml's `hessian` / `gradient` display: each iteration framed by rules,
            // the criterion right-aligned, then the Hessian (lower triangle) and the
            // gradient vector in matrix-list layout on the equation stripe.
            const std::string rule(80, '-');
            std::vector<std::string> cn;
            for (size_t i = 0; i < d.names.size(); ++i)
                if (d.estIndex[i] >= 0) cn.push_back(d.names[i]);
            const std::string eq = spec.depvar + ":";
            const auto pad = [](const std::string& t, size_t w) {
                return t.size() >= w ? t : std::string(w - t.size(), ' ') + t;
            };
            const auto header = [&](size_t L) {
                std::string a(L + 1, ' '), b(L, ' ');
                for (const auto& c : cn) {
                    a += pad(spec.depvar, 10) + ":";
                    b += pad(c, 11);
                }
                return a + "\n" + b + "\n";
            };
            text += rule + "\n";
            for (size_t i = 0; i < r.ilog.size(); ++i) {
                text += "Iteration " + std::to_string(i) + ":\n";
                text += pad(crit + " = " + trim(gfmt(r.ilog[i], 10)), 80) + "\n";
                const size_t kk = cn.size();
                if (showHessian && i < r.hess_log.size() && r.hess_log[i].size() == kk * kk) {
                    size_t L = 0;
                    for (const auto& c : cn) L = std::max(L, eq.size() + c.size());
                    text += "Hessian matrix:\n" + header(L);
                    for (size_t a = 0; a < kk; ++a) {
                        std::string line = pad(eq + cn[a], L);
                        for (size_t c = 0; c <= a; ++c) line += "  " + pad(trim(gfmt(-r.hess_log[i][a * kk + c], 9)), 9);
                        text += line + "\n";
                    }
                }
                if (showGradient && i < r.grad_log.size() && r.grad_log[i].size() == kk) {
                    double g2 = 0.0;
                    for (double g : r.grad_log[i]) g2 += g * g;
                    text += "Gradient vector (length = " + trim(gfmt(std::sqrt(g2), 9)) + "):\n" + header(2);
                    std::string line = "r1";
                    for (double g : r.grad_log[i]) line += "  " + pad(trim(gfmt(g, 9)), 9);
                    text += line + "\n";
                }
                text += rule + "\n";
            }
        } else if (!noLog) {
            for (size_t i = 0; i < r.ilog.size(); ++i) {
                text += "Iteration " + std::to_string(i) + ":  " + crit + " = " + gfmt(r.ilog[i], 10) +
                        "  \n";
            }
        }
        if (!r.converged) text += "convergence not achieved\n";
        if (!text.empty()) {
            text.pop_back();
            blocks.push_back(displayBlock(text));
        }
    }

    // ---- header ----
    const double dispers = r.deviance / df, dispersP = r.deviance_p / df;
    const double aic = (-2.0 * r.ll + 2.0 * static_cast<double>(rank)) / nObs;
    const double bic = r.deviance - df * std::log(nObs);
    {
        std::vector<std::string> L;
        std::string s = "Generalized linear models";
        toCol(s, 51); s += "Number of obs"; toCol(s, 67); s += "="; toCol(s, 69); s += countFmt(nObs);
        L.push_back(s);
        s = "Optimization     : ML";
        toCol(s, 51); s += "Residual df"; toCol(s, 67); s += "="; toCol(s, 69); s += countFmt(df);
        L.push_back(s);
        s.clear();
        toCol(s, 51); s += "Scale parameter"; toCol(s, 67); s += "="; toCol(s, 70); s += gfmt(r.phi, 9);
        L.push_back(s);
        s = "Deviance"; toCol(s, 18); s += "="; toCol(s, 20); s += gfmt(r.deviance, 12);
        toCol(s, 51); s += "(1/df) Deviance"; toCol(s, 67); s += "="; toCol(s, 70); s += gfmt(dispers, 9);
        L.push_back(s);
        s = "Pearson"; toCol(s, 18); s += "="; toCol(s, 20); s += gfmt(r.deviance_p, 12);
        toCol(s, 51); s += "(1/df) Pearson"; toCol(s, 67); s += "="; toCol(s, 70); s += gfmt(dispersP, 9);
        L.push_back(s);
        L.push_back("");
        s = "Variance function: V(u) = "; toCol(s, 27); s += fs.varfuncf;
        toCol(s, 51); s += "[" + fs.varfunct + "]";
        L.push_back(s);
        s = "Link function    : g(u) = "; toCol(s, 27); s += ls.linkf;
        toCol(s, 51); s += "[" + ls.linkt + "]";
        L.push_back(s);
        L.push_back("");
        s.clear(); toCol(s, 51); s += "AIC"; toCol(s, 67); s += "="; toCol(s, 70); s += gfmt(aic, 9);
        L.push_back(s);
        s = crit; toCol(s, std::max<size_t>(18, crit.size() + 2)); s += "= " + gfmt(r.ll, 12);
        toCol(s, 51); s += "BIC"; toCol(s, 67); s += "="; toCol(s, 70); s += gfmt(bic, 9);
        L.push_back(s);
        if (clustered && !spec.robust) {
            const std::string note = "(Std. err. adjusted for " + std::to_string(d.nClust) +
                                     " clusters in " + spec.clusterVar + ")";
            L.push_back("");
            L.push_back(std::string(note.size() < 78 ? 78 - note.size() : 0, ' ') + note);
        }
        std::string text;
        for (const auto& line : L) text += line + "\n";
        text.pop_back();
        // noheader: the table alone (glm's header block is this text; the
        // renderer drops its own title lines under the block's noheader)
        if (!noHeader) blocks.push_back(displayBlock(text));
    }

    // ---- the estimation block: e() is read from it ----
    ojson block;
    block["type"] = "estimation";
    block["estimator"] = "glm";
    if (noHeader) block["noheader"] = true;
    block["depvar"] = spec.depvar;
    block["n_obs"] = nObs;
    // the level the reported interval is built at (r(table) reads it from here)
    block["level"] = levelPct.has_value() ? *levelPct : static_cast<double>(session.level);
    ojson ct = ojson::array();
    for (size_t i = 0; i < K; ++i) {
        if (d.estIndex[i] < 0) continue;  // base levels: posted in e(b), not tabulated
        ct.push_back(coefRow(d.names[i], bp[i], std::sqrt(Vp[i * K + i])));
    }
    block["coeftable"] = ct;
    // The e() scalars the oracle reads (gdump), under the names resolveEScalar
    // looks up: N/rank/N_clust/ll through the eScalarKey table, the rest by name.
    block["ll"] = r.ll;
    block["k_vars"] = rank;           // e(rank)
    block["df_m"] = spec.noconstant ? rank : rank - 1;
    block["df"] = df;
    block["k"] = static_cast<double>(K);
    block["ic"] = r.ic;
    block["converged"] = r.converged;
    block["rc"] = 0;
    block["phi"] = r.phi;
    block["deviance"] = r.deviance;
    block["deviance_p"] = r.deviance_p;
    block["dispers"] = dispers;
    block["dispers_p"] = dispersP;
    // Stata's scaled deviances and the two criteria, on the same df/rank.
    block["deviance_s"] = r.deviance / r.phi;
    block["dispers_s"] = r.deviance / r.phi / df;
    block["deviance_ps"] = r.deviance_p / r.phi;
    block["dispers_ps"] = r.deviance_p / r.phi / df;
    block["aic"] = aic;
    block["bic"] = bic;
    // glm's ml settings and model scalars (StataBE 19.5 bank, every ML glm fit of
    // the families and links fitted here: e(vf) 1, e(nbml) 0, e(k_eq) 1,
    // e(k_eq_model) 0, e(k_dv) 1, e(noconstant) 0, e(canonical) 1 for the
    // canonical pairs gaussian/identity, binomial/logit and poisson/log, 0 for
    // binomial/probit, gamma/log and nbinomial/log; e(power) 1 identity, 0
    // logit/log, the macro "." after probit). e(chi2) is ml's Wald test that
    // every estimated slope is 0 (bank 5cb00c3c `glm ..., family(poisson)`
    // chi2 3086.44, chi2type Wald); a V that does not invert posts none.
    std::optional<double> modelP;  // e(p), posted after the model install below
    {
        // glm.ado's canonical pairs: gaussian/identity, igaussian/power(-2),
        // poisson/log, nbinomial/nbinomial, binomial/logit, gamma/reciprocal
        using F = cecon::GlmFamily;
        using Lk = cecon::GlmLinkFn;
        const bool canonical = (fs.fam == F::Gaussian && ls.link == Lk::Identity) ||
                               (fs.fam == F::InverseGaussian && ls.link == Lk::PowerM2) ||
                               (fs.fam == F::Poisson && ls.link == Lk::Log) ||
                               (fs.fam == F::NegBinomial && ls.link == Lk::NegBin) ||
                               (fs.fam == F::Bernoulli && ls.link == Lk::Logit) ||
                               (fs.fam == F::Gamma && ls.link == Lk::Reciprocal);
        block["canonical"] = canonical ? 1.0 : 0.0;
        block["vf"] = vfactor;
        block["nbml"] = 0.0;
        block["k_eq"] = 1.0;
        block["k_eq_model"] = 0.0;
        block["k_dv"] = 1.0;
        block["noconstant"] = spec.noconstant ? 1.0 : 0.0;
        block["consonly"] = spec.terms.empty() && !spec.noconstant ? 1.0 : 0.0;
        double autoCns = 0.0;
        for (size_t i = 0; i < K; ++i) autoCns += d.estIndex[i] < 0 ? 1.0 : 0.0;
        block["k_autoCns"] = autoCns;
        if (ls.power != ".") block["power"] = ls.link == cecon::GlmLinkFn::Power || ls.link == cecon::GlmLinkFn::OPower
                                                  ? ls.p : std::stod(ls.power);
        std::vector<int> slope;  // estimable columns other than the constant
        for (size_t i = 0; i < K; ++i)
            if (d.estIndex[i] >= 0 && d.names[i] != "_cons") slope.push_back(d.estIndex[i]);
        const int q = static_cast<int>(slope.size());
        const size_t ke = static_cast<size_t>(d.k);
        if (q > 0 && Vml.size() == ke * ke && r.beta.size() == ke) {
            std::vector<double> Vs(static_cast<size_t>(q) * static_cast<size_t>(q)), bs(static_cast<size_t>(q));
            for (int a = 0; a < q; ++a) {
                bs[static_cast<size_t>(a)] = r.beta[static_cast<size_t>(slope[static_cast<size_t>(a)])];
                for (int c = 0; c < q; ++c)
                    Vs[static_cast<size_t>(a) * static_cast<size_t>(q) + static_cast<size_t>(c)] =
                        Vml[static_cast<size_t>(slope[static_cast<size_t>(a)]) * ke +
                            static_cast<size_t>(slope[static_cast<size_t>(c)])];
            }
            if (const auto w = waldQuadForm(Vs, bs, q)) {
                const double chi2 = *w;
                if (std::isfinite(chi2)) {
                    block["chi2"] = chi2;
                    modelP = est_detail::chi2UpperTailM4(static_cast<double>(q), chi2);
                }
            }
        }
    }
    if (spec.robust) {
        block["vcov_type"] = "robust";
    } else if (clustered) {
        block["n_clusters"] = d.nClust;
        block["vcov_type"] = "cluster(" + spec.clusterVar + ")";
    } else {
        block["vcov_type"] = "oim";
    }
    ojson model = specToJson(spec);
    model["family"] = static_cast<int>(fs.fam);
    model["alpha"] = fs.alpha;
    model["link"] = static_cast<int>(ls.link);
    model["b"] = bp;
    model["V"] = Vp;
    model["names"] = d.names;
    block["glm_model"] = model;

    LastModel lm;
    lm.cobraCmd = "glm";  // not in any engine post-estimation set: test/lincom/nlcom/predict refuse
    lm.stataCmd = "glm";
    lm.y = spec.depvar;
    for (const auto& t : spec.terms) {
        if (t.atoms.empty()) {
            lm.x.push_back((t.factor ? "i." : "") + t.var);
            continue;
        }
        std::string term;
        for (const auto& a : t.atoms) term += std::string(term.empty() ? "" : "#") + (a.factor ? "i." : "c.") + a.var;
        lm.x.push_back(term);
    }
    lm.vcov = spec.robust ? "robust" : clustered ? "cluster" : "oim";
    if (clustered && !spec.robust) lm.cluster = {spec.clusterVar};
    lm.depvar = spec.depvar;
    lm.dataPath = basePath;
    if (spec.ifExpr) lm.fitKeepif = *spec.ifExpr;
    if (!spec.weightVar.empty()) {
        lm.fitWeight = spec.weightVar;
        lm.fitWtype = spec.wtype;
    }
    lm.result = block;
    // e(sample): the design's rows. Without if/in the design read the working
    // frame itself, so its rows are the head's (a filtered fit reads a projected
    // copy whose rows are not, and keeps e(sample) unavailable).
    // (a filtered fit maps its copy's rows back through the kept index column)
    if (headRowsKnown && session.datastore() != nullptr) {
        std::vector<char> mask(static_cast<size_t>(session.datastore()->nRows()), 0);
        bool inRange = true;
        for (int64_t r : headRows) {
            if (r < 0 || static_cast<size_t>(r) >= mask.size()) inRange = false;
            else mask[static_cast<size_t>(r)] = 1;
        }
        if (inRange) (void)e3::installEsampleMask(session, lm, mask);
    }
    session.setLastModel(lm);
    session.lastEstBlock = block;
    session.estAddedClear();
    // setLastModel publishes e(cmd/depvar/vce/clustvar) and ERASES e(vcetype) for
    // a non-engine vce spelling; glm's own macros go in after it.
    session.eval.setEString("vce", spec.robust ? "robust" : clustered ? "cluster" : "oim");
    session.eval.setEString("vcetype", clustered ? "Robust" : "OIM");
    session.eval.setEString("varfunct", fs.varfunct);
    session.eval.setEString("linkt", ls.linkt);
    session.eval.setEString("opt", "moptimize");
    session.eval.setEString("technique", "nr");
    session.eval.setEString("a", fs.alphaText);
    // The family's and link's programs and formulas, ml's settings, and the
    // Newey-West lag glm records even without hac (the sample's rows less 2:
    // bank 5cb00c3c N 336, e(hac_lag) "334"). Noted on the fit, so the next
    // model install drops them.
    const auto postNoted = [&session](const char* name, const std::string& v) {
        session.eval.setEString(name, v);
        session.noteEOnFit(name);
    };
    if (modelP) e3::setEScalarOnFit(session, "p", *modelP);
    postNoted("link", ls.link_prog);
    postNoted("varfunc", fs.varfunc);
    postNoted("linkf", ls.linkf);
    postNoted("varfuncf", fs.varfuncf);
    if (ls.power == ".") postNoted("power", ".");
    postNoted("hac_lag", std::to_string(d.n - 2));  // rows, not the fweighted N (bank 14061316: 334)
    if (spec.noconstant) postNoted("cons", "noconstant");
    if (!scoreRaw.empty()) {
        std::string nm = splitWs(scoreRaw)[0];
        if (!nm.empty() && nm.back() == '*') nm = nm.substr(0, nm.size() - 1) + "1";
        postNoted("scorevars", nm);
    }
    postNoted("m", fs.mText);
    if (fs.mText != "1") postNoted("marginsprop", "atslow");  // glm.ado: m other than 1
    if (!scaleMsg.empty()) postNoted("msg", scaleMsg);
    postNoted("opt1", "ML");
    postNoted("oim", "oim");
    postNoted("chi2type", "Wald");
    postNoted("crittype", clustered ? "log pseudolikelihood" : "log likelihood");
    postNoted("ml_method", "e2");
    postNoted("user", "glim_lf");
    // `difficult` switches ml's nonconcave stepping to the hybrid method
    postNoted("singularHmethod", difficultOpt ? "hybrid" : "m-marquardt");
    postNoted("which", "max");
    postNoted("deriv_useminbound", "off");
    if (!spec.offsetVar.empty())
        postNoted("offset", spec.exposure ? "ln(" + spec.offsetVar + ")" : spec.offsetVar);
    // e(b)/e(V) are POSTED (the coeftable fallback would synthesize a diagonal
    // V and drop the base columns); e(ilog) is ml's 1 x 20, zero-filled.
    // glm's ML fit names its one equation after the depvar
    // (aer_estim2_glm_oracle.log:230-238 `price:mpg ... price:_cons`, syn_A
    // `y:1b.id`; w2v_estripe_census.log esc_glm_ml `yb:x1 yb:x2 yb:_cons`).
    // The irls fit is bare (esc_glm_irls, auto_G), but `irls` is in kVocabGlm's
    // unimplemented list, so every fit that reaches here is ML. The coefficient
    // table keeps the bare names `_b[x1]` reads.
    std::vector<std::string> eqNames;
    for (const auto& n : d.names) eqNames.push_back(spec.depvar + ":" + n);
    postEMatrix(session, "b", rowVector(bp, eqNames));
    postEMatrix(session, "V", square(Vp, eqNames));
    std::vector<double> ilog(20, 0.0);
    for (size_t i = 0; i < r.ilog.size() && i < ilog.size(); ++i) ilog[i] = r.ilog[i];
    std::vector<std::string> ilogNames;
    for (int i = 1; i <= 20; ++i) ilogNames.push_back("c" + std::to_string(i));
    {
        eval::EvalMatrix ilogM = rowVector(ilog, ilogNames);
        ilogM.rownames = {"r1"};  // StataBE: `matrix list e(ilog)` row r1
        postEMatrix(session, "ilog", std::move(ilogM));
    }
    std::vector<double> grad(K, 0.0);
    for (size_t i = 0; i < K; ++i)
        if (d.estIndex[i] >= 0) grad[i] = r.gradient[static_cast<size_t>(d.estIndex[i])];
    // e(gradient) is on e(b)'s equation stripe, in row r1 (StataBE 19.5, BUG-1219:
    // `matrix list e(gradient)` after glm yb x1 x2, family(binomial) is `r1` by
    // `yb:x1 yb:x2 yb:_cons`; glm y x1 x2 `y:x1 y:x2 y:_cons`).
    eval::EvalMatrix gradM = rowVector(grad, eqNames);
    gradM.rownames = {"r1"};
    postEMatrix(session, "gradient", std::move(gradM));
    // e(Cns): ml's constraint matrix for the base levels (one row each, 1 in the
    // level's column, and a last column _Cns:_r of 0), posted only when there is
    // one (bank 94c843ce `glm fatal beertax i.year, family(poisson)`: 1 x 10 on
    // `fatal:beertax fatal:1982b.year ... _Cns:_r`).
    {
        eval::EvalMatrix cns;
        cns.colnames = eqNames;
        cns.colnames.push_back("_Cns:_r");
        for (size_t i = 0; i < K; ++i) {
            if (d.estIndex[i] >= 0) continue;
            std::vector<double> row(K + 1, 0.0);
            row[i] = 1.0;
            cns.data.push_back(std::move(row));
            cns.rownames.push_back("r" + std::to_string(cns.data.size()));
        }
        if (!cns.data.empty()) postEMatrix(session, "Cns", std::move(cns));
        else dropEMatrix(session, "Cns");
    }
    // e(V_modelbased) after vce(cluster): the inverse Hessian the sandwich was
    // built on, on e(V)'s stripe (bank 5eeb21b0).
    if (clustered && r.V_oim.size() == r.V.size()) {
        std::vector<double> bp2, Vmb;
        expandPosted(d, r.beta, r.V_oim, bp2, Vmb);
        for (size_t a = 0; a < K; ++a)
            for (size_t c = 0; c < a; ++c) {
                const double m = 0.5 * (Vmb[a * K + c] + Vmb[c * K + a]);
                Vmb[a * K + c] = m;
                Vmb[c * K + a] = m;
            }
        postEMatrix(session, "V_modelbased", square(Vmb, eqNames));
    } else {
        dropEMatrix(session, "V_modelbased");
    }

    // eform: exp(b), exp(b) se and the exponentiated interval in the reported
    // table, z and p unchanged; e(b) is untouched and r(table) carries eform 1
    // (bank b7486d54 `glm hi beertax unemp, family(binomial) link(logit) eform`).
    bool eform = false;
    for (const auto& [k, v] : parsed.opts)
        if (k == "eform" && v.isBool && v.boolValue) eform = true;
    // level(#): the reported interval b -/+ invnormal(1 - (1 - level/100)/2) se
    // (bank 3f513b3e `glm ..., family(poisson) le(90)`: r(table) crit 1.6448536)
    if (levelPct.has_value() && block.contains("coeftable") && block["coeftable"].is_array()) {
        const double zc = cecon::z_critical(1.0 - *levelPct / 100.0);
        for (auto& row : block["coeftable"]) {
            if (!row.is_object() || !row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
                !row["se"].is_number() || !(row["se"].get<double>() > 0))
                continue;
            const double b = row["b"].get<double>(), se = row["se"].get<double>();
            row["ci_lo"] = b - zc * se;
            row["ci_hi"] = b + zc * se;
        }
    }
    if (eform && block.contains("coeftable") && block["coeftable"].is_array()) {
        for (auto& row : block["coeftable"]) {
            if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
            const double e = std::exp(row["b"].get<double>());
            row["b"] = e;
            if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
            if (row.contains("ci_lo") && row["ci_lo"].is_number()) row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
            if (row.contains("ci_hi") && row["ci_hi"].is_number()) row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
        }
        block["coef_metric"] = "eform";
    }
    blocks.push_back(block);
    if (!scaleMsg.empty()) blocks.push_back(displayBlock("(Standard errors scaled using " + scaleMsg + ".)"));
    if (!r.converged) blocks.push_back(displayBlock("Warning: Convergence not achieved."));
    return blocks;
}

// ===========================================================================
// margins after glm: average marginal effects on the predicted mean, with the
// delta-method variance through the model's own e(V) (cluster-robust when the
// fit was). Oracle arm gam_cl_margins: `margins, dydx(g) post` after the
// clustered gamma/log fit with i.(g) -- the factor effect is the discrete change
// from the base level, averaged over the estimation sample.

ojson runGlmMargins(const eval::ParsedCommand& parsed, SessionState& session) {
    const auto& lmOpt = session.lastModel();
    if (!lmOpt.has_value() || !lmOpt->result.has_value() ||
        !lmOpt->result->contains("glm_model")) {
        return errRc("last estimates not found", 301);
    }
    const LastModel lm = *lmOpt;
    const ojson& gm = (*lm.result)["glm_model"];
    const ModelSpec spec = specFromJson(gm);
    // margins differentiates the four links it was built on (glm_mean); the other
    // links, the binomial with a denominator and the inverse Gaussian keep a refusal
    if (gm.value("link", 0) > static_cast<int>(cecon::GlmLinkFn::Probit) || !spec.mVar.empty() ||
        spec.mConst != 1.0 || gm.value("family", 0) > static_cast<int>(cecon::GlmFamily::NegBinomial))
        return ojson(eval::refusedCommand("margins", "glm_margins_link"));
    bool interacted = false;
    for (const auto& t : spec.terms) interacted = interacted || !t.atoms.empty();
    bool hasDydx = false, hasAt = false, atmeans = false;
    for (const auto& [k, v] : parsed.opts) {
        if (k == "dydx" && !v.isBool && !trim(v.strValue).empty()) hasDydx = true;
        if (k == "at" && !v.isBool && !trim(v.strValue).empty()) hasAt = true;
        if (k == "atmeans") atmeans = true;
    }
    // margins-corpus-shapes: a model with an interaction, or dydx() at at()
    // points (F7c `dydx(tt) at(d=(-0.4(.2).4))` on the model without one), goes
    // through the margins grid (every column rebuilt at
    // each point, the response-scale discrete change, V through the fit's
    // e(V)); w2v_margins_corpus F7/F7nb/F7c. Otherwise at()/atmeans and a
    // marginlist keep the refusals below: on predictive margins the grid's
    // analytic Jacobian misses Stata's numerical one by up to ~1e-9, so
    // r(table)'s p-values and one r(V) miss T2 (w2v_margins_siblings
    // C8_gam_at_d_rt, C8_gam_g_rt, C8_gam_atmeans_v, C8_bin_g_rt,
    // C8_bin_at_c1_rt, C8_gamif_at_d0_rt; FU-643/FU-678 carry the class), and
    // `margins, dydx() [post]` keeps the landed path (w2_glm_lifecycle
    // gam_cl_margins).
    //
    // ME05: at(), atmeans and a marginlist go through the grid as well
    // (w2v_margins_siblings C8_gam_at_d, C8_gam_g, C8_gam_atmeans, C8_bin_g,
    // C8_bin_at_c1, C8_gamif_at_d0; log:2153 C8_gam_g_b
    // `2.525391298626589|2.532498120754016|2.433619571829875`). Stata's own
    // r(V) there is a numerical Jacobian, so the grid's analytic one agrees to
    // about 1e-9 relative -- a difference in the oracle's last digits, not a
    // different answer. `margins, dydx()` without at() keeps the landed path.
    const bool viaGrid = interacted || hasAt || atmeans || !parsed.varlist.empty();
    if (!viaGrid) {
        static const std::unordered_set<std::string> kLandedKnown = {"dydx", "post"};
        std::unordered_set<std::string> landedUnimplemented = kGlmMarginsUnimplemented;
        landedUnimplemented.insert("at");
        landedUnimplemented.insert("atmeans");
        if (auto e = eval::rejectUnknownOptions(parsed.opts, kLandedKnown, landedUnimplemented))
            return ojson(*e);
        if (!parsed.varlist.empty()) return ojson(eval::refusedCommand("margins", "glm_margins_marginlist"));
    } else if (auto e = eval::rejectUnknownOptions(parsed.opts, kGlmMarginsKnown, kGlmMarginsUnimplemented)) {
        return ojson(*e);
    }
    if ((parsed.ifExpr && !parsed.ifExpr->empty()) || (parsed.inRange && !parsed.inRange->empty()) ||
        parsed.weightType.has_value())
        return ojson(eval::refusedCommand("margins", "glm_margins_qualifier"));
    std::string dydxRaw;
    bool post = false;
    for (const auto& [k, v] : parsed.opts) {
        if (k == "dydx" && !v.isBool) dydxRaw = v.strValue;
        if (k == "post" && (!v.isBool || v.boolValue)) post = true;
    }
    if (viaGrid) {
        const auto famG = static_cast<cecon::GlmFamily>(gm.value("family", 0));
        FrameScope frameG = openFrame(session, lm.dataPath, spec, false);
        if (!frameG.err.is_null()) return frameG.err;
        Design dg;
        ojson errG;
        if (!buildDesign(*frameG.ds, spec, famG, dg, errG)) return errG;
        MarginsGridFit fit;
        fit.names = dg.names;
        fit.b = gm.value("b", std::vector<double>());
        fit.V = gm.value("V", std::vector<double>());
        if (fit.b.size() != dg.names.size() || fit.V.size() != dg.names.size() * dg.names.size())
            return errRc("margins: the glm estimates in memory do not match the fit-time data", 459);
        if (!dg.off.empty()) return ojson(eval::refusedCommand("margins", "glm_margins_offset"));
        fit.link = gm.value("link", 0);
        fit.n = dg.n;
        std::set<std::string> vars;
        for (const auto& t : spec.terms) {
            if (t.atoms.empty()) vars.insert(t.var);
            for (const auto& a : t.atoms) vars.insert(a.var);
        }
        for (const auto& v : vars) {
            std::vector<double> all;
            if (!readNumeric(*frameG.ds, v, all, errG)) return errG;
            std::vector<double>& col = fit.data[v];
            for (int64_t r : dg.rows) col.push_back(all[static_cast<size_t>(r)]);
        }
        fit.weights = dg.w;
        fit.cluster = !spec.clusterVar.empty();
        fit.depvar = spec.depvar;
        fit.dataPath = lm.dataPath;
        if (auto served = runMarginsGrid(session, fit, parsed.varlist, parsed.opts)) return *served;
        return ojson(eval::refusedCommand("margins", "glm_margins_form"));
    }
    // A bare `margins` is the predictive margin of the mean -- not covered.
    if (trim(dydxRaw).empty()) return ojson(eval::refusedCommand("margins", "glm_margins_predictive"));
    if (!spec.weightVar.empty()) return ojson(eval::refusedCommand("margins", "glm_margins_weighted"));
    const auto fam = static_cast<cecon::GlmFamily>(gm.value("family", 0));
    const auto link = static_cast<cecon::GlmLinkFn>(gm.value("link", 0));
    const std::vector<double> bp = gm.value("b", std::vector<double>());
    const std::vector<double> Vp = gm.value("V", std::vector<double>());

    FrameScope frame = openFrame(session, lm.dataPath, spec, false);
    if (!frame.err.is_null()) return frame.err;
    Design d;
    ojson err;
    if (!buildDesign(*frame.ds, spec, fam, d, err)) return err;
    const size_t K = d.names.size();
    if (bp.size() != K || Vp.size() != K * K)
        return errRc("margins: the glm estimates in memory do not match the fit-time data", 459);

    // Which terms were asked for (`*` / `_all` = every covariate).
    std::vector<size_t> wanted;
    for (const auto& tokRaw : splitWs(dydxRaw)) {
        std::string tok = tokRaw;
        if (tok.rfind("i.", 0) == 0 || tok.rfind("c.", 0) == 0) tok = tok.substr(2);
        if (tok == "*" || tok == "_all") {
            for (size_t t = 0; t < d.terms.size(); ++t) wanted.push_back(t);
            continue;
        }
        bool found = false;
        for (size_t t = 0; t < d.terms.size(); ++t) {
            if (d.terms[t].var == tok || (d.terms[t].var.size() > tok.size() &&
                                          d.terms[t].var.compare(0, tok.size(), tok) == 0)) {
                wanted.push_back(t);
                found = true;
                break;
            }
        }
        if (!found) return errRc("variable " + tok + " not found in list of covariates", 322);
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    // Posted-order design row for observation i.
    auto rowOf = [&](int64_t i, std::vector<double>& x) {
        x.assign(K, 0.0);
        for (size_t c = 0; c < K; ++c) {
            const int e = d.estIndex[c];
            if (e >= 0) x[c] = d.X[static_cast<size_t>(e) * d.n + i];
        }
    };
    auto etaOf = [&](const std::vector<double>& x, int64_t i) {
        double e = 0.0;
        for (size_t c = 0; c < K; ++c) e += x[c] * bp[c];
        return e + (d.off.empty() ? 0.0 : d.off[i]);
    };
    const double invN = 1.0 / static_cast<double>(d.n);

    std::vector<std::string> names;
    std::vector<double> est;
    std::vector<std::vector<double>> G;  // gradient rows (length K)
    std::vector<bool> isBase;
    std::vector<double> x, xl, xb;
    for (size_t t : wanted) {
        const Design::TermCols& tc = d.terms[t];
        if (!tc.factor) {
            const size_t j = static_cast<size_t>(tc.postedCols[0]);
            double s = 0.0;
            std::vector<double> g(K, 0.0);
            for (int64_t i = 0; i < d.n; ++i) {
                rowOf(i, x);
                const double e = etaOf(x, i);
                const double dm = cecon::glm_dmean(link, e), d2 = cecon::glm_d2mean(link, e);
                s += dm * bp[j];
                for (size_t c = 0; c < K; ++c) g[c] += d2 * bp[j] * x[c];
                g[j] += dm;
            }
            for (auto& v : g) v *= invN;
            names.push_back(tc.var);
            est.push_back(s * invN);
            G.push_back(g);
            isBase.push_back(false);
            continue;
        }
        const size_t baseCol = static_cast<size_t>(tc.postedCols[0]);
        for (size_t li = 0; li < tc.levels.size(); ++li) {
            const size_t col = static_cast<size_t>(tc.postedCols[li]);
            names.push_back(d.names[col]);
            if (li == 0) {
                est.push_back(0.0);
                G.push_back(std::vector<double>(K, 0.0));
                isBase.push_back(true);
                continue;
            }
            double s = 0.0;
            std::vector<double> g(K, 0.0);
            for (int64_t i = 0; i < d.n; ++i) {
                rowOf(i, x);
                xl = x;
                xb = x;
                for (int pc : tc.postedCols) {
                    xl[static_cast<size_t>(pc)] = 0.0;
                    xb[static_cast<size_t>(pc)] = 0.0;
                }
                xl[col] = 1.0;
                xb[baseCol] = 1.0;  // the base column's coefficient is zero
                const double el = etaOf(xl, i), eb = etaOf(xb, i);
                const double ml = cecon::glm_mean(link, el), mb = cecon::glm_mean(link, eb);
                const double dl = cecon::glm_dmean(link, el), db = cecon::glm_dmean(link, eb);
                s += ml - mb;
                for (size_t c = 0; c < K; ++c) g[c] += dl * xl[c] - db * xb[c];
            }
            for (auto& v : g) v *= invN;
            est.push_back(s * invN);
            G.push_back(g);
            isBase.push_back(false);
        }
    }
    const size_t M = names.size();
    std::vector<double> MV(M * M, 0.0);
    for (size_t a = 0; a < M; ++a)
        for (size_t b = 0; b < M; ++b) {
            double s = 0.0;
            for (size_t i = 0; i < K; ++i) {
                if (G[a][i] == 0.0) continue;
                double inner = 0.0;
                for (size_t j = 0; j < K; ++j) inner += Vp[i * K + j] * G[b][j];
                s += G[a][i] * inner;
            }
            MV[a * M + b] = s;
        }

    ojson blocks = ojson::array();
    {
        std::string wrt;
        for (size_t m = 0; m < M; ++m)
            if (!isBase[m]) wrt += (wrt.empty() ? "" : " ") + names[m];
        std::string s = "Average marginal effects";
        const std::string nobs = "Number of obs = " + std::to_string(d.n);
        s += std::string(s.size() + nobs.size() < 78 ? 78 - s.size() - nobs.size() : 1, ' ') + nobs;
        std::string text = s + "\n";
        text += std::string("Model VCE: ") + (spec.clusterVar.empty() ? "OIM" : "Robust") + "\n\n";
        text += "Expression: Predicted mean " + spec.depvar + ", predict()\n";
        text += "dy/dx wrt:  " + wrt;
        blocks.push_back(displayBlock(text));
    }
    ojson block;
    block["type"] = "estimation";
    block["estimator"] = "margins";
    block["n_obs"] = d.n;
    block["glm_margins"] = true;  // glm-owned e() (see dropStaleGlmEStores)
    ojson ct = ojson::array(), coefs = ojson::array();
    for (size_t m = 0; m < M; ++m) {
        if (isBase[m]) continue;
        ojson row = coefRow(names[m], est[m], std::sqrt(MV[m * M + m]));
        ct.push_back(row);
        coefs.push_back(ojson{{"name", names[m]}, {"dydx", est[m]}, {"se", row["se"]}});
    }
    block["coeftable"] = ct;
    blocks.push_back(block);
    bool anyFactor = false;
    for (size_t t : wanted) anyFactor = anyFactor || d.terms[t].factor;
    if (anyFactor)
        blocks.push_back(displayBlock(
            "Note: dy/dx for factor levels is the discrete change from the base level."));

    // marginsplot reads the last margins; publish this one so it cannot plot a
    // previous model's effects.
    session.lastMargins = ojson{{"model", "glm"}, {"n_obs", d.n}, {"coefs", coefs}};
    session.lastMarginsDydx.clear();

    if (post) {
        LastModel lm2;
        lm2.cobraCmd = "margins";
        lm2.stataCmd = "margins";
        lm2.depvar = lm.depvar;
        lm2.dataPath = lm.dataPath;
        lm2.result = block;
        session.setLastModel(lm2);
        session.lastEstBlock = block;
        session.estAddedClear();
        postEMatrix(session, "b", rowVector(est, names));
        postEMatrix(session, "V", square(MV, names));
        dropEMatrix(session, "ilog");
        dropEMatrix(session, "gradient");
        dropEMatrix(session, "Cns");
        dropEMatrix(session, "V_modelbased");
        // `post` replaces e() wholesale: the glm's own macros go with it.
        for (const char* k : kGlmEStrings) session.eval.setEString(k, "");
    }
    // margins is r-class: r(N)/r(b)/r(V)/r(table) of these effects, posted
    // after any `post` (Stata keeps them; w2v_margins_siblings C8_gamif_dydx_x
    // N=160, .1303959895000301, se .3815414001390036).
    {
        ojson tableRows = ojson::array();
        std::vector<std::vector<double>> Vrows(M, std::vector<double>(M, 0.0));
        for (size_t a = 0; a < M; ++a) {
            for (size_t c = 0; c < M; ++c) Vrows[a][c] = MV[a * M + c];
            if (isBase[a]) {
                tableRows.push_back(ojson{{"name", names[a]}, {"b", 0.0}, {"se", nullptr}, {"t", nullptr},
                                          {"p", nullptr}, {"ci_lo", nullptr}, {"ci_hi", nullptr}});
            } else {
                tableRows.push_back(coefRow(names[a], est[a], std::sqrt(MV[a * M + a])));
            }
        }
        std::string xvars;
        for (const auto& tokRaw : splitWs(dydxRaw)) {
            std::string tok = tokRaw;
            if (tok == "*" || tok == "_all") {
                xvars.clear();
                break;
            }
            if (tok.rfind("i.", 0) == 0 || tok.rfind("c.", 0) == 0) tok = tok.substr(2);
            xvars += (xvars.empty() ? "" : " ") + tok;
        }
        postGlmDydxRClass(session, block, names, est, Vrows, tableRows, xvars, !spec.clusterVar.empty(), &G);
    }
    return blocks;
}

// ===========================================================================
// Stale-e() guard (called by the dispatcher around every command).

std::optional<GlmEStores> glmOwnedEStores(const SessionState& session) {
    if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return std::nullopt;
    const ojson& blk = *session.lastEstBlock;
    if (!blk.contains("glm_model") && !blk.contains("glm_margins")) return std::nullopt;
    GlmEStores out;
    for (const char* nm : kGlmEMatrices)
        if (const eval::EvalMatrix* m = session.eval.findClassMatrix('e', nm)) out.matrices.emplace_back(nm, *m);
    for (const char* k : kGlmEStrings)
        if (const std::string* v = session.eval.eStrings.find(k)) out.strings.emplace_back(k, *v);
    return out;
}

void dropStaleGlmEStores(SessionState& session, const GlmEStores& before) {
    if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object() &&
        (session.lastEstBlock->contains("glm_model") || session.lastEstBlock->contains("glm_margins")))
        return;  // still a glm (or its posted margins, or a restored glm): nothing is stale
    for (const auto& [nm, was] : before.matrices) {
        const eval::EvalMatrix* now = session.eval.findClassMatrix('e', nm);
        if (now != nullptr && now->data == was.data && now->rownames == was.rownames &&
            now->colnames == was.colnames)
            dropEMatrix(session, nm);
    }
    for (const auto& [k, was] : before.strings) {
        const std::string* now = session.eval.eStrings.find(k);
        if (now != nullptr && *now == was) session.eval.setEString(k, "");
    }
}

}  // namespace piet::handlers
