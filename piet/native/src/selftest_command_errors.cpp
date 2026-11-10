// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: command errors.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── WS-B (extcompat, 2026-08-06): the community-name gate ────────────────────
//
// Three separable things are pinned here, because three different mistakes
// are possible:
//   (1) TABLE DRIFT — communityNameTable() (author + native equivalent, the
//       gate message's only source) and the per-row CommandSpec::communityNames
//       tagging are two hand-kept lists of the same 17 names. Either can be
//       edited alone. The cross-check below makes that impossible to ship.
//   (2) GATE BEHAVIOUR — off rejects with r(199) and the pinned message shape
//       (both variants: with and without a native equivalent); on/default runs
//       the command exactly as before; PIET's OWN names are never gated.
//   (3) THE `set` VERB — moves the session state in both directions and
//       honestly rejects a malformed value instead of silently leaving the
//       names enabled (which would be the silent-wrong-answer class).
//
// The gate default is ON, and this function restores it before returning, so
// no later selftest case inherits a disabled registry.
void testExtcompatGate() {
  std::printf("[selftest] --- extcompat community-name gate (WS-B) ---\n");

  // (1) Drift: the two lists must name exactly the same 17 spellings.
  {
    std::set<std::string> fromTable;
    for (const auto& cn : piet::registry::communityNameTable()) fromTable.insert(cn.name);
    std::set<std::string> fromRows;
    for (const auto& spec : piet::registry::commandRegistry()) {
      for (const auto& n : spec.communityNames) fromRows.insert(n);
    }
    // AER-SCOPE1: the campaign adds community names bundle by bundle (A adds
    // Cox's `winsor`; C adds the eight gtools names and the two ftools ones,
    // 18 -> 28; E adds the twelve TABLES packages -- estadd estpost estout
    // outreg2 texsave texdoc tabout frmttable regsave parmest mat2txt erepost
    // -- 28 -> 40), so this literal moves with it. `tex` is NOT among them and
    // must not be counted: it is texdoc's line verb, a spelling of the same
    // package rather than a package of its own, and it carries its own registry
    // row only because it reaches the dispatcher as a head. The count is a
    // tripwire against a name silently DISAPPEARING; the cross-check below is
    // the real lock, and it is the one that cannot be satisfied by editing a
    // number. `fastreshape` is NOT among the 40 and must not be added to make a
    // count come out: its author is not established, and communityNameTable()
    // is also the source of the NOTICE.txt attribution this project owes.
    //
    // I adds the twelve UTIL packages -- unique gunique distinct missings mdesc
    // labmask renvarlab splitvallabels filelist carryforward rangestat tsspell
    // -- 40 -> 52. `marksample` is NOT among them and must not be counted: it
    // is StataCorp's own built-in, so it carries a registry row and no
    // community tag. Neither is `dropmiss` (no package under that name on any
    // path -- only the successor `missings` -- so no author to attribute), nor
    // the three record-linkage names, which have no registry row at all.
    // `renvars` was refused here for the same "no package" reason and that
    // reason turned out to be SSC-shaped: the package is on the Stata Journal
    // as dm88 (`net install dm88_1, from(.../sj5-4)`), and the installed
    // renvars.ado header names Nicholas J. Cox and Jeroen Weesie. K1 gives it
    // its own handler (an alias onto renvarlab would have answered r(198) on
    // the real corpus lines) and it IS counted below.
    //
    // J adds the six GRAPH-X packages -- grc1leg2 grstyle geodist geonear
    // cdfplot event_plot -- 52 -> 58. `grc1leg` (un-suffixed) was NOT among
    // them, on the ground that "its package states no author anywhere" and a
    // row invented from a distribution URL is precisely what this rule exists
    // to stop. Both halves of that were right about the two files it read (the
    // .ado and the .sthlp); what it did not read is the installer's own record
    // of the .pkg description, `ado/plus/stata.trk`, whose `d` lines for
    // grc1leg.pkg say "Program by Vince Wiggins, StataCorp" and give the 2003
    // Statalist distribution date. That is the package's OWN claim rather than
    // an inference from where it is hosted, so the refusal is inverted and the
    // name is counted below. Still NOT among them are the eight refusals
    // (cibar binsreg spmap maptile shp2dta geoinpoly heatplot distplot), which
    // have no registry row at all.
    // 58 -> 60: AER bundle H adds ivreg2 and xtivreg2. ivreg2's attribution is
    // VERIFIED against the package installed on this machine -- ivreg2.sthlp's
    // citation block reads "Baum, C.F., Schaffer, M.E., Stillman, S. 2010." and
    // names all three. (The .ado header's "authors cfb & mes" is a code
    // shorthand; taking it literally would have dropped Stillman.) xtivreg2 is
    // not installed here, so its row rests on the published module citation.
    // 64 -> 65: bundle K1c adds `parallel`, whose ado header credits George G.
    // Vega Yon and Brian Quistorff.
    // 63 -> 64: AER scope 2 bundle B2 adds `strdist`, whose ado header names
    // Michael D Barker and Felix Pöge. It was previously excluded as "record
    // linkage, out of scope"; the scope moved, not the attribution rule.
    // 60 -> 63: AER scope 2 bundle K1 adds `grc1leg` on the stata.trk evidence
    // described above, plus `tolower` (tolower.ado's own first line credits Nick
    // Cox, Statalist Digest 895) and `renvars` (renvars.ado's `*!` line credits
    // njc/jw, STB-60 dm88). `move` is NOT among them and must not be added to
    // make a count come out: it is StataCorp's own built-in, so it takes a
    // registry row and no community tag, exactly like `marksample` above.
    // 68 -> 78: AER tier-1 (2026-10-28) adds require (Correia, Seay -- require.sthlp's
    // Authors block), labvars, astile, matsort, matselrc, fillmissing, findname,
    // sencode, zipsave and zipuse, each read from its package's `ssc describe`
    // record on the oracle Mac (matselrc from STB-56 dm79's record; zipuse ships in
    // the zipsave package). The official verbs of the same bundle take rows, no tag.
    // 66 -> 68: package-007 unblock (T7) adds `ihstrans` (ihstrans.ado's `*!` line
    // credits Jan Helmdag) and `sctorezone` (sctorezone.ado's header credits
    // SurveyCTO / William Blackmon, IPA, package scto). `lookfor` is StataCorp's
    // own ado and takes a registry row, no community tag.
    // 78 -> 79: package-007 lane LB adds `leebounds` (leebounds.ado's `*!` line
    // credits Harald Tauchmann); it landed on main as 68 -> 69 while the tier-1
    // branch was out, and the two counts met at its merge.
    check(fromTable.size() == 79,
          "extcompat: communityNameTable() carries exactly the 79 documented names, got " +
              std::to_string(fromTable.size()));
    check(fromTable == fromRows,
          "extcompat: CommandSpec::communityNames tagging == communityNameTable() (no drift)");

    // Every gated name must be a REGISTERED spelling, or the gate would be
    // rejecting a name the dispatcher never had an arm for anyway — and the
    // message would promise a compatibility that does not exist.
    bool allRegistered = true;
    bool allAttributed = true;
    for (const auto& cn : piet::registry::communityNameTable()) {
      if (piet::registry::findByName(cn.name) == nullptr) allRegistered = false;
      if (cn.author.empty()) allAttributed = false;
      // A declared native equivalent must itself be a real command.
      if (!cn.nativeEquivalent.empty() &&
          piet::registry::findByName(cn.nativeEquivalent) == nullptr) {
        allRegistered = false;
      }
    }
    check(allRegistered,
          "extcompat: every community name (and every declared native equivalent) is a "
          "registered spelling");
    check(allAttributed, "extcompat: every community name carries a NOTICE.txt author string");
  }

  // Canonical flip (WS-B): our own name owns the row; the package name is the
  // alias. Both spellings still resolve.
  {
    const auto* viaCommunity = piet::registry::findByName("reghdfe");
    const auto* viaNative = piet::registry::findByName("feols");
    check(viaCommunity != nullptr && viaCommunity->canonical == "feols",
          "extcompat: `reghdfe` resolves to the row whose canonical is `feols` (canonical flip)");
    check(viaNative != nullptr && viaNative->canonical == "feols",
          "extcompat: `feols` is the canonical spelling of its own row");
  }

  // Message shape — built from the table, asserted verbatim.
  {
    const auto* withNative = piet::registry::findCommunityName("reghdfe");
    const auto* withoutNative = piet::registry::findCommunityName("csdid");
    check(withNative != nullptr &&
              piet::registry::communityGateMessage(*withNative) ==
                  "'reghdfe' is a community-package compatibility name (package by Sergio "
                  "Correia (+ Paulo Guimaraes, Tom Zylkin), not affiliated). PIET's native "
                  "command is 'feols'. Run: set extcompat on",
          "extcompat: gate message (native-equivalent variant) is byte-exact");
    check(withoutNative != nullptr &&
              piet::registry::communityGateMessage(*withoutNative) ==
                  "'csdid' is a community-package compatibility name (package by Fernando "
                  "Rios-Avila; Brantly Callaway, Pedro H. C. Sant'Anna, not affiliated). Run: "
                  "set extcompat on",
          "extcompat: gate message drops the middle sentence when there is no native equivalent");
  }

  // (2)/(3) Live dispatch behaviour.
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path() / "piet_native_selftest_extcompat";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  std::string workdir = tmp.u8string();

  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    }
    return nlohmann::ordered_json();
  };

  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);

    // Default (nothing persisted, no `set`): the names work. This is the
    // state every gate/wave/freeze run is in.
    check(piet::settings::extcompatEnabled(),
          "extcompat: default is ON (absent settings file -> the 17 names answer)");
    {
      auto blocks = piet::dispatch("reghdfe fatal beertax, absorb(state)", session);
      auto err = firstError(blocks);
      check(err.is_null() || err.value("code", std::string()) != "unsupported_command",
            "extcompat ON: `reghdfe` reaches its real handler (not the gate)");
    }

    // `set extcompat off` — the verb moves the session state.
    (void)piet::dispatch("set extcompat off", session);
    check(!piet::settings::extcompatEnabled(), "extcompat: `set extcompat off` disables the names");

    {
      auto blocks = piet::dispatch("reghdfe fatal beertax, absorb(state)", session);
      auto err = firstError(blocks);
      check(!err.is_null() && err.value("code", std::string()) == "unsupported_command" &&
                err.value("token", std::string()) == "reghdfe",
            "extcompat OFF: `reghdfe` is rejected as an unsupported command");
      check(!err.is_null() &&
                err.value("message", std::string())
                        .find("is a community-package compatibility name") != std::string::npos,
            "extcompat OFF: the reject carries the community-package message");
      check(!err.is_null() && piet::eval::coarseRc(err) == "199",
            "extcompat OFF: the reject maps to Stata r(199)");
    }

    // Our own spellings are untouched by the gate.
    {
      auto blocks = piet::dispatch("feols fatal beertax, absorb(state)", session);
      auto err = firstError(blocks);
      check(err.is_null() || err.value("code", std::string()) != "unsupported_command",
            "extcompat OFF: PIET's own `feols` still runs");
    }
    {
      auto blocks = piet::dispatch("regress fatal beertax", session);
      auto err = firstError(blocks);
      check(err.is_null() || err.value("code", std::string()) != "unsupported_command",
            "extcompat OFF: an ordinary Stata command is unaffected");
    }

    // A row whose canonical IS a community name is gated on that canonical.
    {
      auto blocks = piet::dispatch("winsor2 fatal", session);
      auto err = firstError(blocks);
      check(!err.is_null() && err.value("token", std::string()) == "winsor2",
            "extcompat OFF: a row whose canonical IS the community name is gated too");
    }

    // Back on, and the same line runs again.
    (void)piet::dispatch("set extcompat on", session);
    check(piet::settings::extcompatEnabled(), "extcompat: `set extcompat on` re-enables the names");
    {
      auto blocks = piet::dispatch("reghdfe fatal beertax, absorb(state)", session);
      auto err = firstError(blocks);
      check(err.is_null() || err.value("code", std::string()) != "unsupported_command",
            "extcompat: after `set extcompat on` the name answers again");
    }

    // Malformed value: an honest reject, and the state does NOT move.
    {
      auto blocks = piet::dispatch("set extcompat maybe", session);
      auto err = firstError(blocks);
      check(!err.is_null() && err.value("code", std::string()) == "unsupported_option",
            "extcompat: `set extcompat <garbage>` is an honest reject");
      check(piet::settings::extcompatEnabled(),
            "extcompat: a malformed `set` leaves the previous state intact");
    }
  }

  // Settings persistence + the consent pair, in an isolated store.
  {
    std::string settingsFile = (tmp / "settings.json").u8string();
#ifndef _WIN32
    setenv("PIET_SETTINGS_FILE", settingsFile.c_str(), 1);
#else
    _putenv_s("PIET_SETTINGS_FILE", settingsFile.c_str());
#endif
    check(piet::settings::testIsolationActive(),
          "extcompat: $PIET_SETTINGS_FILE marks a gate/test isolation run");
    {
      auto r = piet::routes::settingsGet("{}");
      auto j = nlohmann::json::parse(r.json);
      check(j.value("extcompat", false), "extcompat: a fresh store reports extcompat true");
      check(j.value("extcompat_consent_at", std::string("x")).empty(),
            "extcompat: a fresh store reports no consent timestamp");
      // The test-mode auto-accept: no dialog is ever requested under
      // isolation, which is what keeps --ui-smoke and the DOM gate unblocked.
      check(!j.value("consent_required", true),
            "extcompat: consent is NEVER required under test isolation (auto-accept)");
    }
    {
      nlohmann::ordered_json body;
      body["extcompat"] = false;
      body["extcompat_consent_at"] = "2026-08-06T00:00:00Z";
      auto rs = piet::routes::settingsSet(body.dump());
      check(rs.ok, "extcompat: settings.set persists the consent pair");
      piet::settings::Settings reread = piet::settings::load();
      check(!reread.extcompat && reread.extcompatConsentAt == "2026-08-06T00:00:00Z",
            "extcompat: a fresh load() reads the pair back off disk");
      check(!piet::settings::extcompatEnabled(),
            "extcompat: a persisted choice applies to the next command (cache refreshed)");
      auto r2 = piet::routes::settingsGet("{}");
      auto j2 = nlohmann::json::parse(r2.json);
      check(!j2.value("consent_required", true),
            "extcompat: a stored consent timestamp clears consent_required");
    }

    // WS-B step 6: the pack scan ENUMERATES and does nothing else. Pinned
    // here because the whole value of a stub is that it stayed a stub — a
    // future loader must be a deliberate change, not something that grew.
    {
      check(piet::settings::enumeratePacks().empty(),
            "packs: no packs dir -> empty enumeration (the normal case)");
      fs::path packs = fs::path(piet::settings::packsDir());
      fs::create_directories(packs / "demo-pack", ec);
      fs::create_directories(packs / "not-a-pack", ec);
      // A directory WITHOUT a manifest is not a pack...
      check(piet::settings::enumeratePacks().empty(),
            "packs: a directory with no manifest.json is not enumerated");
      // ...and one WITH a manifest is listed by id, with the manifest itself
      // left unread (this one is deliberately unparseable: a loader would
      // choke on it, an enumerator must not care).
      std::ofstream(packs / "demo-pack" / "manifest.json") << "{ not valid json";
      auto ids = piet::settings::enumeratePacks();
      check(ids.size() == 1 && ids[0] == "demo-pack",
            "packs: a directory carrying manifest.json is enumerated by its id");
      check(ids.size() == 1,
            "packs: the enumeration reads no manifest and loads nothing (an invalid "
            "manifest is still merely listed)");
      auto rp = piet::routes::settingsGet("{}");
      auto jp = nlohmann::json::parse(rp.json);
      check(jp.contains("packs") && jp["packs"].is_array() && jp["packs"].size() == 1,
            "packs: settings.get carries the boot-time enumeration");
      fs::remove_all(packs, ec);
    }
#ifndef _WIN32
    unsetenv("PIET_SETTINGS_FILE");
#else
    _putenv_s("PIET_SETTINGS_FILE", "");
#endif
  }

  // The OTHER half of the auto-accept, and the one that can actually fail
  // silently: a gate that only proves "no dialog under isolation" is GREEN
  // even if the dialog can never appear at all. Drop the isolation hook and
  // point the real config-dir resolution at a throwaway HOME — a genuine
  // first launch — and consent MUST be requested.
