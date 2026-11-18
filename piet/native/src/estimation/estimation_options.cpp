// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Preserve the existing registered option vocabularies and their bindings.
#include "estimation_options.h"
#include <unordered_set>

namespace piet::handlers::estimation_vocab {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weighttype", "null",
                                                          "cluster"},
     {}});
// tobit: nolog / log (the iteration log, which PIET does not print) and iterate(#)
// (runTobit forwards it); offset() is valid Stata the engine does not fit.
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust", "nolog", "log", "iterate"},
     {"offset"}});
// heckman: the maximum-likelihood fit reads noconstant, iterate(), vce() / robust and
// the display-free nolog / log (runHeckmanMl); first (the probit's table),
// nshazard() / mills() (a new variable) and ml's other options are valid Stata it
// does not implement.
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level", "noconstant", "nolog", "log", "iterate", "vce",
                   "robust"},
     {"first", "nshazard", "mills", "offset", "constraints", "technique", "difficult", "from",
      "score", "noskip", "collinear", "rhosigma", "rhotrunc", "rholimited", "rhoforce"}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09): `post` moves to `known` -- see
// the postestimation_vocab copy (the one actually READ by runMargins) for the
// full rationale. Three byte-identical copies must move together.
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post", "atmeans"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag", "force", "level"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level", "nolog", "log", "iterate", "irr"},
     {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
// w2b-estim-vce-options: probit's `robust` abbreviates to ONE character --
// tests/eval/golden/aer_w2/w2b_estim.log:706 `probit d x1 x2, r nocons` is rc 0
// with the Robust std. err. column. logit gets `r` from the measured-minimums
// table (opt_minimums_bug264.inc `{"robust", 1}`); probit has no robust row
// there, and the 1-char spelling is below canonicalizeForCommand's prefix
// floor, so it is DECLARED here as an alias (BUG-430's mechanism). `known`
// holds `robust` alone: it is the only key this registration speaks for, and
// every longer prefix of it is accepted by the same 1-char minimum. The
// handler's own net (buildEstimationArgvM4 kEstimationKnownOpts) is unchanged.
const piet::eval::OptVocab& kVocabProbitRobust = piet::eval::registerOptVocab(
    {"probit"}, {{"robust"}, {}, {{"r", "robust"}}});
}

namespace piet::handlers::extended_estimation_vocab {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
// FU-23 (BUG-757): `vce` and `robust` are accepted and IGNORED by csdid itself,
// whatever their argument: w2l_vce_shape.log csdid_ctl_vcecluster,
// csdid_ctl_vcerobust, csdid_vce_c2, csdid_vce_cluster_novar, csdid_robust_val
// and csdid_vce_robust_x are all rc 0 with the default fit's se11 2.931073697886207.
// Registering the token unions with the two other TUs' copies (see above), so
// adding them here is enough for the dispatcher and for runCsdid's net.
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2",
      "vce", "robust",
      // csdid-notyet-agg: csdid `[notyet]`, `agg(str)` (w2ag log:451 ag_ny,
      // :1040-1272 ag_asimple/aevent/agroup/acal, all rc 0). Unions like vce/robust above.
      "notyet", "agg",
      // csdid.ado `Level(int 95)`: the table's interval (vs-stata bank 1ae70ce8).
      "level"},
     {},
     // Level's capital L makes `l` its minimum abbreviation; a one-letter key is
     // below the prefix floor (and `l` also prefixes long2), so it is declared.
     {{"l", "level"}}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
// sureg's syntax: the options this build does not forward to the fit
// (iterated SUR, dfk/small corrections, constraints, the corr/BP test) are
// refused by name; level()/noconstant/vce() reach runReg3's own refusals.
const piet::eval::OptVocab& kVocabSureg = piet::eval::registerOptVocab(
    {"sureg"}, {{"level", "noconstant", "nocons", "vce", "noheader", "notable", "nofooter",
                 "nolog", "log", "isure", "iterate", "tolerance"},
                {"dfk", "dfk2", "small", "corr", "constraints", "trace"}});
// FU-23 (BUG-758): `nograph` suppresses boottest's confidence-curve plot; PIET's
// boottest draws none, so the option has nothing to change. Every boottest arm
// of w2l_vce_shape.log passes it at rc 0, and this net refused it r(198).
// `boottype` (papers 053, 086): runBoottest reads its value (w2t C12, F6).
// `weighttype` (minimum abbreviation `weight`, boottest), not `weights`
// (BUG-1173).
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weighttype", "null",
                                                          "cluster", "nograph", "boottype"},
     {}});
// tobit: nolog / log (the iteration log, which PIET does not print) and iterate(#)
// (runTobit forwards it); offset() is valid Stata the engine does not fit.
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust", "nolog", "log", "iterate"},
     {"offset"}});
// heckman: the maximum-likelihood fit reads noconstant, iterate(), vce() / robust and
// the display-free nolog / log (runHeckmanMl); first (the probit's table),
// nshazard() / mills() (a new variable) and ml's other options are valid Stata it
// does not implement.
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level", "noconstant", "nolog", "log", "iterate", "vce",
                   "robust"},
     {"first", "nshazard", "mills", "offset", "constraints", "technique", "difficult", "from",
      "score", "noskip", "collinear", "rhosigma", "rhotrunc", "rholimited", "rhoforce"}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09): `post` moves to `known` -- see
