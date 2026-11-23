// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include "expression_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/graph.h"
#include "piet/handlers/gtools_aliases.h"
#include "piet/handlers/session_commands.h"
namespace piet::dispatch_detail {
namespace {
// (command as dispatched, option key) pairs the command's Stata syntax does not
// take, each refused by StataBE 19.5 in the vs-stata truth bank of 2026-11-10
// with r(198) "option <key> not allowed", and typed on no bank line of that
// command that ran:
//   vcov()   -- PIET's internal synonym of vce(); refused on regress, poisson,
//               nbreg, fracreg, mixed, nl, xtreg, ivreg, ivreg2, ivregress,
//               ivprobit, ivtobit, xtivreg2, reghdfe, ivreghdfe, ppmlhdfe
//               (e.g. f60ad6..: `regress price mpg weight, vcov(robust)`).
//   robust   -- tobit, ivtobit, ivprobit, fracreg take vce(robust) only;
//               reghdfe likewise (ivreghdfe keeps its ivreg2-style robust).
//   cluster()-- fracreg (vce(cluster ...) only).
//   vce()    -- ivreg2, xtivreg2, ivreg take robust/cluster() only.
//   pairwise -- correlate/pwcorr have no such option.
//   sig      -- correlate (pwcorr's option; `corr price mpg weight rep78, sig`
//               r(198), 3 bank cells).
//   gmm, keepsingletons -- ivregress/ivreg (gmm is an estimator word, not an
//               option; keepsingletons is reghdfe's).
//   ll(), ul() -- ivprobit (ivtobit's censoring limits).
//   instruments(), onestep -- nl (gmm's options).
//   absorb() -- xtivreg2.
//   level()  -- did_imputation.
//   generate() -- carryforward's option is gen() exactly (`gen(r2)` rc 0,
//               `generate(r2)` r(198)).
//   fast     -- gisid.
//   asis     -- copy.
//   by()     -- tsspell.
//   eqlines  -- esttab.
//   paired   -- ttest (`var1 == var2` is the paired form; 0dc603b35571b219).
//   ascii    -- filefilter (674669eae767c344).
struct NotStataOption {
  const char* cmd;
  const char* key;
};
constexpr NotStataOption kNotStataOptions[] = {
    {"regress", "vcov"},     {"poisson", "vcov"},       {"nbreg", "vcov"},
    {"fracreg", "vcov"},     {"mixed", "vcov"},         {"nl", "vcov"},
    {"xtreg", "vcov"},       {"ivreg", "vcov"},         {"ivreg2", "vcov"},
    {"ivregress", "vcov"},   {"ivprobit", "vcov"},      {"ivtobit", "vcov"},
    {"xtivreg2", "vcov"},    {"reghdfe", "vcov"},       {"ivreghdfe", "vcov"},
    {"ppmlhdfe", "vcov"},
    {"tobit", "robust"},     {"ivtobit", "robust"},     {"ivprobit", "robust"},
    {"fracreg", "robust"},   {"reghdfe", "robust"},
    {"fracreg", "cluster"},
    {"ivreg2", "vce"},       {"xtivreg2", "vce"},       {"ivreg", "vce"},
    {"corr", "pairwise"},    {"pwcorr", "pairwise"},      {"corr", "sig"},
    {"ivregress", "gmm"},    {"ivreg", "gmm"},
    {"ivregress", "keepsingletons"}, {"ivreg", "keepsingletons"},
    {"ivprobit", "ll"},      {"ivprobit", "ul"},
    {"nl", "instruments"},   {"nl", "onestep"},
    {"xtivreg2", "absorb"},
    {"did_imputation", "level"},
    {"carryforward", "generate"},
    {"gisid", "fast"},
    {"copy", "asis"},
    {"tsspell", "by"},
    {"esttab", "eqlines"},
    {"ttest", "paired"},
    {"filefilter", "ascii"},
};
bool notStataOption(const std::string& cmd, const std::string& typed, const std::string& resolved) {
  for (const auto& row : kNotStataOptions)
    if (cmd == row.cmd && (typed == row.key || resolved == row.key)) return true;
  return false;
}

// Options whose Stata syntax fixes their form: `needsArgs` ones are declared
// with an argument (`Format(passthru)`, `Df(#)`, `Level(#)`, `Lags(numlist)`,
// `Parse(string)`, `PANELNAME(passthru)`) and typed bare are StataBE's r(198)
// "option <typed> incorrectly specified"; the others are flags, and typed with
// an argument are "option <typed>() incorrectly specified". A spelling is the
// option when it is a prefix of `full` at least `minLen` long (the syntax
// line's capitals) or PIET resolved it there. vs-stata bank 2026-11-10:
// `tset person day, fo` / `for` and `tsset t, panelname` (56cf6db34ee2a481,
// 03cd802b358bddc8, and their tsset/xtset twins), `lrtest A B, d`
// (fdf6a919cfebe3b3; `d(3)` runs), `oaxaca ..., l pooled` and `var close
// volume, l` (e8fba03546c95311, f8ba26845dfc2c8f), `split s, destring p
// gen(n)` (52af7b87ca26946c), `ivreghdfe ..., l` (5f1bf0cd9f0731fd; `l(90)`
// runs); esttab's Label flag with an argument, `l(" END")` / `la(..)` /
// `lab(..)` (e9dcff6d0a9db79f, 9e25be35eb1e7578, 55c8850ecf911d9e).
struct OptionForm {
  const char* cmd;
  const char* full;
  size_t minLen;
  bool needsArgs;
};
constexpr OptionForm kOptionForms[] = {
    {"tset", "format", 1, true},     {"tsset", "format", 1, true},
    {"xtset", "format", 1, true},    {"tset", "panelname", 9, true},
    {"tsset", "panelname", 9, true}, {"xtset", "panelname", 9, true},
    {"lrtest", "df", 1, true},       {"oaxaca", "level", 1, true},
    {"var", "lags", 1, true},        {"split", "parse", 1, true},
    {"ivreghdfe", "level", 1, true}, {"esttab", "label", 1, false},
};
bool wrongOptionForm(const std::string& cmd, const std::string& typed, const std::string& resolved,
                     bool bare) {
  for (const auto& row : kOptionForms) {
    if (cmd != row.cmd || bare != row.needsArgs) continue;
    const std::string full(row.full);
    if (resolved == full ||
        (typed.size() >= row.minLen && typed.size() <= full.size() &&
         full.compare(0, typed.size(), typed) == 0))
      return true;
  }
  return false;
}

// Commands that pass every option through their Stata syntax line: an option
// that is no spelling of anything that line (with the ml maximize options and
// the estimation display options its `*` hands to mlopts / _get_diopts) takes
// is StataBE's r(198) "option X not allowed" (vs-stata bank 2026-11-10:
// `mlogit race age grade wage, zznotanoption` c8ea95fe43d320da, the ologit,
// oprobit, stcox, streg, stset, var, xtabond, tsfilter and pca twins), where
// PIET ran the model without it. A typed key is let through when it is a
// prefix of one of the names (or of its `no` form, or the name of its `no`
// form): minimum abbreviations are the spelling tables' business, this net only
// refuses words the syntax cannot mean.
constexpr const char* kMlMaximizeOpts[] = {
    "mlopts", "trace", "tracediffs", "gradient", "hessian", "showstep", "iterate", "tolerance",
    "ltolerance", "gtolerance", "nonrtolerance", "shownrtolerance", "showtolerance",
    "nrtolerance", "qtolerance", "difficult", "halfsteponly", "constraints", "nocnsnotes",
    "collinear", "technique", "vce", "dots", "depsilon", "showh", "notconcave", "useviews",
    "nolog", "log", "nrderiv", "from"};
constexpr const char* kDisplayOpts[] = {
    "level", "vsquish", "noallbaselevels", "allbaselevels", "nobaselevels", "baselevels",
    "nocnsreport", "fullcnsreport", "noemptycells", "emptycells", "noomitted", "omitted",
    "nolstretch", "lstretch", "coeflegend", "selegend", "cformat", "sformat", "pformat",
    "coding", "versus", "compare", "fvwrap", "fvwrapon", "nofvlabel", "fvlabel", "noci",
    "nopvalues", "markdown", "citype", "noabbrev", "abbrev"};
struct StataSyntaxVocab {
  const char* cmd;
  bool ml;       // `*` reaches mlopts
  bool display;  // `*` reaches _get_diopts
  std::vector<const char*> own;
};
const StataSyntaxVocab kStataSyntaxVocabs[] = {
    {"mlogit", true, true,
     {"from", "nolog", "log", "noconstant", "constraints", "technique", "vce", "ltolerance",
      "tolerance", "nowarning", "robust", "cluster", "crittype", "score", "doopt", "level",
      "notable", "noheader", "rrr", "baseoutcome", "moptobj"}},
    {"ologit", true, true,
     {"from", "nolog", "log", "offset", "technique", "vce", "ltolerance", "tolerance",
      "nowarning", "robust", "cluster", "crittype", "score", "doopt", "notable", "noheader",
      "nocoef", "or", "moptobj"}},
    {"oprobit", true, true,
     {"from", "nolog", "log", "offset", "technique", "vce", "ltolerance", "tolerance",
      "nowarning", "robust", "cluster", "crittype", "score", "doopt", "notable", "noheader",
      "nocoef", "moptobj"}},
    {"stcox", true, true,
     {"frailty", "shared", "estimate", "cluster", "cmd", "nohr", "level", "robust", "noshow",
      "nolog", "log", "breslow", "efron", "exactm", "exactp", "basehazard", "basehc",
      "basechazard", "basesurv", "mgale", "esr", "schoenfeld", "scaledsch", "tvc", "texp",
      "altvce", "vce", "strata", "offset", "noadjust", "noheader", "notable", "nocoef"}},
    {"streg", true, true,
     {"cluster", "cmd", "level", "distribution", "time", "frailty", "shared", "noheader",
      "robust", "tratio", "nohr", "noshow", "score", "nocoef", "strata", "ancillary", "anc2",
      "noconstant", "vce", "forceshared", "sttewt", "moptobj", "offset", "nolrtest", "notable"}},
    {"stset", false, false,
     {"clear", "noshow", "mi", "noheader", "after", "before", "enter", "entry", "exit", "ever",
      "failure", "id", "if", "ifopt", "never", "origin", "scale", "time0"}},
    {"var", false, true,
     {"lags", "exog", "constraints", "noconstant", "dfk", "lutstats", "nobigf", "level", "small",
      "noisure", "iterate", "tolerance", "nocnsreport", "nolog", "log", "vce", "nodisplay",
      "zlags", "nomodel"}},
    {"xtabond", false, true,
     {"noconstant", "diffvars", "inst", "lags", "maxldep", "maxlags", "twostep", "pre",
      "endogenous", "vce", "level", "artests", "small", "robust", "xtabond"}},
    {"tsfilter", false, false,
     {"smooth", "trend", "stub", "gain", "minperiod", "maxperiod", "smaorder", "stationary",
      "drift", "order", "difference", "maorder"}},
    {"pca", false, false,
     {"novce", "vce", "correlation", "covariance", "means", "blanks", "level", "n", "names",
      "shape", "sds", "force", "nodisplay", "components", "factors", "mineigen", "ignore", "tol",
      "forcepsd"}},
};
bool notInStataSyntax(const std::string& cmd, const std::string& typedIn) {
  std::string typed = typedIn;
  for (char& ch : typed)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  if (typed.empty()) return false;
  auto spells = [&](const std::string& full) {
    auto pre = [&](const std::string& t, const std::string& f) {
      return !t.empty() && t.size() <= f.size() && f.compare(0, t.size(), t) == 0;
    };
    if (pre(typed, full) || pre(typed, "no" + full)) return true;
    return full.size() > 2 && full.compare(0, 2, "no") == 0 && pre(typed, full.substr(2));
  };
  for (const auto& row : kStataSyntaxVocabs) {
    if (cmd != row.cmd) continue;
    for (const char* o : row.own)
      if (spells(o)) return false;
    if (row.ml)
      for (const char* o : kMlMaximizeOpts)
        if (spells(o)) return false;
    if (row.display)
      for (const char* o : kDisplayOpts)
        if (spells(o)) return false;
    return true;
  }
  return false;
}

// The two nets above refuse an option that the command's syntax hands to `*`
// or to a later parser, so Stata reads the varlist first: a name that is no
// variable is r(111) "variable X not found" before the option is judged
// (`mlogit rep78 zzvar, zzopt`, `tobit mpg zzvar, ll(17) robust`, `corr price
// zzvar, pairwise` -- the order the varlist-first step in checkStataLead
// keeps for its commands). Only the commands whose words before the comma are
// variables (fracreg and ivregress after their model word) are looked up, and only plain
// names (an abbreviation that is unique counts); operators, wildcards and
// parentheses stay the arm's. Returns the r(111) block, if any.
std::optional<ojson> unknownVariableBeforeOption(const std::string& cmd,
                                                 const eval::ParsedCommand& parsed,
                                                 SessionState& session) {
  static const std::set<std::string> kVarlistLead = {
      "regress",  "poisson",   "nbreg",     "fracreg",   "xtreg",      "ivreg",
      "ivregress", "ivprobit", "ivtobit",   "xtivreg2",  "reghdfe",    "ivreghdfe",
      "ppmlhdfe", "tobit",     "corr",      "pwcorr",    "ivreg2",     "did_imputation",
      "carryforward", "gisid", "tsspell",   "ttest",     "mlogit",     "ologit",
      "oprobit",  "stcox",     "streg",     "stset",     "var",        "xtabond",
      "pca"};
  if (kVarlistLead.count(cmd) == 0 || !session.hasData()) return std::nullopt;
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  std::vector<std::string> names;
  const bool modelWord = cmd == "fracreg" || cmd == "ivregress";  // `fracreg logit`, `ivregress liml`
  for (size_t i = modelWord ? 1 : 0; i < parsed.varlist.size(); ++i)
    if (std::regex_match(parsed.varlist[i], rePlain)) names.push_back(parsed.varlist[i]);
  if (names.empty()) return std::nullopt;
  return loops::checkKnownVarlist(names, session);
}
}  // namespace

bool resolveCommandOptions(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // 1a-ter. BUG-105 DoD ③ / FIX:OPTNET-ABBREV-SWEEP — THE OPTION-NAME
  // RESOLUTION POINT, the sibling of the command one above.
  //
  // Implements internal note BUG105-OPTABBREV-AUDIT-20260731.md §2's nine measured
  // minimums. Unlike command names, OPTIONS really do follow a prefix rule in
  // Stata: each option documents its own minimum via `{opt l:evel(#)}` markup
  // and every prefix at-or-longer-than that minimum is accepted. So the table
  // below is (option, minimum length) and every prefix in that range is
  // generated — which is also what makes it a FIX and not just a widening:
  //
  // BUG-231 (measured this stint, not predicted by the audit): eval::
  // rejectUnknownOptions already resolves a >=2-char unique prefix against the
  // net's `known` set, so an abbreviated option ALREADY PASSED the gate — while
  // the handler's own `k == "offset"` comparisons still saw the raw key and
  // skipped it. The option was accepted and then silently dropped:
  //   `poisson fatal beertax, offset(pop)` -> b = -8861267.42121
  //   `poisson fatal beertax, offs(pop)`   -> b =        6.78117  (== no option)
  //   `poisson fatal beertax, exp(pop)`    -> runs; `exposure(pop)` rejects
  // A wrong number behind a right-looking command, with no diagnostic. Rewriting
  // the key HERE — before any handler compares it — is what closes that, because
  // it is the only point that is ordering-correct for every net regardless of
  // where in the handler its reject net happens to sit.
  //
  // AER scope-2 A1: the three passes themselves now live in ONE function,
  // resolveOptionKeyForCommand() (this file, immediately above dispatchImpl),
  // so `piet_native --dump-optvocab --resolve` can report the resolution this
  // loop performs instead of carrying a second copy of the rule. Nothing about
  // the rule moved with it: each pass was already independent per key, so one
  // per-key call produces the same rewrite the two whole-vector passes did.
  std::string bankSub;
  if (!parsed.varlist.empty()) {
    bankSub = parsed.varlist.front();
    if (!bankSub.empty() && bankSub.front() == '(') bankSub.erase(0, 1);
    for (char& ch : bankSub) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  // `twoway lfit y x, r(10 40)`: the options after a bare single plot belong to that plot, and the fit
  // plots' own class syntax reads r() / l() as range() / level() before the global vocabulary can
  // call them recast() / lstyle(). Several plots (parentheses or ||) leave the options global.
  // `graph twoway lfit ...` / `gr tw lfit ...` is the same statement: the plot type is the word after the
  // (abbreviated) twoway word.
  std::string plotWord = bankSub;
  bool twowayForm = cmd == "twoway";
  if (cmd == "graph" && parsed.varlist.size() >= 2 && bankSub.size() >= 2 &&
      std::string("twoway").compare(0, bankSub.size(), bankSub) == 0) {
    twowayForm = true;
    plotWord = parsed.varlist[1];
    if (!plotWord.empty() && plotWord.front() == '(') plotWord.erase(0, 1);
  }
  const bool singlePlot = twowayForm && !plotWord.empty() && parsed.argText.find("||") == std::string::npos &&
                          parsed.argText.find('(') == std::string::npos;
  for (auto& kv : parsed.opts) {
    if (singlePlot) {
      const std::string own = handlers::plotTypeOwnSpelling(plotWord, kv.first);
      if (!own.empty()) kv.first = own;
    }
    const OptKeyResolution optRes =
        resolveOptionKeyForCommand(cmd, kv.first, kv.second.isBool ? 1 : 2, bankSub);
    // list's `N` is case-sensitive and the key arrives lower-cased: the bank's
    // refusal is of a typed lower-case `n` (`list ..., n` r(198)), while
    // `list ..., mean sum N` runs (bank 3227e56bff29bd73). parseListLayout
    // re-reads the spelling typed.
    const bool listCapitalN =
        kv.first == "n" && (cmd == "list" || (cmd == "duplicates" && (bankSub == "list" ||
                                                                      bankSub == "examples"))) &&
        std::regex_search(parsed.raw, std::regex(R"([\s,]N(\(|\s|$))"));
    if (std::strcmp(optRes.stage, "bank-refuse") == 0 && !listCapitalN) {
      // StataBE refused this exact spelling on this command (truth bank,
      // 2026-11-10). Answered here, before any handler, because a handler that
      // parses its own options never reaches a reject net and would run it.
      // predict answers in its own words, Stata's unquoted r(198) "option x
      // not allowed" (BUG-1257: `predict p, x` / `, st` after logit).
      if (cmd == "predict") {
        blocks.push_back(ojson(eval::errorBlock(
            "syntax_error", "option " + kv.first + (kv.second.isBool ? "" : "()") + " not allowed",
            std::nullopt, std::nullopt, std::nullopt, "r(198)")));
        return true;
      }
      blocks.push_back(ojson(
          eval::engineError("option `" + kv.first + "` not allowed", "unsupported_option")));
      return true;
    }
    // Keys PIET's shared vocabularies carry for a command (an internal synonym
    // such as `vcov`, or an option of a sibling command) that the command's own
    // Stata syntax does not have: StataBE 19.5 answers r(198) "option X not
    // allowed" for the key in any spelling that reaches it, and no bank line
    // that types it on the command runs (vs-stata bank 2026-11-10; each row
    // names its refusal). The typed key is matched as well as the resolved
    // one, for a spelling PIET maps onto a key Stata knows by another name.
    if (notStataOption(cmd, kv.first, optRes.resolved)) {
      if (auto bad = unknownVariableBeforeOption(cmd, parsed, session)) {
        blocks.push_back(*bad);
        return true;
      }
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "option " + kv.first + (kv.second.isBool ? "" : "()") + " not allowed",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // An empty lead is a replay (estimation) or the command's own usage
    // refusal: Stata answers those before it reads an option (`mlogit ,
    // zzabbrprobe` with no estimates is r(301)), so the net waits for a lead.
    if (!parsed.varlist.empty() && notInStataSyntax(cmd, kv.first)) {
      if (auto bad = unknownVariableBeforeOption(cmd, parsed, session)) {
        blocks.push_back(*bad);
        return true;
      }
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error", "option " + kv.first + (kv.second.isBool ? "" : "()") + " not allowed",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (wrongOptionForm(cmd, kv.first, optRes.resolved, kv.second.isBool)) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error",
          "option " + kv.first + (kv.second.isBool ? "" : "()") + " incorrectly specified",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    kv.first = optRes.resolved;
    if (optRes.exactOnly) kv.second.exactOnly = true;
  }
  return false;
}


bool checkCommunityCommand(std::string& cmdTyped,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // 1a-bis. WS-B (extcompat, 2026-08-06) — THE community-name gate. One
  // point, immediately after name resolution, so every route into a command
  // passes it exactly once: a `capture`/`quietly`/`noisily` prefix strips and
  // re-enters dispatch() (Pre-0 above), and `bysort G: reghdfe ...` recurses
  // through dispatch() with the sub-command, so both are covered here without
  // a second check anywhere.
  //
  // ORDER MATTERS for gate ②. The setting is checked FIRST — one relaxed
  // atomic load — and the registry lookup happens only in the off case, which
  // is not the default. On the default path (and therefore in every gate,
  // wave, freeze and answerbook run) this costs one predictable branch per
  // line and touches no table.
  //
  // The message is a REJECT, not a notice: it is only ever produced on the
  // failure path, so no successful command's output can carry an attribution
  // string (LEGAL-DISTRIBUTION §2.7(c) — the display surface is what the
  // answerbook gates byte-compare). Code `unsupported_command` maps to Stata
  // r(199) via eval::coarseRc, so `capture reghdfe ...` sets _rc = 199.
  {
    dprof::Scope __z(dprof::kNameGate);
    if (!settings::extcompatEnabled()) {
      // BUG-105/WS-K: this gate reads cmdTyped, not cmd. resolveCanonical()
      // already exempts community names so the two are equal here today, but
      // the gate's question is literally "did the user type a community name",
      // and it must stay answerable from the typed token alone.
      if (const registry::CommunityName* cn = registry::findCommunityName(cmdTyped)) {
        blocks.push_back(ojson(eval::errorBlock("unsupported_command",
                                                registry::communityGateMessage(*cn), cmdTyped)));
        return true;
      }
    }
  }
  return false;
}


bool normalizeGtoolsAlias(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // AER-SCOPE1 bundle C: gtools/ftools translation. Placed AFTER the extcompat
  // community gate (so `set extcompat off` still refuses these names) and
  // BEFORE the by/bysort interception and the no-data guard (so the rewritten
  // head is what those see -- `gegen` must be `egen` by the time the by-prefix
  // branch compares sub-command names, and by the time kNoDataCommands is
  // consulted). One step for ten commands, rather than ten reimplementations.
  if (handlers::isGtoolsAlias(cmd)) {
    handlers::GtoolsTranslation tr = handlers::translateGtoolsAlias(parsed, cmd, session);
    if (!tr.error.empty()) {
      blocks.push_back(tr.error);
      return true;
    }
    parsed = tr.parsed;
    cmd = tr.canonical;  // cmdTyped keeps the user's spelling, for messages
  }
  return false;
}


bool countWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {

  // BUG-786: with no data in memory Stata's `count if` counts zero observations
  // -- `clear` / `count if e(sample)` is 0 at rc 0 (w2_esample.log:1290 E15
  // |0|0|28|). PIET refused it with r(111) "no dataset". Only a condition that
  // names no variable is served (a constant, or e(sample) already rewritten to
  // `0` above); `count if x` still has no `x` to find, and keeps the refusal
  // (Stata: r(111) variable not found). BUG-1104: a BARE `count` is the same
  // zero (StataBE `clear all` / `count` prints 0, r(N) 0).
  if (!session.hasData() && cmd == "count" && parsed.varlist.empty() && parsed.opts.empty() &&
      !parsed.inRange.has_value()) {
    // A Unicode variable name (`count if قيمة > 2`) names a variable too.
    static const std::regex reIdentChar(R"([A-Za-z_]|[^\x00-\x7F])");
    if (!parsed.ifExpr.has_value() || !std::regex_search(*parsed.ifExpr, reIdentChar)) {
      session.eval.rValues.set("N", 0.0);
      eval::declareRPosted(session.eval, "count", {"N"});  // count's help
      ojson out;
      out["type"] = "display";
      out["text"] = "0";
      blocks.push_back(out);
      return true;
    }
  }
  return false;
}


bool assertWithoutData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {

  // With no data in memory Stata's `assert` evaluates its claim once, as a
  // constant: `clear` / `assert 1==0` is r(9), `assert 601 == 601` rc 0, and a
  // claim naming a variable is r(111) (StataBE 19.5, checked 2026-10-21). PIET
  // refused every form as "no dataset", which stopped probe_b4 after a failed
  // `import excel ..., clear` had (correctly) emptied the data.
  if (!session.hasData() && cmd == "assert" && parsed.opts.empty() &&
      !parsed.inRange.has_value() && !parsed.ifExpr.has_value() && !parsed.varlist.empty()) {
    const std::string claim = genExprText(parsed);  // BUG-1433: a literal keeps its blanks
    std::string raised;
    bool isStr = false;
    auto v = eval::evalArith(claim, &session.eval, &raised, &isStr);
    if (v && !isStr) {
      if (*v == "0")
        blocks.push_back(ojson(eval::errorBlock("engine_error", "assertion is false", std::nullopt,
                                                std::nullopt, std::nullopt, "r(9)")));
      return true;
    }
  }
  return false;
}


bool parseEmptySave(std::string& cmd,
    eval::ParsedCommand& parsed,
    ojson& blocks,
    bool& saveEmptyOk) {

  if (cmd == "save" || cmd == "sa") {
    for (const auto& kv : parsed.opts)
      if (kv.first.size() >= 5 && std::string("emptyok").compare(0, kv.first.size(), kv.first) == 0)
        saveEmptyOk = true;
  }
  return false;
}


bool allowEmptyGenerate(std::string& cmd,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk) {

  if (!session.hasData() && !needsNoData &&
      (cmd == "generate" || cmd == "gen" || cmd == "g" || saveEmptyOk)) {
    ojson so = ojson(handlers::runSetObs("0", session));
    if (so.is_object() && so.value("type", std::string()) == "error") {
      blocks.push_back(std::move(so));
      return true;
    }
  }
  return false;
}


bool validateEmptyDataset(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData,
    bool& saveEmptyOk) {

  // BUG-1458: with nothing in memory Stata holds an empty dataset, not none.
  // StataBE 19.5, 2026-11-03, after `clear`: `summarize`, `summarize, detail`,
  // `summarize if 1` and `list` are rc 0 and print nothing; `summarize x` and
  // `list x` are r(111) "no variables defined"; `summarize in 1` and `list in
  // 1` are r(198) "observation numbers out of range"; `save f` without
  // emptyok is r(111) "no variables defined". An `if` is read but meets no
  // observation: `summarize if _n==1` and `list if missing(.)` are rc 0, while
  // `if nosuch==1` is r(111) "nosuch not found", `if (` r(132) and `if "a"`
  // r(109), as any expression fails.
  if (!session.hasData() && !needsNoData) {
    auto refuse = [&](const std::string& msg, const std::string& rc) {
      blocks.push_back(ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                              std::nullopt, rc)));
      return blocks;
    };
    if (cmd == "summarize" || cmd == "list") {
      if (!parsed.varlist.empty()) { blocks = refuse("no variables defined", "r(111)"); return true; }
      if (parsed.inRange.has_value()) { blocks = refuse("observation numbers out of range", "r(198)"); return true; }
      if (!parsed.ifExpr.has_value()) return true;
      // `_n` and `_N` are 0 here: no observation is there to number.
      static const std::regex reObsSpecial(R"(\b_[nN]\b)");
      const std::string cond = std::regex_replace(*parsed.ifExpr, reObsSpecial, "0");
      std::string raised;
      bool isStr = false;
      if (std::optional<std::string> v = eval::evalArith(cond, &session.eval, &raised, &isStr)) {
        if (isStr) { blocks = refuse("type mismatch", "r(109)"); return true; }
        return true;
      }
      eval::MacroFnError fe;
      if (eval::classifyFailedExpr(cond, raised, &fe) && fe.failed())
        { blocks = refuse(fe.message, "r(" + fe.rc + ")"); return true; }
      if (std::optional<std::string> name = firstBareName(cond))
        { blocks = refuse(*name + " not found", "r(111)"); return true; }
      { blocks = refuse("invalid syntax", "r(198)"); return true; }
    }
    if (cmd == "saveold") {
      // Options are read before the dataset is looked at: `saveold a.dta,
      // emptyok` is r(198) "option emptyok not allowed" over an empty dataset
      // too (bank 98001ce25427fdef).
      static const std::unordered_set<std::string> kKnown = {"replace", "version", "nolabel"};
      static const std::unordered_set<std::string> kNamedGap = {"all"};
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kNamedGap)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (cmd == "save" && !saveEmptyOk) {
      // A name-less `save, replace` with no file behind the dataset is r(198)
      // "invalid file specification", said before the empty dataset is (bank
      // 566e23f7ee9f5797).
      size_t at = parsed.raw.find_first_not_of(" \t");
      at = parsed.raw.find_first_of(" \t,", at == std::string::npos ? 0 : at);
      at = at == std::string::npos ? parsed.raw.size() : parsed.raw.find_first_not_of(" \t", at);
      const bool noName = at == std::string::npos || parsed.raw[at] == ',';
      if (noName && session.currentSaveTarget().empty()) {
        blocks = refuse("invalid file specification", "r(198)");
        return true;
      }
      blocks = refuse("no variables defined", "r(111)");
      return true;
    }
    // StataBE 19.5 on the empty dataset (after `clear`, or a k=0 N=0 file):
    // `count in 1` / `count in f/l` r(198) "observation numbers out of range"
    // (vs-stata bank 93b1ba934706c88f, 0b79ea5283a757ff); a bare `codebook`
    // (any options) and a bare `duplicates report` r(2000) "no observations"
    // (302ade60041764f7, c87cfb41c4baeb37, ee286ae99eefa337); a bare
    // `correlate` and a bare `export delimited` r(102) "too few variables
    // specified" -- their varlist defaults to _all, which is empty
    // (bb20edec81143228, e11ac529a4c5164b). A typed varlist keeps the
    // variable-not-found answer below.
    if (cmd == "count" && parsed.inRange.has_value()) {
      blocks = refuse("observation numbers out of range", "r(198)");
      return true;
    }
    if (cmd == "codebook" && parsed.varlist.empty()) {
      blocks = refuse("no observations", "r(2000)");
      return true;
    }
    if (cmd == "duplicates" && parsed.varlist.size() == 1 && lower(parsed.varlist[0]) == "report") {
      blocks = refuse("no observations", "r(2000)");
      return true;
    }
    if ((cmd == "corr" && parsed.varlist.empty()) ||
        (cmd == "export" && parsed.varlist.size() >= 2 && lower(parsed.varlist[0]) == "delimited" &&
         lower(parsed.varlist[1]) == "using")) {
      blocks = refuse("too few variables specified", "r(102)");
      return true;
    }
    // StataBE 19.5 after `clear` (with or without `set obs`): a bare `centile`
    // is rc 0 and prints its empty table header; `centile x` is r(111)
    // (measured on StataBE, D4-D6). runCentile answers both.
    if (cmd == "centile") {
      blocks.push_back(ojson(handlers::runCentile(parsed, session)));
      return true;
    }
  }
  return false;
}


bool requireCommandData(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    bool needsNoData) {

  if (!session.hasData() && !needsNoData) {
    // BUG-1437: a name the command table has no row for is Stata's r(199)
    // "unrecognized" whatever is in memory (StataBE `clear all` / `nosuchcmd`, or
    // a program `program drop _all` removed), which the last arm of dispatchImpl
    // answers once data exist. Every arm below this guard is a registry row
    // (beep and window, the two that are not, are in kNoDataCommands).
    if (registry::findByName(cmd) == nullptr) {
      blocks.push_back(ojson(eval::unsupportedCommand(
          parsed.cmd, std::nullopt, std::nullopt, "piet_gap",
          eval::UnsupportedCommandKind::kUnknownToken)));
      return true;
    }
    // BUG-1248: a postestimation verb reads e() before any data. With nothing in
    // memory and no estimates StataBE answers r(301) "last estimates not found"
    // (test, lincom, nlcom, margins, predict, every estat, lassocoef; esttab's
    // "last estimates (.) not found") and boottest its rc-0 "No estimates
    // detected"; each arm answers exactly that first, so with no e() live they
    // go on to it.
    static const std::set<std::string> kEstimatesFirst = {
        "test",  "lincom",   "nlcom",     "margins", "predict",
        "estat", "boottest", "lassocoef", "esttab"};
    if (kEstimatesFirst.count(cmd) == 0 || session.eval.eLive()) {
      blocks.push_back(ojson(eval::noDataLoaded()));
      return true;
    }
  }
  return false;
}


bool checkCommandArity(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // 2b. BUG-010 residue — generalized trailing-token guard. Most handler
  // arms below read only the specific tokens they expect off
  // parsed.varlist and silently ignore anything left over, e.g.
  // `count gen zzz9 = 1` tokenizes to cmd="count",
  // varlist=["gen","zzz9","=","1"]; runCount only ever consults
  // parsed.ifExpr, so the entire "gen zzz9 = 1" tail vanishes and the line
  // silently degrades to a bare `count` (960, not the intended `gen`).
  // Real Stata rejects this at parse time. xtset/tsset already got
  // bespoke arm-local guards for this (see above); rather than writing N
  // more bespoke blocks (or a full parser rewrite that would validate
  // every arm's grammar), this is one small allow-list of the commands
  // whose OWN Stata grammar takes zero or one fixed token — so ANY
  // leftover varlist token is unambiguously a mistake, not a legitimate
  // variable name. Kept deliberately narrow (the "worst offenders" swept
  // from the M3a/M3b arms above): commands with genuinely variable-arity
  // varlists (describe, summarize, regress, list, sort, ...) are correctly
  // left alone since a trailing token there IS the next variable name.
  {
    struct FixedArity {
      size_t maxTokens;  // max allowed parsed.varlist.size() for this cmd
      const char* usage;
    };
    static const std::map<std::string, FixedArity> kFixedArityCommands = {
        {"count", {0, "count [if] [in] — takes no variable list"}},
        {"pwd", {0, "pwd — takes no arguments"}},
        {"clear", {1, "clear [all]"}},
        {"preserve", {0, "preserve — takes no arguments"}},
        {"restore", {0, "restore — takes no arguments"}},
        {"sysuse", {1, "sysuse DATASET | sysuse dir"}},
    };
    auto arityIt = kFixedArityCommands.find(cmd);
    if (arityIt != kFixedArityCommands.end() && parsed.varlist.size() > arityIt->second.maxTokens) {
      std::vector<std::string> extra(
          parsed.varlist.begin() + static_cast<long>(arityIt->second.maxTokens), parsed.varlist.end());
      // count, preserve and restore take no varlist at all: a word in that
      // position is Stata's r(101) "varlist not allowed" (StataBE 19.5: `count
      // price`, `preserve zzarg`, `restore zzarg`). A qualifier-shaped word --
      // `if`/`in`, or one glued to its parenthesis (`in(1/5)`) -- is not a
      // varlist and keeps the answer below (`count in 1/5 in 2/3` is r(198)).
      const std::string& w = extra.front();
      const bool qualifierShaped = w == "in" || w == "if" || w.rfind("in(", 0) == 0 ||
                                   w.rfind("if(", 0) == 0;
      const bool noVarlistSyntax = cmd == "count" || cmd == "preserve" || cmd == "restore";
      if (noVarlistSyntax && !qualifierShaped) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "varlist not allowed", std::nullopt,
                                                std::nullopt, std::nullopt, "r(101)")));
        return true;
      }
      blocks.push_back(ojson(eval::engineError(
          cmdTyped + ": unexpected extra tokens after `" + arityIt->second.usage +
              "`: " + joinVarlist(extra) + ". Put each command on its own line.",
          "syntax_error")));
      return true;
    }
  }
  return false;
}