#ifndef _WIN32
  {
    const char* realHome = std::getenv("HOME");
    std::string savedHome = realHome ? realHome : "";
    fs::path fakeHome = tmp / "fresh_home";
    fs::create_directories(fakeHome, ec);
    setenv("HOME", fakeHome.u8string().c_str(), 1);
#if !defined(__APPLE__)
    const char* realXdg = std::getenv("XDG_CONFIG_HOME");
    std::string savedXdg = realXdg ? realXdg : "";
    unsetenv("XDG_CONFIG_HOME");
#endif
    auto r = piet::routes::settingsGet("{}");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("consent_required", false),
          "extcompat: a genuine first launch (no isolation, no stored consent) DOES require "
          "consent — the auto-accept is scoped to gate runs, it did not disable the dialog");
    if (!savedHome.empty()) {
      setenv("HOME", savedHome.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
#if !defined(__APPLE__)
    if (!savedXdg.empty()) setenv("XDG_CONFIG_HOME", savedXdg.c_str(), 1);
#endif
  }
#endif

  // Restore the process default for every case that runs after this one.
  piet::settings::invalidateExtcompatCache();
  piet::settings::setExtcompatEnabled(true);
  fs::remove_all(tmp, ec);
}


// --- BUG-105 secondary ① (2026-07-31): unsupported_command message honesty.
//
// The retired fallback sentence ("If this is a valid Stata command, it may be
// added in a later milestone") was ONE string covering three unrelated causes,
// and it cast doubt on Stata-validity in every one of them — actively wrong for
// the abbreviation gaps above, where the command IS valid Stata and the real
// reason was that PIET's own table lacked the prefix.
//
// So the claim under test is not "an error was raised" — that was already true
// and already asserted elsewhere by `code`. It is that the three causes now say
// three DIFFERENT things. Each arm therefore asserts both its own wording and
// the ABSENCE of the other two's: an assertion that only checked for its own
// substring would still pass if the fix collapsed back into one shared string
// that happened to contain all three phrases.
void testBUG105UnsupportedCommandHonesty() {
  std::printf("[selftest] --- BUG-105 secondary (1): unsupported_command honesty ---\n");

#ifdef _WIN32
  std::error_code uhec;
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_bug105unsupp").u8string();
  std::filesystem::create_directories(workdir, uhec);
#else
  std::string workdir = "/tmp/piet_native_selftest_bug105unsupp";
  (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif

  // Fresh session + fresh `sysuse` per invocation: `graph bar`'s honest-error
  // branch sits behind graph's own inner hasData() re-check, so an empty
  // session would answer no_data_loaded and the arm would measure nothing.
  auto errOf = [&](const std::string& cmd) {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    nlohmann::json blocks = piet::dispatch(cmd, session);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    }
    return nlohmann::json();
  };
  auto says = [](const nlohmann::json& b, const std::string& needle) {
    return b.value("message", std::string()).find(needle) != std::string::npos;
  };

  // The defect string and the doubt-casting clause it lived in. Neither may
  // survive on ANY of the three arms.
  const std::string kRetiredTail = "later milestone";
  const std::string kRetiredDoubt = "valid Stata command";
  // Each cause's own signature phrase.
  const std::string kTableSig = "command table has no entry";
  const std::string kGapSig = "recognized Stata feature";
  const std::string kSubSig = "this subcommand is not";

  // Arm 1 — kUnknownToken: the token never resolved to a table entry. This is
  // the arm every BUG-105 abbreviation gap landed on.
  {
    nlohmann::json b = errOf("totallyunknownxyz123");
    check(b.value("code", std::string()) == "unsupported_command",
          "BUG-105(1): an unrecognized token is still an honest unsupported_command");
    check(says(b, kTableSig),
          "BUG-105(1): the unknown-token message names PIET's command table as the reason");
    check(!says(b, kRetiredTail) && !says(b, kRetiredDoubt),
          "BUG-105(1): it no longer casts doubt on the command's Stata-validity");
    check(!says(b, kGapSig) && !says(b, kSubSig),
          "BUG-105(1): unknown-token does not also claim the other two causes");
  }

  // Arm 2 — kNotImplemented: `graph pie` IS valid Stata; PIET just lacks it.
  // Telling this user to check their spelling would be the same class of lie
  // the retired sentence told, pointed the other way. (graph-family AI-1:
  // `graph bar`, the arm's first subject, is implemented now.)
  {
    nlohmann::json b = errOf("graph pie beertax, over(state)");
    check(b.value("code", std::string()) == "unsupported_command",
          "BUG-105(1): `graph pie` is still an honest unsupported_command");
    check(says(b, kGapSig),
          "BUG-105(1): a known gap is named as unimplemented, not as a spelling problem");
    check(!says(b, kRetiredTail) && !says(b, kRetiredDoubt),
          "BUG-105(1): the known-gap arm dropped the retired milestone wording too");
    check(!says(b, kTableSig) && !says(b, kSubSig),
          "BUG-105(1): known-gap does not also claim the other two causes");
  }

  // Arm 3 — kUnknownSubcommand: `label` is implemented, `nosuchsub` is not.
  // "PIET has no entry under that name" would be false — it has `label`.
  {
    nlohmann::json b = errOf("label nosuchsub beertax");
    check(b.value("code", std::string()) == "unsupported_command",
          "BUG-105(1): an unknown `label` subcommand is still an honest unsupported_command");
    check(says(b, kSubSig),
          "BUG-105(1): the subcommand arm says the head command IS implemented");
    check(!says(b, kRetiredTail) && !says(b, kRetiredDoubt),
          "BUG-105(1): the subcommand arm dropped the retired milestone wording too");
    check(!says(b, kTableSig) && !says(b, kGapSig),
          "BUG-105(1): unknown-subcommand does not also claim the other two causes");
  }

#ifdef _WIN32
  std::error_code uhrec;
  std::filesystem::remove_all(workdir, uhrec);
#else
  (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
}


// --- BUG-105 secondary ②: the in/weight qualifier gates fire before the
// command token has been resolved, so an unrouted word came back as a feature
// gap ("`xyzzy` does not support the `in` row-range qualifier") instead of
// "command not found". Fixed at the gate (eval/tokenizer.cpp) rather than by
// moving extractQualifiers past command resolution: the gates already REJECT
// every unrouted word (a registry miss makes allowInRange/FweightOnly false),
// so re-ordering the two facts changes the MESSAGE and nothing else — no input
// that used to run stops running.
//
// The by/bysort legs are the drift guard for the one exemption this needs:
// dispatcher.cpp routes those five spellings without a registry row, so the
// tokenizer keeps its own copy of that list. If the copies ever diverge, the
// exempted spelling starts answering unsupported_command and these legs fail.
void testBUG105InQualPrescan() {
  std::printf("[selftest] --- BUG-105 secondary (2): qualifier gate vs unknown command ---\n");

  const std::string kTableSig = "command table has no entry";
  const std::string kInGapSig = "row-range qualifier";
  const std::string kWeightGapSig = "weighted estimation";

  // An object (not a default-constructed null) so a NON-throwing line answers
  // "no code / no message" through value() instead of a json type_error.
  auto errOf = [](const std::string& line) {
    nlohmann::json b = nlohmann::json::object();
    try {
      (void)piet::eval::tokenize(line);
    } catch (const piet::eval::TokenizerError& e) {
      b = e.block;
    }
    return b;
  };
  auto says = [](const nlohmann::json& b, const std::string& needle) {
    return b.value("message", std::string()).find(needle) != std::string::npos;
  };

  // (1) unknown command + `in` -> command-not-found, NOT a feature gap. This is
  // the finding's own repro shape (BUGLOG's `l` case, pre-abbreviation-stopgap).
  {
    nlohmann::json b = errOf("xyzzy state in 1/5");
    check(b.value("code", std::string()) == "unsupported_command",
          "BUG-105(2): unknown command + `in` is an honest unsupported_command");
    check(says(b, kTableSig) && !says(b, kInGapSig),
          "BUG-105(2): it names PIET's command table, not a made-up `in` limitation");
  }
  // Same defect one gate up: the weight qualifier invented a feature gap too.
  {
    nlohmann::json b = errOf("xyzzy state [aw=pop]");
    check(b.value("code", std::string()) == "unsupported_command",
          "BUG-105(2): unknown command + [aw=] is an honest unsupported_command");
    check(says(b, kTableSig) && !says(b, kWeightGapSig),
          "BUG-105(2): the weight gate no longer claims an unrouted word lacks weight support");
  }

  // (2) known commands are untouched — both the legal and the rejected side.
  {
    piet::eval::ParsedCommand pc = piet::eval::tokenize("list id year in 1/5");
    check(pc.inRange && *pc.inRange == "1/5",
          "BUG-105(2): a routed command that ALLOWS `in` still parses it (no regression)");
  }
  {
    nlohmann::json b = errOf("tabstat wage in 1/5");
    check(b.value("code", std::string()) == "unsupported_option" && says(b, kInGapSig) &&
              says(b, "tabstat"),
          "BUG-105(2): a routed command that REJECTS `in` keeps its feature-gap message");
  }
  {
    // `regress` moved to WeightPolicy::AllWeights in the P3 weights wave
    // ([aw=] is now legal there — see testBS096QualifierPolicyTable), and `corr`,
    // its first replacement, serves [aw=]/[fw=] since BUG-1014 (help correlate;
    // w2aj_qualifiers.log:1869 qf_co_aw rc 0), and `tobit`, its next, serves all four
    // types since BUG-1544. `reg3` (WeightPolicy::None, while help reg3 allows
    // aweights and fweights) is the same "routed command that REJECTS a weight" shape
    // the test name promises: an engine gap, not Stata's rule.
    nlohmann::json b = errOf("reg3 y x1 [aw=x2]");
    check(b.value("code", std::string()) == "unsupported_option" && says(b, kWeightGapSig),
          "BUG-105(2): a routed command that REJECTS a weight keeps its feature-gap message");
  }
  // by/bysort: routed WITHOUT a registry row, so the outer token must stay
  // exempt from the unknown-command verdict on every spelling -- and (SUM-BYWEIGHT)
  // a weight there belongs to the sub-command, which the by-driver re-tokenizes:
  // the outer pass passes it through (w2as_summary_stats_family sx_cumul_by_equal
  // `by d, sort: cumul y [aw=aw], gen(cyb) equal` rc 0).
  for (const std::string& sp : {"bysort", "by", "bys", "byso", "bysor"}) {
    nlohmann::json b = errOf(sp + " grp: summarize wage [fw=x1]");
    check(b.empty(),
          "BUG-105(2): `" + sp + "` prefix is neither an unknown command nor a weight gate "
          "(tokenizer/dispatcher by-list drift guard; the sub-command's row judges the weight)");
  }

  // (3) the quoted-`in` trap: a literal " in " inside a string is not a
  // qualifier, so neither gate may fire and no verdict may be reached at all
  // ([[piet-quote-aware-qualifier]] / BUG-115 class).
  {
    piet::eval::ParsedCommand pc = piet::eval::tokenize("gen s1 = \"value in range\"");
    check(!pc.inRange, "BUG-105(2): a quoted ` in ` is still not read as an `in` qualifier");
  }
  {
    // Same trap on the arm that would now answer unsupported_command: an
    // unrouted command with a quoted `in` must fall through to the dispatcher
    // untouched by these gates (it errors there, not here).
    piet::eval::ParsedCommand pc = piet::eval::tokenize("xyzzy \"a in b\"");
    check(!pc.inRange && pc.cmd == "xyzzy",
          "BUG-105(2): the new unknown-command verdict is reachable ONLY behind a real qualifier");
  }
}


void testBS100RcCoarseMap() {
  std::printf("[selftest] --- BS-100 rc coarse map behind `capture`'s _rc (replaces the old "
              "boolean-collapsed _rc=1 hardcode) ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_bs100").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  // unsupported_command -> 199 (Stata's own r(199) "unrecognized command").
  {
    std::string wd = base + "/cmd";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("sysuse fatalities, clear", session);  // else the no-data guard yields r(111), not the unsupported_command -> 199 path under test
    piet::dispatch("capture this_is_not_a_real_stata_command_xyz", session);
    check(session.eval.locals.get("_rc", "") == "199",
          "BS-100: an unrecognized command inside `capture` sets _rc=199 "
          "(coarse: unsupported_command)");
  }

  // unsupported_option -> 198 (Stata's own r(198) "invalid syntax").
  {
    std::string wd = base + "/opt";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("sysuse fatalities, clear", session);
    piet::dispatch("capture regress fatal beertax, floob", session);
    check(session.eval.locals.get("_rc", "") == "198",
          "BS-100: a bogus option inside `capture` sets _rc=198 (coarse: unsupported_option)");
  }

  // no_data_loaded -> 111 (closest coarse analog: variable/dataset not found).
  {
    std::string wd = base + "/nodata";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("capture regress y x", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "BS-100: `regress` with no dataset loaded, inside `capture`, sets _rc=111 "
          "(coarse: no_data_loaded)");
  }

  // engine_error naming a missing FILE -> 601 (file not found).
  {
    std::string wd = base + "/filenf";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("capture use \"" + wd + "/does_not_exist_xyz.dta\"", session);
    check(session.eval.locals.get("_rc", "") == "601",
          "BS-100: `use` on a nonexistent file, inside `capture`, sets _rc=601 "
          "(coarse: engine_error naming a missing file)");
  }

  // engine_error naming an existing file without `replace` -> 602 (file
  // already exists).
  {
    std::string wd = base + "/fileexists";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("sysuse fatalities, clear", session);
    std::string target = wd + "/dup.hbin";
    piet::dispatch("save \"" + target + "\"", session);           // first save: succeeds
    piet::dispatch("capture save \"" + target + "\"", session);   // second: no replace -> exists
    check(session.eval.locals.get("_rc", "") == "602",
          "BS-100: `save` onto an existing file without `replace`, inside `capture`, sets "
          "_rc=602 (coarse: engine_error naming an existing file)");
  }

  // The engine's absent-column error is Stata's r(111) "variable not found":
  // `eststo: regress y nosuchvar` and `eststo bad: regress y nosuchvar` are
  // r(111) in StataBE (aer_w2 w2ak_estout_family.log:219, :227). It used to be
  // pinned here as the generic _rc=1 (FU-1079 moved it, W2AK).
  {
    std::string wd = base + "/generic";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    piet::dispatch("sysuse fatalities, clear", session);
    piet::dispatch("capture regress fatal nonexistentvar_bs100_xyz", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "BS-100: an engine \"column not found\" error is _rc=111, Stata's variable-not-found "
          "(w2ak_estout_family.log:219)");
  }

  fs::remove_all(base, ec);
}


void testC1N3CoarseRcGaps() {
  std::printf("[selftest] --- C1/N3 coarseRc gaps: variable-not-found -> 111, confirm "
              "type-mismatch -> 7 ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_n3").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // variable-not-found (summarize) -> r(111), oracle-pinned.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture summarize nonexistvar_c1n3", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "C1/N3: `capture summarize <missing var>` sets _rc=111 (was falling to the generic "
          "\"1\" default)");
  }

  // confirm variable not found -> r(111) too (same class, different
  // message shape: "variable 'X' not found in dataset").
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture confirm variable nonexistvar_c1n3", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "C1/N3: `capture confirm variable <missing var>` sets _rc=111");
  }

  // confirm string/numeric type mismatch -> r(7), oracle-pinned (`confirm
  // string variable income` on a numeric var -> rc=7).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture confirm string variable income", session);
    check(session.eval.locals.get("_rc", "") == "7",
          "C1/N3: `capture confirm string variable income` (income is numeric) sets _rc=7");
  }

  // Regression guard: file-not-found (601) is NOT swallowed by the new
  // "variable"+"not found" substring rule.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture use \"" + workdir + "/does_not_exist_c1n3.dta\"", session);
    check(session.eval.locals.get("_rc", "") == "601",
          "C1/N3 regression guard: file-not-found still maps to _rc=601");
  }

  // testBS100's own case: a cobra "column not found" error (regress on a
  // missing X) is r(111), as in StataBE (w2ak_estout_family.log:219, :227;
  // FU-1079, W2AK) -- it was pinned at the generic _rc=1 before.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("capture regress fatal nonexistentvar_c1n3_xyz", session);
    check(session.eval.locals.get("_rc", "") == "111",
          "C1/N3: a cobra \"column not found\" engine error is _rc=111 "
          "(w2ak_estout_family.log:219)");
  }

  fs::remove_all(workdir, ec);
}