// the postestimation_vocab copy (the one actually READ by runMargins) for the
// full rationale. Three byte-identical copies must move together.
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post", "atmeans"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post",
      // csdid-estat-estore-post: csdid_estat.ado csdid_simple/csdid_event
      // `syntax, [estore(name) ...]` (w2ag log:1359 ag_evst_restore rc 0). Unions
      // with the two other TUs' copies (registerOptVocab, errors.h).
      "estore"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag", "force", "level"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
// w2b-iv-options: `first` (w2b_estim.log:879 ivregress_first, :1064
// ivreghdfe_first, both rc 0). Registered on its own rather than edited into
// the verbatim copy above: the registry UNIONS per token, so this lands in the
// same `reghdfe` slot kVocabIvRegress names, which is runIvRegress's net (and,
// for canonicalisation only, the non-IV reghdfe word, whose own hand-written
// net in runRegress still refuses `first`).
const piet::eval::OptVocab& kVocabIvFirst = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"first"}, {}});
// w2b-iv-options: ppmlhdfe's `d` / `d(newvar)` (w2b_estim.log:1305
// ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named). ppmlhdfe's slot only: plain
// `poisson` has no such option.
const piet::eval::OptVocab& kVocabPpmlD = piet::eval::registerOptVocab(
    {"ppmlhdfe"}, {{"d"}, {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level", "nolog", "log", "iterate", "irr"},
     {}});
// `nolog` / `log`: plain poisson's (and nbreg's) iteration-log switch. PIET prints
// no iteration log, so the table and results are those of Stata's `nolog` fit
// (bank 5f9d1097 `poisson fatal beertax unemp, nolog`). poisson's slot only: the
// shared count-model net also serves ppmlhdfe, whose options are not ml's.
const piet::eval::OptVocab& kVocabPoissonLog = piet::eval::registerOptVocab(
    {"poisson"}, {{"nolog", "log", "iterate", "irr"}, {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
}

namespace piet::handlers::postestimation_vocab {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weighttype", "null",
                                                          "cluster"},
     {}});
// tobit: nolog / log (the iteration log, which PIET does not print) and iterate(#)
// (runTobit forwards it); offset() is valid Stata the engine does not fit.
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust", "nolog", "log", "iterate"},
     {"offset"}});
// heckman: the maximum-likelihood fit reads noconstant, iterate(), vce() / robust and
// the display-free nolog / log (runHeckmanMl); first (the probit's table),
// nshazard() / mills() (a new variable) and ml's other options are valid Stata it
// does not implement.
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level", "noconstant", "nolog", "log", "iterate", "vce",
                   "robust"},
     {"first", "nshazard", "mills", "offset", "constraints", "technique", "difficult", "from",
      "score", "noskip", "collinear", "rhosigma", "rhotrunc", "rholimited", "rhoforce"}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09, BUG-467's sibling surface): `post` REPLACES
// e() with the margin (Cells `B4REC|margins|dydx_post|0|`,
// `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`, oracle_r.log). `margins`
// is in kOptMinComplete (opt_minimums_bug264.inc), so the key arrives
// exactOnly -- adding it to `known` accepts ONLY the exact spelling `post`;
// no abbreviation surface widens. THREE byte-identical copies (the estimation_vocab,
// extended_estimation_vocab and postestimation_vocab namespaces of this file) -- registerOptVocab unions
// registrations for the same command, so all three must move together or
// two would silently drift (IDIOMS Duplication). `atmeans` (FU-94,
// w2b_estim.log:499/:558/:588) joins the same way, exact spelling only.
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post", "atmeans"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag", "force", "level"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level", "nolog", "log", "iterate", "irr"},
     {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
// lincom's own net (dispatch/route_postestimation.cpp) accepted every unique
// prefix of these words but the handler reads the full spelling, so `le(90)` and
// `ef` were accepted and then dropped: the 95% interval and the linear metric
// came back (bank 41338389 `lincom mpg - 2*foreign, le(90)` r(ub) -5062.87 at
// 90%; def07b25 `lincom mpg + weight, ef` r(ub) 1.00921 = exp()). Registering
// the words lets the dispatcher hand the handler the canonical key. lincom.ado's
// `syntax` declares Level(cilevel), so `l` is a declared spelling (bank 659439a7
// `l(90)` rc 0), an alias since it is below the prefix rule's floor.
// stcox's ties, strata and display-metric words (stcox.ado `syntax`: BREslow
// EFRon EXACTM EXACTP noHR, and STrata() through _vce_parserun's mark list), so
// `st(drug)` / `efr` reach the handler as the words it reads (bank 62687ee2
// `stcox age, st(drug)`, 6ccd1af5 `stcox age i.drug, efr`).
const piet::eval::OptVocab& kVocabStcox = piet::eval::registerOptVocab(
    {"stcox"}, {{"nohr", "breslow", "efron", "exactm", "exactp", "strata", "vce", "vcov", "robust",
                 "cluster", "level"},
                {"tvc", "texp", "offset", "shared", "frailty", "basehc", "basechazard", "basesurv",
                 "mgale", "esr", "schoenfeld", "scaledsch"},
                {}});
const piet::eval::OptVocab& kVocabLincom = piet::eval::registerOptVocab(
    {"lincom"}, {{"level", "eform", "or", "hr", "irr", "rrr", "shr", "coef"},
                 {"post"},
                 {{"l", "level"}}});
}

namespace piet::handlers::session_vocab {
const piet::eval::OptVocab& kVocabLog = piet::eval::registerOptVocab(
    {"log"}, {{"replace", "append", "text",
                                                                   "smcl", "name"},
     {}});
const piet::eval::OptVocab& kVocabMkdir = piet::eval::registerOptVocab(
    {"mkdir"}, {{"public"},
     {}});
}