void stampStringVarlistRc(const std::string& cmd, const eval::ParsedCommand& parsed,
                          ojson& blocks) {
  // A string variable where an estimator needs a number. StataBE 19.5 (vs-stata
  // bank 2026-11-10): regress, ologit, oprobit and tobit treat it as missing and
  // run out of observations, r(2000) "no observations" (`regress price make`,
  // `regress make mpg`, `regress price c.make`, `ologit make mpg weight`, `tobit`
  // twin; 2000 on every such cell); as a factor variable it is r(109) "string
  // variables may not be used as factor variables" (`regress price i.make`,
  // 5e10c2bc828fde40). reghdfe, mean, pca and proportion check the types first:
  // r(109) (`reghdfe make mpg`, `mean make`, `pca make price mpg`, `proportion
  // s`). PIET's arms refuse with the engine's [factor_misuse] text and no code.
  static const std::map<std::string, int> kRc = {
      {"regress", 2000}, {"ologit", 2000}, {"oprobit", 2000}, {"tobit", 2000},
      {"reghdfe", 109},  {"mean", 109},    {"pca", 109},      {"proportion", 109}};
  if (!blocks.is_array()) return;
  // summarize with a negative aweight or fweight: r(402) "negative weights
  // encountered" (vs-stata bank: `summarize price [aw=-weight]` and the
  // aw/fw twins, 402 on every such cell), where the engine's text carried no
  // code.
  if (cmd == "summarize" || cmd == "sum" || cmd == "su") {
    for (auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      const std::string msg = b.value("message", std::string());
      if (msg.find("has a negative value") == std::string::npos ||
          (msg.find("aweight") == std::string::npos && msg.find("fweight") == std::string::npos))
        continue;
      if (b.value("reason", std::string()).rfind("r(", 0) == 0) continue;
      b["reason"] = "r(402)";
      b["message"] = "negative weights encountered";
    }
    // A string variable as the weight is r(109) "type mismatch" (vs-stata bank
    // 0a800c4fdcce9673 `summarize price [aw=make]`); the engine names the
    // column it could not read as a number.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
      const std::string want = "column '" + *parsed.weightVar + "' is a factor, not f64";
      for (auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "error") continue;
        if (b.value("message", std::string()).find(want) == std::string::npos) continue;
        b["reason"] = "r(109)";
        b["message"] = "type mismatch";
      }
    }
    return;
  }
  // nbreg with a negative outcome: r(459) "Y must be greater than or equal to
  // zero" (vs-stata bank, `nbreg nf beertax unemp` with nf = -fatal); the
  // engine refuses it with its own text.
  if (cmd == "nbreg") {
    for (auto& b : blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      const std::string msg = b.value("message", std::string());
      const size_t at = msg.find("nbreg: outcome '");
      if (at == std::string::npos || msg.find("has negative values") == std::string::npos) continue;
      if (b.value("reason", std::string()).rfind("r(", 0) == 0) continue;
      const size_t q1 = at + 16, q2 = msg.find('\'', q1);
      b["reason"] = "r(459)";
      b["message"] = (q2 == std::string::npos ? std::string("outcome") : msg.substr(q1, q2 - q1)) +
                     " must be greater than or equal to zero";
    }
    return;
  }
  const auto it = kRc.find(cmd);
  if (it == kRc.end()) return;
  for (auto& b : blocks) {
    if (!b.is_object() || b.value("type", std::string()) != "error") continue;
    const std::string msg = b.value("message", std::string());
    if (msg.find("[factor_misuse]") == std::string::npos ||
        msg.find("must be numeric") == std::string::npos)
      continue;
    const std::string reason = b.value("reason", std::string());
    if (reason.rfind("r(", 0) == 0) continue;
    int rc = it->second;
    if (rc == 2000) {
      // The variable the engine names, typed with a factor operator (i., ib#.,
      // or inside an interaction) anywhere in the varlist: 109.
      const size_t q1 = msg.find('\'');
      const size_t q2 = q1 == std::string::npos ? q1 : msg.find('\'', q1 + 1);
      const std::string var = q2 == std::string::npos ? std::string() : msg.substr(q1 + 1, q2 - q1 - 1);
      if (!var.empty()) {
        // Every term of an interaction is read (`i.make#c.mpg`, `c.mpg##i.make`):
        // a term is the variable after its operator's last dot.
        for (const auto& tok : parsed.varlist) {
          const std::string t = lower(tok);
          const bool interaction = t.find('#') != std::string::npos;
          size_t start = 0;
          while (start <= t.size()) {
            size_t end = t.find('#', start);
            if (end == std::string::npos) end = t.size();
            const std::string term = t.substr(start, end - start);
            start = end + 1;
            if (term.empty()) continue;  // the second `#` of `##`
            const size_t dot = term.rfind('.');
            const std::string name = dot == std::string::npos ? term : term.substr(dot + 1);
            const std::string op = dot == std::string::npos ? std::string() : term.substr(0, dot);
            if (name != var) continue;
            if (interaction || op.find('i') != std::string::npos) rc = 109;
          }
        }
      }
    }
    b["reason"] = "r(" + std::to_string(rc) + ")";
    if (rc == 2000) b["message"] = "no observations";
    else b["message"] = "string variables not allowed in varlist";
  }
}