void testC2BS031SilentIfDropSweep() {
  std::printf("[selftest] --- C2/BS-031 silent-if/in-drop sweep (duplicates tag/list, recode, "
              "clonevar, string gen/replace `in`) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c2_bs031").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto freshData = [&]() {
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen high = (beertax > .5)", session);
  };

  // (a) duplicates tag ... if ..., gen() -- oracle (live-verified this
  // stint, StataBE): N=48 (if-sample size), mean=min=max=47 (every one of
  // the 48 in-sample rows shares year==1982 with the other 47), the other
  // 288 out-of-sample rows are MISSING (dataset row count unchanged: 336).
  freshData();
  {
    auto tagBlocks = piet::dispatch("duplicates tag year if year==1982, gen(dtag)", session);
    const nlohmann::ordered_json* mut = nullptr;
    for (const auto& b : tagBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "data_mutation") mut = &b;
    }
    check(mut != nullptr && mut->value("n_rows_after", -1) == 336,
          "C2/BS-031(a): `duplicates tag ... if ..., gen(dtag)` keeps all 336 rows (missing "
          "outside the sample, not dropped)");
    auto sumBlocks = piet::dispatch("summarize dtag", session);
    const nlohmann::ordered_json* sumBlock = nullptr;
    for (const auto& b : sumBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "summarize") sumBlock = &b;
    }
    check(sumBlock != nullptr && sumBlock->contains("stats") &&
              (*sumBlock)["stats"].contains("dtag"),
          "C2/BS-031(a): `summarize dtag` returns dtag stats");
    if (sumBlock != nullptr && sumBlock->contains("stats") && (*sumBlock)["stats"].contains("dtag")) {
      const auto& st = (*sumBlock)["stats"]["dtag"];
      check(st.value("N", -1.0) == 48.0,
            "C2/BS-031(a): dtag N==48 (if-sample size, oracle-pinned), got " +
                std::to_string(st.value("N", -1.0)));
      check(std::abs(st.value("mean", -1.0) - 47.0) < 1e-9,
            "C2/BS-031(a): dtag mean==47 (oracle-pinned)");
      check(std::abs(st.value("min", -1.0) - 47.0) < 1e-9 &&
                std::abs(st.value("max", -1.0) - 47.0) < 1e-9,
            "C2/BS-031(a): dtag min==max==47 (every in-sample row has the same 48-member group)");
    }
  }

  // (a-cont) duplicates list ... if ... -- live-verified this stint: within
  // the year==1982 subset, `state` has no duplicates (0), `year` is all
  // duplicates (all 48 share the value).
  freshData();
  {
    auto listNoneBlocks = piet::dispatch("duplicates list state if year==1982", session);
    bool sawNoDup = false;
    for (const auto& b : listNoneBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "display" &&
          b.value("text", std::string()) == "No duplicates found.") {
        sawNoDup = true;
      }
    }
    check(sawNoDup,
          "C2/BS-031(a): `duplicates list state if year==1982` correctly reports 0 duplicates "
          "(each state appears once per year within the if-sample) -- previously ran unfiltered");

    auto listSomeBlocks = piet::dispatch("duplicates list year if year==1982", session);
    bool sawFortyEight = false;
    for (const auto& b : listSomeBlocks) {
      if (b.is_object() && b.value("note", std::string()) == "48 duplicate observations") {
        sawFortyEight = true;
      }
    }
    check(sawFortyEight,
          "C2/BS-031(a): `duplicates list year if year==1982` lists exactly the 48 in-sample "
          "rows (oracle-pinned), not all 336");
  }

  // (b) recode ... if ..., gen() -- oracle (live-verified this stint,
  // StataBE): `recode year (1982/1984=1) if high==0, gen(rec1)` ->
  // rec1==1 for 98 rows, 1985/1986/1987/1988 unchanged at 33 rows each,
  // Total (non-missing) 230; the 106 if-excluded (high==1) rows are
  // MISSING.
  freshData();
  {
    (void)piet::dispatch("recode year (1982/1984=1) if high==0, gen(rec1)", session);
    auto tabBlocks = piet::dispatch("tabulate rec1", session);
    const nlohmann::ordered_json* tab = nullptr;
    for (const auto& b : tabBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "tabulate") tab = &b;
    }
    check(tab != nullptr, "C2/BS-031(b): `tabulate rec1` returns a tabulate block");
    if (tab != nullptr && tab->contains("table")) {
      const auto& table = (*tab)["table"];
      check(table.contains("values") && table["values"].value("1.0", -1) == 98,
            "C2/BS-031(b): rec1==1 has 98 rows (oracle-pinned), got " +
                std::to_string(table["values"].value("1.0", -1)));
      check(table["values"].value("1985.0", -1) == 33 && table["values"].value("1986.0", -1) == 33 &&
                table["values"].value("1987.0", -1) == 33 && table["values"].value("1988.0", -1) == 33,
            "C2/BS-031(b): rec1's unmapped year categories (1985-1988) each keep 33 rows "
            "(oracle-pinned)");
      check(table.contains("margins") && table["margins"].value("total", -1) == 230,
            "C2/BS-031(b): rec1 non-missing total == 230 (336 - 106 if-excluded), got " +
                std::to_string(table["margins"].value("total", -1)));
    }
  }
  // `recode ... if ...` WITHOUT gen() recodes in place inside the sample and
  // leaves every other row as it was ([D] recode, option generate(): values
  // outside the if or in range are not modified). It was refused until
  // data-verbs-effects-rename-recode-label; the guard this replaces asserted
  // that refusal. Not a silent full-dataset
  // recode: the 98 in-sample rows of (b) become 1, the high==1 rows keep their
  // years.
  freshData();
  {
    auto noGenBlocks = piet::dispatch("recode year (1982/1984=1) if high==0", session);
    bool sawNoGenError = false;
    for (const auto& b : noGenBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawNoGenError = true;
    }
    auto countN = [&](const std::string& cond) {
      (void)piet::dispatch("quietly count if " + cond, session);
      const double* n = session.eval.rValues.find("N");
      return n == nullptr ? -1.0 : *n;
    };
    const double nOne = countN("year==1");
    const double nKept = countN("inrange(year, 1982, 1984)");
    check(!sawNoGenError && nOne == 98.0,
          "C2/BS-031(b): in-place `recode ... if ...` maps the 98 in-sample rows (as gen() does)");
    check(nKept > 0.0,
          "C2/BS-031(b): in-place `recode ... if ...` leaves the out-of-sample rows unchanged");
  }

  // (c) clonevar ... if ... -- oracle (live-verified this stint, StataBE):
  // `clonevar inc2 = income if year==1982` -> 48 non-missing (matching the
  // if-sample), mean matches BS-025's own independently oracle-pinned
  // year==1982 income mean (12998.3) -- clonevar is gen-class, so the other
  // 288 rows are MISSING, not dropped (n_rows_after stays 336).
  freshData();
  {
    auto cloneBlocks = piet::dispatch("clonevar inc2 = income if year==1982", session);
    const nlohmann::ordered_json* mut = nullptr;
    for (const auto& b : cloneBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "data_mutation") mut = &b;
    }
    check(mut != nullptr && mut->value("n_rows_after", -1) == 336,
          "C2/BS-031(c): `clonevar inc2 = income if year==1982` keeps all 336 rows");
    auto sumBlocks = piet::dispatch("summarize inc2", session);
    const nlohmann::ordered_json* sumBlock = nullptr;
    for (const auto& b : sumBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "summarize") sumBlock = &b;
    }
    if (sumBlock != nullptr && sumBlock->contains("stats") && (*sumBlock)["stats"].contains("inc2")) {
      const auto& st = (*sumBlock)["stats"]["inc2"];
      check(st.value("N", -1.0) == 48.0,
            "C2/BS-031(c): inc2 N==48 (oracle-pinned, if-sample size), got " +
                std::to_string(st.value("N", -1.0)));
      check(std::abs(st.value("mean", -1.0) - 12998.3) < 1.0,
            "C2/BS-031(c): inc2 mean ~= 12998.3 (oracle-pinned, matches BS-025's independently "
            "verified year==1982 income mean)");
    } else {
      check(false, "C2/BS-031(c): `summarize inc2` returns inc2 stats");
    }
  }

  // (d) string `gen ... in a/b` -- oracle (live-verified this stint,
  // StataBE): `gen str8 sv = "x" in 1/10` -> 10 rows == "x", 326 rows == ""
  // (Stata string-missing).
  freshData();
  {
    (void)piet::dispatch("gen str8 sv = \"x\" in 1/10", session);
    (void)piet::dispatch("count if sv==\"x\"", session);
    const double* nX = session.eval.rValues.find("N");
    check(nX != nullptr && *nX == 10.0,
          "C2/BS-031(d): `count if sv==\"x\"` == 10 (oracle-pinned in-range count)");
    (void)piet::dispatch("count if sv==\"\"", session);
    const double* nEmpty = session.eval.rValues.find("N");
    check(nEmpty != nullptr && *nEmpty == 326.0,
          "C2/BS-031(d): `count if sv==\"\"` == 326 (oracle-pinned out-of-range missing count)");
  }

  // (e) string `replace ... in a/b` -- oracle (live-verified this stint,
  // StataBE): starting from all-"x", `replace sv = "y" in 5/8` -> 4 rows
  // become "y", the other 332 KEEP "x" (replace-class: unchanged, not
  // missing).
  freshData();
  {
    (void)piet::dispatch("gen str8 sv = \"x\"", session);
    (void)piet::dispatch("replace sv = \"y\" in 5/8", session);
    (void)piet::dispatch("count if sv==\"y\"", session);
    const double* nY = session.eval.rValues.find("N");
    check(nY != nullptr && *nY == 4.0,
          "C2/BS-031(e): `count if sv==\"y\"` == 4 (oracle-pinned in-range count)");
    (void)piet::dispatch("count if sv==\"x\"", session);
    const double* nStillX = session.eval.rValues.find("N");
    check(nStillX != nullptr && *nStillX == 332.0,
          "C2/BS-031(e): `count if sv==\"x\"` == 332 (out-of-range rows KEPT their original "
          "value, oracle-pinned replace-class semantics)");
  }

  // Regression guard: `duplicates drop ... if ...` is now an honest reject
  // (the recombination this would need wasn't implemented this stint),
  // not a silent full-dataset dedup.
  freshData();
  {
    // `force' spelled so the guard reaches the `if' refusal (without it the
    // varlist drop is r(198) first, as measured on StataBE).
    auto dropBlocks = piet::dispatch("duplicates drop year if year==1982, force", session);
    bool sawDropError = false;
    for (const auto& b : dropBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawDropError = true;
    }
    check(sawDropError,
          "C2/BS-031 regression guard: `duplicates drop ... if ...` is an honest reject (was "
          "silently collapsing the WHOLE dataset before this sweep)");
  }

  fs::remove_all(workdir, ec);
}


