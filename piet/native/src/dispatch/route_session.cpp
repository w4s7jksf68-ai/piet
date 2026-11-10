// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "data_support.h"
#include "expression_support.h"
#include "session_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_session(std::string& cmd,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  // ------------------------------------------------------------------
  // M3b: clear / set obs / cd / pwd / noop — the foundational surface the
  // `clear -> set obs N -> gen` data-construction idiom (and every do-file
  // header's `set more off` / `version 16`) depends on.
  if (cmd == "clear") {
    // BS-044 (BETA campaign C4): forward the raw subcommand token (if any)
    // — runClear itself now does the full per-subcommand scoping (kept
    // arity-capped to a single token by kFixedArityCommands above).
    std::string clearSub = !parsed.varlist.empty() ? parsed.varlist[0] : std::string();
    ojson clearBlock = ojson(handlers::runClear(clearSub, session));
    // r() is empty after a clear that wrote it (`clear all`, `clear results`;
    // RULING.md D.3.14). A clear that left r() alone records nothing.
    if (!clearBlock.is_object() || clearBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "clear", {});
    blocks.push_back(std::move(clearBlock));
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "graphics") {
    // BS-047 (BETA A4): `set graphics on|off` is now genuinely tracked —
    // graph_m4.cpp's twoway/graph_combine builders read session.graphicsOff
    // to populate the wire-schema `suppressed` field (previously hardcoded
    // false; see graph_m4.cpp's own comment at both call sites).
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetGraphics(R"(\bset\s+graphics\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetGraphics)) arg = lower(m.str(1));
    }
    if (arg == "off") {
      session.graphicsOff = true;
    } else if (arg == "on") {
      session.graphicsOff = false;
    }
    // Neither on/off (a malformed value) silently no-ops, matching this
    // arm's pre-existing "tracked but tolerant" behavior.
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "obs") {
    std::smatch m;
    static const std::regex reSetObs(R"(\bset\s+obs\s+(\S+))", std::regex::icase);
    std::string nArg;
    if (std::regex_search(parsed.raw, m, reSetObs)) {
      nArg = m.str(1);
    } else if (parsed.varlist.size() > 1) {
      nArg = parsed.varlist[1];
    }
    blocks.push_back(ojson(handlers::runSetObs(nArg, session)));
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "level" || lower(parsed.varlist[0]) == "clevel")) {
    // BS-048 (BETA A4): `set level #` / `set clevel #` — the session
    // default confidence level, consulted ONLY when a command's own
    // `level(#)` option is absent (the existing --level flow:
    // buildEstimationArgvM4 / handlers.cpp::runRegress / runTtest).
    std::smatch m;
    static const std::regex reSetLevel(R"(\bset\s+(?:level|clevel)\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetLevel)) nArg = m.str(1);
    bool ok = false;
    int lv = 0;
    try {
      size_t pos = 0;
      lv = std::stoi(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    if (!ok || lv < 1 || lv > 99) {
      blocks.push_back(ojson(eval::engineError(
          "set level(#) must be an integer between 1 and 99, e.g. `set level 90`.",
          "unsupported_option")));
      return true;
    }
    session.level = lv;
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "seed") {
    // BS-046 (BETA A4): `set seed #` persistence + c(seed) exposure. No
    // cobra command in this build's dispatch surface yet consumes a
    // session-level default seed (wildboot/mediate/bootstrap land in later
    // D-phase waves, per the wildboot fixed-seed+distribution parity
    // strategy) — state-tracking + c(seed) readback only until then.
    std::smatch m;
    static const std::regex reSetSeed(R"(\bset\s+seed\s+(\S+))", std::regex::icase);
    std::string nArg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetSeed)) nArg = m.str(1);
    bool ok = false;
    long long sv = 0;
    try {
      size_t pos = 0;
      sv = std::stoll(nArg, &pos);
      ok = (pos == nArg.size());
    } catch (...) {
      ok = false;
    }
    // `set seed <statecode>` is `set rngstate` ([R] set seed; boottest
    // runs `set seed `=c(seed)'` to continue the current stream).
    if (!ok && !nArg.empty() && (nArg[0] == 'X' || nArg[0] == 'x') &&
        session.seed.stream().restore(nArg))
      return true;
    if (!ok) {
      blocks.push_back(ojson(eval::engineError(
          "set seed requires an integer argument, e.g. `set seed 12345`.", "unsupported_option")));
      return true;
    }
    session.seed = sv;
    session.rngStream = 0;  // every set seed restarts row draws, including an unchanged seed
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() &&
      (lower(parsed.varlist[0]) == "ty" || lower(parsed.varlist[0]) == "typ" ||
       lower(parsed.varlist[0]) == "type")) {
    // BUG-471 T2: `set type float|double [, permanently]` — the default storage
    // type of an untyped generate/egen (c(type); w2e_float_default.log
    // settype_double_gen :1252, settype_float_restore_gen :1292,
    // settype_abbrev_ty :1335 for the `ty` spelling). It was the generic
    // "ignored" echo below, so `set type double` changed nothing. `permanently`
    // is accepted and, like `set extcompat`, session-scoped: a do-file line
    // never rewrites the user's configuration.
    static const std::unordered_set<std::string> kSetTypeOpts = {"permanently"};
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kSetTypeOpts)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    const std::string arg = parsed.varlist.size() == 2 ? parsed.varlist[1] : std::string();
    if (arg != "float" && arg != "double") {
      // settype_bad_rc :1314 — `set type int` is r(198) and leaves c(type) as it
      // was (settype_bad_ctype_unchanged :1324).
      // StataBE's wording: :1309 for int (byte/long take it too); bare and any
      // other token per a17cd14f's StataBE 19.5 measurement.
      blocks.push_back(ojson(eval::engineError(
          arg.empty() ? std::string("nothing found where 'float' or 'double' expected")
          : (arg == "byte" || arg == "int" || arg == "long")
              ? "-set type- may not be set to " + arg + "; float and double are allowed"
              : "'" + arg + "' found where 'float' or 'double' expected",
          "unsupported_option")));
      return true;
    }
    session.defaultStorageType = arg;
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "more") {
    // EM1: `set more on|off [, permanently]` sets c(more) (w2af_macro_text_file.log
    // :328-405 mt_cmore_*, mt_set_more_*; w2d_uncovered.log:4505 set_more_off_perm).
    // `perm`, `permanent` and `permanently` are all accepted and, like `set type`'s,
    // session-scoped. Anything but on/off is r(198).
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const std::string o = lower(k);
      if (o.size() < 4 || std::string("permanently").compare(0, o.size(), o) != 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
    const std::string arg = parsed.varlist.size() == 2 ? lower(parsed.varlist[1]) : std::string();
    if (arg != "on" && arg != "off") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    session.moreOn = arg == "on";
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "extcompat") {
    // WS-B (extcompat, 2026-08-06): `set extcompat on|off` — the session
    // switch for the 17 community-package compatibility names (the gate at
    // step 1a-bis above). SESSION SCOPE ONLY, deliberately: a headless
    // `--do` run must never rewrite the user's config file as a side effect
    // of a line in a script, so nothing here calls settings::save(). The
    // DURABLE choice is owned by the first-run consent dialog and the
    // Settings panel, both of which go through settings.set. This mirrors
    // real Stata, where a `set` without `, permanently` lasts the session.
    std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg.empty()) {
      std::smatch m;
      static const std::regex reSetExt(R"(\bset\s+extcompat\s+(\S+))", std::regex::icase);
      if (std::regex_search(parsed.raw, m, reSetExt)) arg = lower(m.str(1));
    }
    if (arg == "on") {
      settings::setExtcompatEnabled(true);
    } else if (arg == "off") {
      settings::setExtcompatEnabled(false);
    } else {
      // Unlike `set graphics` above, a malformed value here is NOT tolerated:
      // silently leaving the names enabled when a script asked for them off
      // would be the silent-wrong-answer class.
      blocks.push_back(ojson(eval::engineError(
          "set extcompat requires on or off, e.g. `set extcompat off`.", "unsupported_option")));
    }
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "rngstate") {
    // `set rngstate statecode`: restore the session's Stata stream from a
    // c(rngstate) string (core/rng_mt64.h; the 4-hex check field is not verified,
    // its rule being unidentified). c(seed) is untouched, as in Stata.
    static const std::regex reSetState(R"(\bset\s+rngstate\s+(\S+))", std::regex::icase);
    std::smatch m;
    std::string arg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    if (std::regex_search(parsed.raw, m, reSetState)) arg = m.str(1);
    if (!session.seed.stream().restore(arg)) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              "set rngstate: invalid RNG state",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
    }
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "rng") {
    // `set rng default|mt64|mt64s|kiss32` records the setting c(rng) reads back
    // (w2j_e3_ritest.log D04 `mt64` after `set rng mt64`). PIET's own draws stay
    // Philox (src/core/rng.h): draw identity with Stata is out of scope (1-S),
    // the setting is what a do-file can observe.
    const std::string arg = parsed.varlist.size() > 1 ? lower(parsed.varlist[1]) : std::string();
    if (arg != "default" && arg != "mt64" && arg != "mt64s" && arg != "kiss32") {
      blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                              arg.empty() ? "set rng: generator name required"
                                                          : "set rng: " + arg + " invalid",
                                              std::nullopt, std::nullopt, std::nullopt,
                                              "r(198)")));
      return true;
    }
    session.rngSetting = arg;
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty() &&
      canonicalSetName(lower(parsed.varlist[0])) == "matsize") {
    // BUG-1330: StataBE ignores `set matsize #` too but checks the number and
    // keeps it for c(matsize) (cclass.cpp): an integer from 10 to 800 (`+50`,
    // `050`), else r(198) `50.5 found where number expected` / `matsize must
    // be between 10 and 800` (`-5`, `0`); a second word is r(101) `varlist not
    // allowed`, an option but `permanently` r(198). Then the generic arm below
    // prints the note.
    if (parsed.varlist.size() > 2) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "varlist not allowed", std::nullopt,
                                              std::nullopt, std::nullopt, "r(101)")));
      return true;
    }
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const std::string o = lower(k);
      if (o.size() < 4 || o.size() > 11 || std::string("permanently").compare(0, o.size(), o) != 0) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "option " + k + " not allowed",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(198)")));
        return true;
      }
    }
    const std::string arg = parsed.varlist.size() > 1 ? parsed.varlist[1] : std::string();
    static const std::regex reInteger(R"(^[+-]?\d+$)");
    std::string why;
    if (!std::regex_match(arg, reInteger)) {
      // StataBE words a word that ends in a point with two blanks (`50.  found`).
      why = arg.empty() ? std::string("nothing found where number expected")
                        : arg + (arg.back() == '.' ? "  " : " ") + "found where number expected";
    } else {
      // Past its sign and leading zeros; more than three digits is out of range.
      const size_t lead = arg.find_first_not_of("+-0");
      const long n = lead == std::string::npos ? 0
                     : arg.size() - lead > 3   ? 1000
                                               : std::stol(arg.substr(lead));
      if (arg[0] == '-' || n < 10 || n > 800) why = "matsize must be between 10 and 800";
    }
    if (!why.empty()) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", why, std::nullopt, std::nullopt,
                                              std::nullopt, "r(198)")));
      return true;
    }
  }


  if (cmd == "set" && !parsed.varlist.empty() && lower(parsed.varlist[0]) == "processors") {
    // BUG-1463: PIET answers as Stata/BE (c(MP) 0, c(processors_max) 1; cclass.cpp),
    // where every `set processors #` is r(198) "set processors allowed only with
    // Stata/MP." -- `set processors 1` too, and `set processors `=min(4,
    // c(processors_max))'' (StataBE 19.5). It used to fall to the ignored-setting
    // note below at rc 0.
    blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                            "set processors allowed only with Stata/MP.",
                                            std::nullopt, std::nullopt, std::nullopt, "r(198)")));
    return true;
  }


  if (cmd == "set" && !parsed.varlist.empty()) {
    // BS-101 (BETA A4): every OTHER `set` subcommand (more, matsize, scheme,
    // trace, linesize, ...; `type` has its own arm above) is a real, common,
    // harmless Stata idiom —
    // turning it into an error would halt do-files that use it, for no
    // PIET-side benefit. Previously a TOTALLY silent no-op; now an explicit,
    // non-halting note, so at least it's visible this was recognized and
    // deliberately ignored rather than swallowed without a trace.
    const std::string setName = canonicalSetName(lower(parsed.varlist[0]));
    if (setName == "rmsg" || setName == "trace" || setName == "varabbrev") {
      // BUG-1330: StataBE 19.5 takes `on` or `off` only: none is r(100) `on or off
      // required`, a second word r(101) `varlist not allowed`, any other word
      // (`ON` too) r(198) `'foo' found where on or off expected`, as is an option
      // but `permanently`.
      std::optional<std::pair<std::string, std::string>> bad;
      if (parsed.varlist.size() < 2)
        bad = {"on or off required", "r(100)"};
      else if (parsed.varlist.size() > 2)
        bad = {"varlist not allowed", "r(101)"};
      else if (parsed.varlist[1] != "on" && parsed.varlist[1] != "off")
        bad = {"'" + parsed.varlist[1] + "' found where on or off expected", "r(198)"};
      for (const auto& [k, v] : parsed.opts) {
        (void)v;
        const std::string o = lower(k);
        if (!bad && (o.size() < 4 || o.size() > 11 ||
                     std::string("permanently").compare(0, o.size(), o) != 0))
          bad = {"option " + k + " not allowed", "r(198)"};
      }
      if (bad) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", bad->first, std::nullopt,
                                                std::nullopt, std::nullopt, bad->second)));
        return true;
      }
    }
    ojson note;
    note["type"] = "echo";
    note["text"] = "(set " + parsed.varlist[0] + ": not applicable to PIET — ignored)";
    blocks.push_back(note);
    // BUG-1330: c() of this setting no longer answers its default (cclass.cpp);
    // matsize's checked number and the checked on/off are the answer instead.
    // Keyed by the setting's full name, so `set rm on` and `set mat 500` reach
    // c(rmsg) and c(matsize) (they answered the default, off and 400).
    std::string kept = "1";
    if (setName == "matsize")
      kept = std::to_string(std::stoi(parsed.varlist[1]));
    else if (setName == "rmsg" || setName == "trace" || setName == "varabbrev")
      kept = parsed.varlist[1];
    session.eval.globals.set(std::string(kSetMarkerPrefix) + setName, kept);
    return true;
  }


  if (cmd == "version") {
    // BUG-1330: `version #` sets the version c(version) reports, for the rest of
    // this do-file or program (kVersionLocal, cclass.h); it was a silent no-op
    // and c(version) the constant 18. c(userversion) follows it outside a
    // program or loop body (versionSetsUserVersion), and only it follows
    // `version #, user` (StataBE 19.5: `version 15` then `version 16, user` is
    // 15 and 16; a program's own `version 12` is 12 and its caller's
    // userversion). Any other option is ignored, as Stata ignores it (`USER`
    // too). A bare `version` prints the version in force, `version 15.0`.
    static const std::regex reHead(R"(^\s*\S+)");
    const std::string body = std::regex_replace(parsed.raw, reHead, "");
    const size_t comma = body.find(',');
    std::istringstream nums(body.substr(0, comma));
    std::vector<std::string> toks;
    for (std::string t; nums >> t;) toks.push_back(t);
    bool user = false;
    if (comma != std::string::npos) {
      std::istringstream opts(body.substr(comma + 1));
      for (std::string t; opts >> t;) user = user || t == "user";
    }
    if (toks.size() > 1 || (toks.empty() && comma != std::string::npos)) {
      blocks.push_back(ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt,
                                              std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    if (toks.empty()) {
      const std::string* cur = session.eval.locals.find(kVersionLocal);
      ojson db;
      db["type"] = "display";
      db["text"] =
          "version " + versionText(cur != nullptr ? std::strtod(cur->c_str(), nullptr) : 19.5);
      blocks.push_back(db);
      return true;
    }
    double v = 0.0;
    if (auto bad = parseVersionNumber(toks[0], &v)) {
      blocks.push_back(*bad);
      return true;
    }
    // BUG-1459: the caller version table reads (session.callerVersion) moves with
    // it; the do-file or program that set it restores it (CallerVersionScope).
    if (!user) {
      session.eval.locals.set(kVersionLocal, eval::fmtNumeric(v));
      session.callerVersion = v;
    }
    if (user || versionSetsUserVersion(session))
      session.eval.locals.set(kUserVersionLocal, eval::fmtNumeric(v));
    return true;
  }


  if (cmd == "set" || cmd == "version" || cmd == "compress") {
    // BUG-336 (batch 2), compress only: it takes no options (honest no-op engine-side);
    // set/version parse their own subcommand grammar and are not option-shaped.
    if (cmd == "compress") {
      if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCompress.known, kVocabCompress.unimplemented)) {
        blocks.push_back(ojson(*err));
        return true;
      }
    }
    if (cmd == "compress") {
      for (ojson& b : runCompressMeta(parsed, session)) blocks.push_back(std::move(b));
      return true;
    }
    // Bare `set` (no subcommand at all) / `version #` — silent, returns NO
    // block at all (commands.py's "noop" native_handler; only the echo block
    // appears in output).
    return true;
  }


  if (cmd == "exit") {
    // BUG-1462: `exit [[=]exp] [, clear STATA]` -- the code is an expression,
    // so `exit _rc` hands on the last return code (StataBE: `capture error
    // 459` / `exit _rc` ends the do-file with r(459)) and `exit = 3+2` is 5.
    // The exact code also rides in `reason`, so `capture exit 7` leaves _rc 7.
    std::string arg = trimWs(parsed.argText);
    if (!arg.empty() && arg[0] == '=') arg = trimWs(arg.substr(1));
    // StataBE 19.5, 2026-11-03: the code is truncated (`exit 3.7` is 3), keeps
    // its sign (`exit -1` leaves _rc -1), a missing value is 2147483647, and a
    // string is r(109) "type mismatch". An expression that fails fails as any
    // expression does: `exit nosuchname` and `exit y*2` are "... not found"
    // r(111).
    int code = 0;
    if (!arg.empty()) {
      bool isStr = false;
      std::string raised;
      std::optional<std::string> v = eval::evalArith(arg, &session.eval, &raised, &isStr);
      eval::MacroFnError fe;
      if (!v.has_value() && eval::classifyFailedExpr(arg, raised, &fe) && fe.failed()) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", fe.message, std::nullopt,
                                                std::nullopt, std::nullopt, "r(" + fe.rc + ")")));
        return true;
      }
      if (std::optional<std::string> name = v.has_value() ? std::nullopt : firstBareName(arg)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", *name + " not found", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(111)"))));
        return true;
      }
      if (v.has_value() && isStr) {
        blocks.push_back(ojson(eval::errorBlock("engine_error", "type mismatch", std::nullopt,
                                                std::nullopt, std::nullopt, std::string("r(109)"))));
        return true;
      }
      double d = 0.0;
      try {
        if (!v.has_value()) throw std::invalid_argument(arg);
        if (!v->empty() && (*v)[0] == '.') {
          d = 2147483647.0;
        } else {
          size_t used = 0;
          d = std::stod(*v, &used);
          if (used != v->size() || !std::isfinite(d)) throw std::invalid_argument(*v);
        }
      } catch (...) {
        ojson err = ojson(eval::engineError("invalid syntax", "syntax_error"));
        err["reason"] = "r(198)";
        blocks.push_back(std::move(err));
        return true;
      }
      code = static_cast<int>(std::max(-2147483648.0, std::min(d, 2147483647.0)));
    }
    std::string text = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    ojson exitBlock = ojson(eval::engineError(text, "program_exit"));
    exitBlock["reason"] = "r(" + std::to_string(code) + ")";
    blocks.push_back(std::move(exitBlock));
    return true;
  }


  if (cmd == "cd") {
    // BUG-251/WS-Q: was `rawLower.find("cd")` + `substr(pos + 2)`. `cd` is NOT
    // abbreviable in Stata (measured: `c /tmp` answers r(199)), so this one was
    // latent rather than live — but it is the same unguarded shape, one
    // registry edit away from the crash the `use` arm actually took.
    std::string dirArg = rawArgsAfterCmd(parsed.raw);
    ojson cdBlock = ojson(handlers::runCd(dirArg, session));
    if (cdBlock.value("type", std::string()) == "echo") session.cwdChosenByUser = true;
    blocks.push_back(std::move(cdBlock));
    return true;
  }


  if (cmd == "pwd") {
    blocks.push_back(ojson(handlers::runPwd(session)));
    return true;
  }


  if (cmd == "log") {
    ojson r = handlers::runLog(parsed.raw, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "which") {
    // NAME not found is rc 0 for a command this build routes or a community ado
    // the oracle machine has installed (its ado/plus tree, 2026-10-21), and
    // r(111) otherwise (aer_w2 w2b_dataio which_gxtile/which_gsieve 111,
    // w2t_boottest_family A2_which_ftools 0; xtivreg2 is routed, rc 0 since the
    // 2026-10-27 install: w2_pweight_xtivreg2 W1_which_xtivreg2 0). It used to answer rc 0 "assumed installed" for every name.
    static const std::set<std::string> kInstalledAdos = {
      "_eststo", "_gcsgvar", "_gmm_dripw", "_gmm_regipw", "_gmm_repeated", "_gtools_internal",
      "_het_did_gmm", "acreg", "acregpackcheck", "appendfile", "artest", "avar", "avar9",
      "binscatter", "binsglm", "binslogit", "binsprobit", "binspwc", "binsqreg", "binsreg",
      "binsreg_irecode", "binsreg_pctile", "binsregselect", "binstest", "boottest", "boottest_1",
      "carryforward", "cdfplot", "cibar", "coefplot", "colorcheck", "colorpalette",
      "colorpalette9", "csdid", "csdid_estat", "csdid_plot", "csdid_rif", "csdid_stats",
      "csdid_table", "did_imputation", "distinct", "drdid", "drdid_display", "drdid_predict",
      "erepost", "estadd", "estfe", "estout", "estpost", "eststo", "esttab", "event_plot",
      "eventstudyinteract", "fasterxtile", "fcollapse", "fegen", "fegen_group", "figout",
      "filelist", "fisid", "flevelsof", "fmerge", "frmttable", "fsort", "ftab", "ftools",
      "gcollapse", "gcontract", "gdistinct", "gduplicates", "gegen", "geodist", "geodist_run",
      "geonear", "gglm", "gisid", "givregress", "glevelsof", "gpoisson", "gquantiles", "grc1leg",
      "grc1leg2", "grc1leg2_examples", "greg", "gregress", "greshape", "grstyle", "grstyle_set",
      "gstats", "gtools", "gtop", "gtoplevelsof", "gunique", "hashsort", "hdfe", "heatplot",
      "hexplot", "ivreg2", "ivreg210", "ivreg210_p", "ivreg28", "ivreg28_cue", "ivreg28_p",
      "ivreg29", "ivreg29_cue", "ivreg29_p", "ivreg2_p", "ivreghdfe", "join", "labcd", "labcopy",
      "labdel", "labdtch", "lablog", "labmap", "labmask", "labnoeq", "labvalch", "labvalclone",
      "labvalcombine", "labvarch", "linepalette", "local_inlist", "mat2txt", "mdesc", "metaparm",
      "missings", "ms_add_comma", "ms_compile_mata", "ms_expand_varlist", "ms_fvstrip",
      "ms_fvunab", "ms_get_version", "ms_parse_absvars", "ms_parse_varlist", "ms_parse_vce",
      "oaxaca", "outreg", "outreg2", "outreg2_prf", "parallel", "parallel_append", "parallel_bs",
      "parallel_map", "parallel_map_template.do", "parallel_sim", "parmby", "parmcip", "parmest",
      "ppmlhdfe", "ppmlhdfe_header", "ppmlhdfe_p", "qreg2", "rangestat", "rangestat_run",
      "ranktest", "ranktest11", "ranktest9", "rdbwdensity", "rdbwselect", "rddensity", "rdplot",
      "rdrobust", "rdrobustplot", "reghdfe", "reghdfe3", "reghdfe3_estat", "reghdfe3_footnote",
      "reghdfe3_p", "reghdfe5", "reghdfe5_estat", "reghdfe5_footnote", "reghdfe5_header",
      "reghdfe5_p", "reghdfe5_parse", "reghdfe_estat", "reghdfe_footnote", "reghdfe_header",
      "reghdfe_p", "regsave", "regsave_tbl", "renvarlab", "renvars", "require", "ritest",
      "scoretest", "sdid", "seeout", "shellout", "splitvallabels", "strdist", "swindex",
      "symbolpalette", "tabout", "tabstatout", "tex", "texdoc", "texsave", "tolower", "tsspell",
      "tsvmat", "unique", "ustrdist", "waldtest", "weakivtest", "winsor2"};
    std::string wname = !parsed.varlist.empty() ? parsed.varlist[0] : std::string();
    if (wname.size() >= 2 && wname.front() == '"' && wname.back() == '"')
      wname = wname.substr(1, wname.size() - 2);
    if (!wname.empty() && registry::findByName(lower(wname)) == nullptr &&
        kInstalledAdos.count(wname) == 0) {
      blocks.push_back(ojson(eval::errorBlock(
          "engine_error", "command " + wname + " not found as either built-in or ado-file",
          std::nullopt, std::nullopt, std::nullopt, "r(111)")));
      return true;
    }
    blocks.push_back(ojson(handlers::runWhich(parsed.raw, parsed.varlist)));
    return true;
  }


  if (cmd == "ssc") {
    ojson r = handlers::runSsc(parsed.varlist);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "confirm") {
    ojson r = handlers::runConfirm(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "mkdir") {
    ojson r = handlers::runMkdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  // BUG-180 (P2-PREP): `rmdir DIR` had NO arm at all — see runRmdir
  // (handlers_m3.cpp) for the live-probed semantics and the benchmark-harness
  // failure that surfaced it.
  if (cmd == "rmdir") {
    ojson r = handlers::runRmdir(parsed.raw, parsed.varlist, parsed.opts, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }


  if (cmd == "erase" || cmd == "rm") {
    // BS-045 (BETA campaign C4): `rm` is a genuine Stata built-in synonym
    // of `erase` (live-probed `which rm` -> "built-in command: rm"), not
    // an invented alias — same handler, same behavior.
    ojson r = handlers::runErase(parsed.raw, parsed.varlist, session);
    if (!(r.is_array() && r.empty())) blocks.push_back(r);
    return true;
  }
  return false;
}

}