bool checkStataLead(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {
  // Stata reads a line left to right, so an estimation command typed with
  // nothing before the comma is a replay of the last results, and with none in
  // memory Stata answers r(301) "last estimates not found" before it looks at
  // any option (`glm , eform`, `logit , or`). The arms
  // read their option vocabulary first; this step answers the replay for them.
  // The rest of the lead (an empty varlist, `using`, `=exp`) and the
  // options-not-allowed rule need no session state and are answered in
  // tokenize() (registry.h StataLead). With results in memory the arm decides.
  // ttest's two forms: `var == exp` (paired / one-sample) and `var, by()`;
  // together they are r(198) "may not combine = and option by()" (vs-stata
  // bank 36ab7cf6448a7aa5 `ttest mpg == trunk, by(foreign)`).
  if (cmd == "ttest") {
    bool eq = false, by = false;
    for (const auto& t : parsed.varlist) eq = eq || t.find('=') != std::string::npos;
    for (const auto& kv : parsed.opts) by = by || lower(kv.first) == "by";
    if (eq && by) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "may not combine = and option by()",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
      return true;
    }
  }
  // Stata reads the varlist before the options: a name that is no variable is
  // r(111) "variable X not found" before any option is judged (vs-stata bank:
  // `tobit mpg zzvar weight`, `zip ten zzvar age grade, inflate(..)`, `zinb`,
  // `xtlogit hi zzvar ..`, `xtpoisson fatal zzvar ..`, `heckman lw zzvar ..`,
  // `drdid zzz ..`, `eventstudyinteract zzz ..`), where PIET's arms answered
  // an option gap first. Only plain names are looked up (an abbreviation that
  // is unique counts); operators, wildcards and parentheses stay the arm's.
  {
    static const std::set<std::string> kVarlistFirst = {
        "tobit", "zip", "zinb", "xtlogit", "xtpoisson", "heckman", "drdid", "eventstudyinteract"};
    if (kVarlistFirst.count(cmd) != 0 && session.hasData()) {
      static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
      std::vector<std::string> names;
      for (const auto& t : parsed.varlist)
        if (std::regex_match(t, rePlain)) names.push_back(t);
      if (!names.empty()) {
        if (auto bad = loops::checkKnownVarlist(names, session)) {
          blocks.push_back(*bad);
          return true;
        }
      }
    }
  }
  // describe's `clear` belongs to `replace` (the describe-as-dataset form):
  // alone it is r(198) "option clear not allowed" (vs-stata bank
  // 34bf3ae35d0dcf5c; `describe, replace clear` runs).
  if (cmd == "describe") {
    bool clear = false, replace = false;
    for (const auto& kv : parsed.opts) {
      clear = clear || kv.first == "clear";
      replace = replace || kv.first == "replace";
    }
    if (clear && !replace) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "option clear not allowed",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
      return true;
    }
  }
  // One model type: `xtreg ..., fe re` is r(198) "only one of re, fe, be, pa,
  // or cre is allowed" and `xtlogit ..., fe re` "only one of re, fe, or pa is
  // allowed" (vs-stata bank 3794b924c441e5c5, dfde49d44aff3e76).
  if (cmd == "xtreg" || cmd == "xtlogit") {
    static const std::set<std::string> kXtreg = {"fe", "re", "be", "pa", "cre"};
    static const std::set<std::string> kXtlogit = {"fe", "re", "pa"};
    const auto& kinds = cmd == "xtreg" ? kXtreg : kXtlogit;
    int n = 0;
    for (const auto& kv : parsed.opts)
      if (kv.second.isBool && kinds.count(lower(kv.first)) != 0) ++n;
    if (n > 1) {
      blocks.push_back(ojson(eval::errorBlock(
          "syntax_error",
          cmd == "xtreg" ? "only one of re, fe, be, pa, or cre is allowed"
                         : "only one of re, fe, or pa is allowed",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
  }
  const registry::StataLead* row = registry::findStataLead(cmd);
  // replay() is true only when nothing at all precedes the comma: with an
  // `if`, `in` or weight the line is a (varlist-less) fit, not a replay.
  if (row == nullptr || !row->replay || !parsed.varlist.empty() || parsed.ifExpr.has_value() ||
      parsed.inRange.has_value() || parsed.weightType.has_value() || session.eval.eLive())
    return false;
  blocks.push_back(ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                          std::nullopt, std::nullopt, "r(301)")));
  return true;
}

}