// ── BUG-139: a clean Stata `exit`/`exit N` must report an honest exit code,
// not rc=1. dispatcher.cpp halts a do-file on `exit` by pushing an UNTAGGED
// error block (`code:"engine_error"`, since CONTRACT.md's CommandErrorResult
// enum has no dedicated exit code) — the SAME channel a real failure uses —
// so runDoFileHeadless's naive "any stopped_at_reason -> rc 1" mapping used
// to treat a clean stop identically to a crash. FIX-DO-EXITCODE recovers the
// requested code from the halting block's message shape (doexec.cpp's
// parseCleanExitMessage) into the new ExecuteResult::stoppedAtExitCode.
void testBUG139CleanExitCode() {
  std::printf("[selftest] --- BUG-139 clean `exit`/`exit N` reports an honest exit code ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug139").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // bare `exit` — Stata's own semantics: exit()=0.
  {
    piet::SessionState session(workdir);
    auto result = piet::doexec::executeDoSource("display \"a\"\nexit\n", session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "F4 bare `exit`: clean halt does not report an error reason");
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 0,
          "BUG-139 bare `exit`: stoppedAtExitCode == 0");
    bool sawError = false;
    for (const auto& b : result.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
    }
    check(!sawError, "F4 trailing `exit`: no error block remains in the output stream");
  }

  // `exit 3` — Stata's own semantics: exit(n)=n.
  {
    piet::SessionState session(workdir);
    auto result = piet::doexec::executeDoSource("display \"a\"\nexit 3\n", session, piet::dispatch);
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 3,
          "BUG-139 `exit 3`: stoppedAtExitCode == 3");
    check(!result.stoppedAtReason.has_value(), "F4 `exit 3`: requested exit is not an error reason");
  }

  // `exit, clear` — the trailing `, clear` must not defeat recognition.
  {
    piet::SessionState session(workdir);
    auto result = piet::doexec::executeDoSource("display \"a\"\nexit, clear\n", session, piet::dispatch);
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 0,
          "BUG-139 `exit, clear`: stoppedAtExitCode == 0 (`, clear` doesn't defeat recognition)");
    check(!result.stoppedAtReason.has_value(),
          "F4 `exit, clear`: requested exit is not an error reason");
  }

  // A genuine engine error must NOT be mistaken for a clean exit — rc must
  // stay 1 for it (the entire point of BUG-139 is honesty in BOTH directions).
  {
    piet::SessionState session(workdir);
    auto result = piet::doexec::executeDoSource("display \"a\"\nsummarize nonexistvar139\n", session,
                                                  piet::dispatch);
    check(result.stoppedAtReason.has_value(), "BUG-139 control: a real error still halts the run");
    check(!result.stoppedAtExitCode.has_value(),
          "BUG-139 control: a real error's stoppedAtExitCode stays unset (never mistaken for a "
          "clean exit)");
  }

  fs::remove_all(workdir, ec);
}


// --- BUG-350: a run that HID errors must say so ------------------------------
// A plain `capture` suppresses the error block — correct Stata behaviour, kept.
// What was missing: nothing recorded that it happened, so the 2026-08-16
// controls=yes run swallowed a `keep if: failed writing scratch CSV`, skipped a
// whole series, and still reported 47/47 artifacts and exit 0. Same family as
// BUG-340 (count passes while every artifact is empty).
//
// The counter is asserted here; the stderr line it drives is checked by the
// probe in the BUGLOG entry (stdout must stay byte-identical, which is why the
// message is NOT a new JSON key — 163 freeze fixtures compare stdout).
void testBUG350SwallowedErrors() {
  std::printf("[selftest] --- BUG-350: swallowed errors are counted ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug350").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  const int64_t before = piet::counters::swallowedErrors();
  (void)piet::dispatch("capture summarize no_such_variable_xyz", session);
  const int64_t after = piet::counters::swallowedErrors();
  check(after == before + 1,
        "BUG-350: a plain `capture` that hid an error INCREMENTS the swallowed counter (" +
            std::to_string(before) + " -> " + std::to_string(after) + ")");
  check(!piet::counters::firstSwallowedError().empty(),
        "BUG-350: the first hidden message is kept, so the summary can name one");

  // A `capture noisily` SHOWS its error — it is not hidden, so it must NOT be
  // counted here, or the summary would cry about errors the user can already see.
  const int64_t b2 = piet::counters::swallowedErrors();
  (void)piet::dispatch("capture noisily summarize also_missing_xyz", session);
  check(piet::counters::swallowedErrors() == b2,
        "BUG-350: `capture noisily` is DISPLAYED, so it is not counted as hidden");

  // And a clean command must not move it either.
  const int64_t b3 = piet::counters::swallowedErrors();
  (void)piet::dispatch("summarize beertax", session);
  check(piet::counters::swallowedErrors() == b3,
        "BUG-350: a successful command does not move the counter");

  // BUG-350 coverage gap, closed 2026-09-01 (leg-1D probe 2). `capture` as a
  // LOOP PREFIX drops the error block on a SECOND code path (loops.cpp's prefix
  // filter, not dispatcher.cpp's), and that path did not count. The counter's
  // claim is "a run that hid something says so", so a hole in it makes every
  // zero-reading weaker than it looks — including the one this probe used to
  // close the audit. Driven through routes::doString because the loop-prefix
  // filter only runs on the real .do path.
  {
    const int64_t b4 = piet::counters::swallowedErrors();
    nlohmann::json body{{"source",
                         "capture forvalues i = 1/2 {\n"
                         "    summarize no_such_variable_in_a_loop_xyz\n"
                         "}\n"}};
    (void)piet::routes::doString(session, body.dump());
    check(piet::counters::swallowedErrors() > b4,
          "BUG-350: `capture` on a LOOP prefix also counts — the second drop site "
          "(loops.cpp prefix filter) hid errors silently until 2026-09-01");
  }

  fs::remove_all(workdir, ec);
}


// --- BUG-112(8) PIET-half: cobra's non-fatal model-spec notes must REACH the
// user ------------------------------------------------------------------------
// The engine half landed 2026-07-31 (`warnings` array, absent when empty) and
// shipped INERT: nothing on this side read the key, so the note existed only in
// JSON no one parsed. Measured before the fix on the shipped B.15.0 archive —
// engine emits it on BOTH planes (one-shot CLI and `cobra serve` frame-run,
// verified by driving serve directly), and the estimation block carried nothing.
//
// The carry had to land TWICE. `regress` normalises through
// handlers.cpp::cobraToEstimation (the M2 copy); estimation_m4.cpp's
// cobraToEstimationM4 is its twin, and the M4a-only version of this fix left the
// most common estimator still silent — the twin-allowlist hazard the passthrough
// list's own comment warns about. This leg drives `regress`, i.e. the M2 path.
void testBUG112WarningsSurface() {
  std::printf("[selftest] --- BUG-112(8) PIET-half: engine warnings reach the block ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug112_warn").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto warningsOf = [](const nlohmann::ordered_json& blocks) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation" &&
          b.contains("warnings") && b["warnings"].is_array()) {
        for (const auto& w : b["warnings"]) out.push_back(w.get<std::string>());
      }
    }
    return out;
  };
  auto hasEstimation = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "estimation") return true;
    return false;
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("egen g = group(state)", session);
  (void)piet::dispatch("keep if g <= 5", session);

  // ib99. names a base level no kept row holds -> the engine falls back to the
  // last level AND says so. The fit itself must still succeed: this class is
  // warn-and-proceed (StataBE 19.5 oracle, 2026-07-31), never a reject.
  auto warned = piet::dispatch("regress fatal beertax ib99.g", session);
  if (!hasEstimation(warned)) {
    skip("BUG-112(8): engine warnings reach the block",
         "no estimation block — this environment has no frames-capable `cobra serve`, so there "
         "is no engine response to carry a warning");
  } else {
    auto ws = warningsOf(warned);
    check(!ws.empty(), "BUG-112(8): the absent-base note REACHES the estimation block");
    check(!ws.empty() && ws[0].find("identifies no observations") != std::string::npos,
          "BUG-112(8): it is the engine's own wording, not a rephrasing by this layer");
    // Absent-when-empty is the property that keeps every clean golden byte-identical.
    auto clean = piet::dispatch("regress fatal beertax", session);
    check(hasEstimation(clean) && warningsOf(clean).empty(),
          "BUG-112(8): a clean fit carries NO warnings key (goldens stay byte-identical)");
  }

  fs::remove_all(workdir, ec);
}


// --- BUG-105 secondary error-honesty: hbin jargon leak (5 sites) + collapse
// parse-error missing expected syntax ----------------------------------------
void testBUG105ErrHonestyLeaks() {
  std::printf("[selftest] --- BUG-105 secondary: err-honesty leaks (hbin jargon + collapse syntax) ---\n");
  namespace fs = std::filesystem;

  // (a) REACHABLE: `collapse` with no stat-varlist is a live, one-command
  // repro of the parse-failure branch (reCollapse requires >=1 char after
  // `collapse`, so the bare verb never matches — no data-corruption or
  // torn-write scaffolding needed).
  {
    std::string workdir =
        (fs::temp_directory_path() / "piet_native_selftest_bug105_collapse").u8string();
    std::error_code ec;
    fs::remove_all(workdir, ec);
    fs::create_directories(workdir, ec);
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 2", session);
    (void)piet::dispatch("gen double x = _n", session);
    auto blocks = piet::dispatch("collapse", session);
    std::string msg;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        msg = b.value("message", std::string());
        break;
      }
    }
    check(msg.find("cannot parse stat varlist") != std::string::npos,
          "BUG-105: bare `collapse` still reports the parse failure");
    check(msg.find("collapse (stat) varlist") != std::string::npos,
          "BUG-105: the parse-failure message now shows the expected syntax");
    fs::remove_all(workdir, ec);
  }

  // (b) DEFENSIVE-ASSERT (rarely reachable — a torn write after cobra already
  // reported success): a text-existence grep gate on the two source files
  // stands in for forced reachability ([[piet-diagnostic-honesty-classes]]).
  {
    std::string root = std::string(selftestSourceFile());
    auto pos = root.rfind("/piet/native/src/selftest.cpp");
    check(pos != std::string::npos, "BUG-105: __FILE__ resolves under piet/native/src/");
    std::string srcDir = (pos == std::string::npos) ? "." : root.substr(0, pos) + "/piet/native/src";

    auto slurp = [](const std::string& p) -> std::string {
      std::ifstream f(cecon::to_native_path(p));
      if (!f) return std::string();
      std::ostringstream ss;
      ss << f.rdbuf();
      return ss.str();
    };
    std::string strmutate = slurp(srcDir + "/strmutate.cpp");
    // Read every data writer, including the extracted string conversions, so
    // moving a writer does not remove it from this defensive-message check.
    std::string conversions = slurp(srcDir + "/handlers_string_conversion.cpp");
    std::string handlers = conversions;
    bool allDataWritersRead = true;
    for (const char* file : {
        "data_order_options.cpp",
        "dataset_save.cpp",
        "variable_copy_rename.cpp",
        "data_collapse.cpp",
        "data_order.cpp",
        "data_recode.cpp",
        "data_duplicates.cpp",
        "data_combine_options.cpp",
        "data_reshape.cpp",
        "data_append.cpp",
        "data_merge.cpp",
        "data_duplicates_route.cpp",
        "panel_descriptive_statistics.cpp",
        "time_fill_options.cpp",
        "data_joinby.cpp",
        "time_fill.cpp",
        "data_quantiles.cpp",
        "data_interpolation.cpp",
        "data_observation_expansion.cpp",
        "delimited_export.cpp",
        "delimited_import.cpp",
        "spreadsheet_export.cpp",
        "spreadsheet_import.cpp",
    }) {
      auto source = slurp(srcDir + "/" + file);
      allDataWritersRead = allDataWritersRead && !source.empty();
      handlers += source;
    }
    check(!strmutate.empty() && !conversions.empty() && allDataWritersRead,
          "BUG-105: strmutate.cpp, all data verbs and string conversions read for the grep gate");

    check(strmutate.find("hbin write failed") == std::string::npos &&
              handlers.find("hbin write failed") == std::string::npos,
          "BUG-105: the internal \"hbin write failed\" jargon no longer appears in either file");

    auto countOccurrences = [](const std::string& hay, const std::string& needle) {
      int n = 0;
      size_t p = 0;
      while ((p = hay.find(needle, p)) != std::string::npos) {
        ++n;
        p += needle.size();
      }
      return n;
    };
    const std::string wanted = "failed writing output dataset (disk full or I/O error)";
    int total = countOccurrences(strmutate, wanted) + countOccurrences(handlers, wanted);
    // BUG-371 raised the count 5 -> 6: `decode` is a new one-column `setcol`
    // writer and carries the same torn-write guard its siblings destring/tostring
    // do. The number is the point of this check — a new writer that FORGOT the
    // guard would leave it at 5 and pass, so the count moves only alongside a
    // site that provably has the text. BUG-928 raised it 6 -> 7: the stateless
    // `order --after` step that places a tostring/destring generate() column
    // right after its source (placeGeneratedAfterSource) is a new writer.
    check(total == 7,
          "BUG-105: all 7 defensive-assert sites (2 strmutate.cpp + 5 across the data-writer TUs) "
          "carry the user-facing replacement text (found " + std::to_string(total) + ")");
  }
}


// BUG-251/252 (WS-Q): no site re-derives the command word from the raw line.
//
// BUG-232's four sites located the command word inside `parsed.raw` with a
// literal spelling and indexed off the result. It repaired them by searching
// for `cmdTyped` instead — correct, but still a search, still unguarded against
// npos, and therefore still one registry edit away from the same crash.
//
// This is the seeded-RED leg for the repair. Every case below runs through
// piet::dispatch, so it exercises the real routing path, and each asserts on
// BEHAVIOUR rather than on a message string, so it survives re-wording.
//
// The load-bearing case is `u` alone. `find()` returns npos == SIZE_MAX and the
// old sites added a small constant to it; that addition WRAPS (npos + 3 == 2),
// so the defect reads from the wrong offset for any line at least that long and
// throws std::out_of_range only for a shorter one. Measured before the fix, with
// `use`'s abbreviations present: the one-character line `u` produced
// `cpp_exception: basic_string` and killed the whole --do run. Reverting
// rawArgsAfterCmd() to `lowerRaw.find("use") + 3` puts this check back to RED.
void testBUG251RawCmdRederivation() {
  std::printf("[selftest] --- BUG-251/252: no raw-line re-derivation of the command word ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug251_rawderive").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string dta = (fs::path(workdir) / "wsq251.dta").u8string();

  // Signature of a dispatched line: error codes, display text and row counts.
  // Deliberately NOT the message wording — a re-worded honest error must still
  // pass; only a crash, a silent wrong answer or a changed count may fail.
  auto sigOf = [&](piet::SessionState& session, const std::string& line) {
    std::string sig;
    try {
      auto blocks = piet::dispatch(line, session);
      for (const auto& b : blocks) {
        if (!b.is_object()) continue;
        const std::string t = b.value("type", std::string());
        if (t == "error") {
          sig += "ERR:" + b.value("code", std::string()) + "|";
        } else if (t == "display") {
          sig += "DISP:" + b.value("text", std::string()) + "|";
        } else if (t == "data_mutation") {
          sig += "MUT:" + b.value("action", std::string()) + ":" +
                 std::to_string(b.value("n_rows_after", -1)) + "|";
        }
      }
    } catch (const std::exception& exc) {
      sig += std::string("THREW:") + exc.what() + "|";
    } catch (...) {
      sig += "THREW:unknown|";
    }
    return sig;
  };
  auto fresh = [&](piet::SessionState& session) {
    (void)piet::dispatch("sysuse fatalities, clear", session);
  };

  // ---- (a) the crasher: a one-character command line must not throw ---------
  {
    piet::SessionState session(workdir);
    fresh(session);
    const std::string sig = sigOf(session, "u");
    check(sig.find("THREW:") == std::string::npos,
          "BUG-251(a): the line `u` does not throw an exception out of dispatch (was "
          "cpp_exception: basic_string, which killed the whole --do run)");
    check(sig.find("ERR:") != std::string::npos,
          "BUG-251(a): `u` with no filename gets an honest error block, not a crash and not "
          "a silent success");
  }

  // ---- (b) every `use` spelling loads the same data -------------------------
  // BUG-252: measured on StataBE 2026-08-07, `u` and `us` both answer rc 0.
  // This is also the only input that can REACH the repaired `use` arm, which is
  // why the abbreviation and the fix ship together.
  {
    piet::SessionState session(workdir);
    fresh(session);
    (void)piet::dispatch("save \"" + dta + "\", replace", session);
    const std::string full = sigOf(session, "use \"" + dta + "\", clear");
    check(full.find("MUT:open:336") != std::string::npos,
          "BUG-252 premise: `use` loads the 336-row saved file");
    for (const char* ab : {"u", "us"}) {
      piet::SessionState s2(workdir);
      fresh(s2);
      check(sigOf(s2, std::string(ab) + " \"" + dta + "\", clear") == full,
            std::string("BUG-252: `") + ab + "` loads exactly what `use` loads");
    }
  }

  // ---- (c) the other repaired arms still answer, abbreviated or not ---------
  // Each of these read its arguments by locating the command word in the raw
  // line. The pairs assert the abbreviation and the full spelling agree.
  {
    struct Pair {
      const char* abbrev;
      const char* full;
      const char* what;
    };
    const Pair pairs[] = {
        {"dis 2+2", "display 2+2", "display"},
        {"sav \"@\", replace", "save \"@\", replace", "save"},
        {"labe variable state \"t\"", "label variable state \"t\"", "label variable"},
    };
    for (const auto& p : pairs) {
      std::string a(p.abbrev), f(p.full);
      const std::string tmp = (fs::path(workdir) / "pair.dta").u8string();
      for (std::string* s : {&a, &f}) {
        size_t at = s->find('@');
        if (at != std::string::npos) s->replace(at, 1, tmp);
      }
      piet::SessionState s1(workdir);
      fresh(s1);
      piet::SessionState s2(workdir);
      fresh(s2);
      const std::string sa = sigOf(s1, a);
      const std::string sf = sigOf(s2, f);
      check(sa.find("THREW:") == std::string::npos,
            std::string("BUG-251(c): `") + p.abbrev + "` does not throw");
      check(sa == sf, std::string("BUG-251(c): abbreviated `") + p.what +
                          "` behaves exactly like the full spelling");
    }
  }

  // ---- (d) arms whose word is not abbreviable are still crash-proof ---------
  // `cd` and `log` have no Stata abbreviation (measured: `c /tmp` answers
  // r(199)), so they were latent, not live. They are asserted anyway: the point
  // of the repair is that reaching them with an unresolvable spelling can no
  // longer throw, whoever edits the registry next.
  {
    piet::SessionState session(workdir);
    fresh(session);
    for (const char* line : {"cd", "log", "pwd"}) {
      check(sigOf(session, line).find("THREW:") == std::string::npos,
            std::string("BUG-251(d): the bare line `") + line + "` does not throw");
    }
  }

  // ---- (e) grep-equivalent: the leading-token skip carries no spelling ------
  // rawArgsAfterCmd() is correct for every command precisely because it knows
  // none of them. Asserting that property directly means a future arm that
  // starts re-deriving cannot claim this test as cover.
  {
    piet::SessionState session(workdir);
    fresh(session);
    // A path that CONTAINS the command word must not confuse the argument
    // scan — the old `find()` took the first occurrence anywhere in the line.
    const fs::path sub = fs::path(workdir) / "use";
    fs::create_directories(sub, ec);
    const std::string nested = (sub / "save.dta").u8string();
    (void)piet::dispatch("save \"" + nested + "\", replace", session);
    piet::SessionState s2(workdir);
    fresh(s2);
    check(sigOf(s2, "u \"" + nested + "\", clear").find("MUT:open:336") != std::string::npos,
          "BUG-251(e): a filepath containing the words `use`/`save` still loads (the argument "
          "scan skips the leading token, it does not search the line)");
}
}
