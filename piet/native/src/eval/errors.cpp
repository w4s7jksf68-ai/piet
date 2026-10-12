#include "piet/eval/errors.h"

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <unordered_set>

namespace piet::eval {

namespace {
const std::unordered_set<std::string>& errorCodes() {
  static const std::unordered_set<std::string> kCodes{
      "unsupported_command", "unsupported_option", "unsupported_macro",
      "unsupported_loop",    "unsupported_program", "unsupported_delimiter",
      "syntax_error",        "engine_error",         "no_data_loaded",
  };
  return kCodes;
}
}  // namespace

bool isKnownErrorCode(const std::string& code) {
  return errorCodes().count(code) != 0;
}

nlohmann::json errorBlock(const std::string& code, const std::string& message,
                           std::optional<std::string> token, std::optional<int> lineNumber,
                           std::optional<std::string> context, std::optional<std::string> reason) {
  nlohmann::json b;
  b["type"] = "error";
  b["code"] = isKnownErrorCode(code) ? code : std::string("engine_error");
  b["message"] = message;
  if (token) b["token"] = *token;
  if (lineNumber) b["line_number"] = *lineNumber;
  if (context) b["context"] = *context;
  if (reason) b["reason"] = *reason;
  return b;
}

nlohmann::json unsupportedLoop(const std::string& token, std::optional<int> lineNumber,
                                std::optional<std::string> context) {
  return errorBlock("unsupported_loop",
                     "`" + token + "` is a next-milestone feature in this version of PIET "
                     "(loops and macros arrive with the Tier-A interpreter).",
                     token, lineNumber, context);
}

nlohmann::json unsupportedMacro(const std::string& token, std::optional<int> lineNumber,
                                 std::optional<std::string> context) {
  return errorBlock("unsupported_macro",
                     "Macro reference `" + token + "` detected. This version of PIET does not expand macros "
                     "— the remainder of the command would be silently wrong. "
                     "Please substitute the value directly.",
                     token, lineNumber, context);
}

nlohmann::json unsupportedProgram(const std::string& token, std::optional<int> lineNumber,
                                   std::optional<std::string> context) {
  return errorBlock("unsupported_program",
                     "`" + token + "` (program definitions) is not supported in this version of PIET.",
                     token, lineNumber, context);
}

nlohmann::json unsupportedDelimiter(const std::string& token, std::optional<int> lineNumber,
                                     std::optional<std::string> context) {
  return errorBlock("unsupported_delimiter",
                     "`#delimit` changes the statement delimiter and is not supported in this version of PIET.",
                     token, lineNumber, context);
}

nlohmann::json unsupportedCommand(const std::string& cmd, std::optional<int> lineNumber,
                                   std::optional<std::string> context, std::string reason,
                                   UnsupportedCommandKind kind) {
  // BUG-105 secondary ① (2026-07-31): the retired second sentence — "If this is
  // a valid Stata command, it may be added in a later milestone" — was one
  // string serving three different causes, and it cast doubt on the command's
  // Stata-validity in ALL of them. For the abbreviation gaps that motivated the
  // finding that doubt is simply false: `l` and `su` ARE valid Stata, and the
  // real, fixable-in-one-place reason was that PIET's command table did not
  // enumerate the prefix. Each cause now states its own reason, so no reader
  // has to guess which one they hit.
  std::string msg = "Command `" + cmd + "` is not supported in this version of PIET.";
  switch (kind) {
    case UnsupportedCommandKind::kUnknownToken:
      msg += " PIET's command table has no entry under that name — check the spelling, or write "
             "the command out in full (PIET enumerates a fixed set of abbreviations, not every "
             "unambiguous Stata prefix).";
      break;
    case UnsupportedCommandKind::kNotImplemented:
      msg += " It is a recognized Stata feature that PIET has not implemented yet.";
      break;
    case UnsupportedCommandKind::kUnknownSubcommand:
      msg += " The command itself is implemented, but this subcommand is not — check the "
             "subcommand spelling.";
      break;
  }
  return errorBlock("unsupported_command", msg, cmd, lineNumber, context, reason);
}

nlohmann::json refusedCommand(const std::string& cmd, const std::string& cause,
                               std::optional<int> lineNumber) {
  // kNotImplemented is the honest kind for every refusal that reaches here: the
  // name IS in the command table (that is what makes it a refusal rather than a
  // miss), and what PIET does not have is the capability behind it. The cause
  // travels in `reason` so a gate can distinguish refusals from one another and
  // from the generic gap, while the user-facing sentence stays the shared one.
  return unsupportedCommand(cmd, lineNumber, std::nullopt, "refused:" + cause,
                            UnsupportedCommandKind::kNotImplemented);
}

nlohmann::json unsupportedOption(const std::string& opt, const std::string& hint,
                                  std::optional<int> lineNumber, std::optional<std::string> context,
                                  std::optional<std::string> reason) {
  std::string msg = "Option `" + opt + "` is not supported in this version of PIET.";
  if (!hint.empty()) msg += " " + hint;
  return errorBlock("unsupported_option", msg, opt, lineNumber, context, reason);
}

nlohmann::json noDataLoaded(std::optional<int> lineNumber) {
  return errorBlock("no_data_loaded",
                     "No dataset is loaded. Use `use <file>` or open a file via the toolbar first.",
                     std::nullopt, lineNumber);
}

nlohmann::json engineError(const std::string& message, const std::string& code,
                            std::optional<int> lineNumber, std::optional<std::string> context) {
  return errorBlock(isKnownErrorCode(code) ? code : std::string("engine_error"), message, std::nullopt,
                     lineNumber, context);
}

// BS-100: see errors.h's doc comment for the full mapping + rationale.
std::string coarseRc(const nlohmann::ordered_json& block) {
  // BUG-187 (P2-PERF ARC-P). When a call site KNOWS the exact Stata return
  // code, it says so in `reason` as "r(NNN)" — and until now nothing read it,
  // so the block fell through to the coarse mapping below and `_rc` came back
  // as a generic 1. A do-file branching on `if _rc == 498` (or 5, or 9) then
  // took the wrong branch off an error whose own text named the right code.
  // An exact answer outranks a category guess, so it is checked first. Only
  // this exact shape is honoured; any other reason string means "no explicit
  // code" and the mapping below still decides.
  const std::string reason = block.value("reason", std::string());
  if (reason.size() > 3 && reason.compare(0, 2, "r(") == 0 && reason.back() == ')') {
    const std::string digits = reason.substr(2, reason.size() - 3);
    if (!digits.empty() &&
        digits.find_first_not_of("0123456789") == std::string::npos) {
      return digits;
    }
  }
  std::string code = block.value("code", std::string());
  if (code == "unsupported_command") return "199";
  if (code == "syntax_error" || code == "unsupported_option" || code == "unsupported_macro" ||
      code == "unsupported_loop" || code == "unsupported_program" ||
      code == "unsupported_delimiter") {
    return "198";
  }
  if (code == "no_data_loaded") return "111";
  if (code == "engine_error") {
    std::string msg = block.value("message", std::string());
    std::string lower = msg;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool mentionsFileOrDir = lower.find("file") != std::string::npos ||
                             lower.find("directory") != std::string::npos;
    if (mentionsFileOrDir && lower.find("not found") != std::string::npos) return "601";
    if (mentionsFileOrDir && lower.find("already exists") != std::string::npos) return "602";
    // BETA campaign C4 (BS-045): `mkdir` on an already-existing directory
    // OR a directory whose parent is missing both answer this SAME message
    // in real Stata ("could not create directory <path>") — oracle-pinned
    // rc=693 for both (c-oracle-stata.log's mkdir-again case; live-probed
    // against StataNow 19.5 for the missing-parent case too). Checked
    // before the generic 601/602 mentionsFileOrDir rules above only in the
    // sense that those require different substrings ("not found"/"already
    // exists") this message never contains, so ordering here doesn't
    // actually matter — kept adjacent for readability.
    // BUG-180 (P2-PREP) adds the teardown twin: real Stata answers EVERY
    // `rmdir` failure — missing path, path is a file, directory not empty —
    // with "could not remove directory <path>" and the SAME r(693)
    // (live-probed, StataNow 19.5, this stint). One rule, both verbs.
    if (mentionsFileOrDir && (lower.find("could not create directory") != std::string::npos ||
                              lower.find("could not remove directory") != std::string::npos)) {
      return "693";
    }
    // N3 (BETA Phase C batch C1): variable-not-found previously fell all
    // the way to the generic "1" default — Stata's own r(111) "variable
    // not found" is the correct coarse bucket (oracle-pinned: both
    // `summarize nonexistvar` and `confirm variable nonexistvar` -> rc=111
    // in real Stata). Matches both message shapes already produced in this
    // codebase — "variable X not found" (summarize/generic lookup) and
    // "variable 'X' not found in dataset" (confirm variable,
    // handlers_m3.cpp's runConfirm) — without over-matching the
    // file/directory case just above (checked first, so a message that
    // happens to also mention "file" still wins 601/602 as before).
    if (lower.find("variable") != std::string::npos && lower.find("not found") != std::string::npos) {
      return "111";
    }
    // P6 wave (frames): `frame drop NOSUCHFRAME` / a frlink into an unknown
    // frame -> Stata's own r(111) as well, same "named thing does not exist"
    // class as the variable and scalar rules around it. Narrow substring
    // pairing ("frame" + "not found"), matching the one message shape
    // handlers_frames.cpp produces ("frame NAME not found").
    if (lower.find("frame") != std::string::npos && lower.find("not found") != std::string::npos) {
      return "111";
    }
    // BETA campaign C4 (BS-042): `confirm scalar NAME` on a missing scalar
    // -> Stata's own r(111) too (live-probed: "scalar notascalar not
    // found" -> rc=111). Narrow substring pairing ("scalar" + "not found"),
    // mirroring the "variable"+"not found" rule immediately above — no
    // other message in this codebase pairs those two words today.
    if (lower.find("scalar") != std::string::npos && lower.find("not found") != std::string::npos) {
      return "111";
    }
    // confirm numeric/string type-mismatch -> Stata's own r(7) "type
    // mismatch" (oracle-pinned: `confirm string variable income` on a
    // numeric var -> rc=7). Matches runConfirm's exact two message shapes.
    // BETA campaign C4 (BS-042/BS-043) widens this same r(7) "type
    // mismatch"/"invalid" bucket to three more live-probed (StataNow 19.5)
    // confirm failure shapes: `confirm integer number 5.5` ("is not an
    // integer value"), `confirm number abc`/blank ("found where number
    // expected", Stata's own exact wording, live-probed), and
    // `confirm names 9def`/`confirm name 9abc` ("invalid name", Stata's
    // own exact wording, live-probed) — all four map to r(7) in real Stata.
    if (lower.find("is not numeric") != std::string::npos ||
        lower.find("is not a string variable") != std::string::npos ||
        lower.find("is not an integer value") != std::string::npos ||
        lower.find("found where number expected") != std::string::npos ||
        lower.find("invalid name") != std::string::npos) {
      return "7";
    }
    // BETA campaign C4 (BS-042): confirm's "already defined" class (oracle:
    // `confirm new variable <existing-var>` -> rc=110) — previously
    // unreachable because handlers_m3.cpp's runConfirm never recognised
    // "new variable" as a sub-command at all (see the C1-era note this
    // replaced); FIXED this batch (runConfirm now emits an
    // "... already defined" message for the conflict), so this mapping is
    // now live, not aspirational.
    if (lower.find("already defined") != std::string::npos) {
      return "110";
    }
    if (lower.find("predicts data perfectly") != std::string::npos) {
      return "2000";
    }
    // P1 wave (`isid`): Stata answers a failed uniqueness assertion with its
    // own r(459) — oracle-pinned on StataBE 19.5 (`isid foreign` on auto.dta →
    // "variable foreign does not uniquely identify the observations", rc 459).
    // Narrow pairing on the message this codebase emits for exactly that class.
    if (lower.find("not uniquely identify the observations") != std::string::npos) {
      return "459";
    }
    // P4 wave (`by:` prefix, BUG-157): Stata answers a `by` prefix on data that
    // is not already sorted by the by-variables with r(5) "not sorted" — the
    // whole reason `bysort` exists as a separate spelling.
    if (lower.find("not sorted") != std::string::npos) {
      return "5";
    }
    // P4 wave (`assert`): a failed assertion is r(9) in Stata — the answerbook
    // pins `capture assert 0` at exactly 9, which is what makes this mapping
    // load-bearing rather than cosmetic (a generic r(1) would read as "some
    // error" and the pin would go RED).
    if (lower.find("assertion is false") != std::string::npos) {
      return "9";
    }
    // W:P8 (BUG-169): `graph export FILE, name(NOSUCH)` -> Stata's own r(693)
    // ("could not find Graph window", live-probed on StataBE 19.5). The
    // message shape is unique to graph_m4.cpp's export arm.
    if (lower.find("could not find graph window") != std::string::npos) {
      return "693";
    }
    // W:P8: `log close NOSUCH` -> Stata's own r(111) "log NOSUCH not found",
    // the same "named thing does not exist" class as the variable/scalar/frame
    // rules above. Pairs on the leading "log " that handlers_m3.cpp's runLog
    // is the only producer of.
    if (lower.rfind("log ", 0) == 0 && lower.find("not found") != std::string::npos) {
      return "111";
    }
    // BUG-657: a time-series operator with no time variable declared is Stata's
    // r(111) "time variable not set" (_ts.ado; w2_eststo_prefix.log:444-447
    // `gen L.x`, w2d_uncovered.log:376-381). The engine refuses it with its own
    // text from two producers -- src/data/frame.cpp (expression path: "... used
    // but no time variable is set") and src/core/model.cpp (model path: "... used
    // but no time variable set") -- which fell to the generic r(1) below.
    if (lower.find("time-series operator") != std::string::npos &&
        lower.find("no time variable") != std::string::npos) {
      return "111";
    }
  }
  return "1";  // historical default — every other class (BS-118: post-beta)
}

// BS-097: see errors.h's doc comment for the full design rationale.
// BUG-148 (regression, shipped in B.7.0): Stata options are ABBREVIATABLE, and every option-net
// added by the BUG-105 work compared FULL spellings only. Real-corpus break: `merge ..., nogen
// keep(match master)` — `nogen` is the documented minimal abbreviation of `nogenerate` ([D] merge)
// and appears 3x in the acceptance corpus's analysis.do, so an honest-reject net turned a working
// script into rc 198 + a skipped dependent variable. Silently ignoring it (the pre-BUG-105
// behaviour) was ALSO wrong — it created the `_merge` variable the user explicitly suppressed —
// so the fix is neither "reject" nor "ignore" but RESOLVE.
//
// Rule: an option key that is a prefix of EXACTLY ONE canonical name resolves to it. Ambiguity
// rejects (Stata does the same), and a 2-char floor keeps single letters from binding by accident.
// One-sided by construction: this can only turn a reject into an accept, never the reverse.
std::string resolveOptionAbbrev(const std::string& key,
                                const std::unordered_set<std::string>& canonical) {
  if (canonical.count(key)) return key;           // exact spelling wins, always
  if (key.size() < 2) return key;                 // too short to disambiguate — leave as-is
  const std::string* hit = nullptr;
  for (const auto& c : canonical) {
    if (c.size() > key.size() && c.compare(0, key.size(), key) == 0) {
      if (hit) return key;                        // ambiguous prefix ⇒ not resolved (reject path)
      hit = &c;
    }
  }
  return hit ? *hit : key;
}

void canonicalizeOptionKeys(std::vector<std::pair<std::string, OptValue>>& opts,
                            const std::unordered_set<std::string>& canonical) {
  for (auto& kv : opts) kv.first = resolveOptionAbbrev(kv.first, canonical);
}

// AER scope-2 B4 P0-B — AMBIGUITY IS A FACT ABOUT STATA'S VOCABULARY, NOT ABOUT
// WHAT PIET IMPLEMENTED.
//
// The two vocabularies were searched in SEQUENCE: unique-in-`known` wins, and
// only a key that resolved to nothing there was offered to `unimplemented`. So a
// rival sitting in `unimplemented` never entered the uniqueness test, and a key
// that Stata calls ambiguous silently picked a winner. Measured:
//
//   export delimited using "f.csv", no
//     StataBE r(198) -- `no` prefixes nolabel, novarnames AND noquote
//     (oracle_p.log:35-39). PIET resolved it to `nolabel` and HONOURED it, rc 0,
//     stripping the value labels the user never asked to strip. It read as inert
//     only because the one fixture in the tree that exercises it (auto.csv)
//     carries no value labels — no test could see it.
//   list make in 1/2, no
//     StataBE r(198) (oracle_p.log:68-72); PIET resolved to `noobs`.
//
// `unimplemented` is DEFINED (errors.h) as "real Stata option keys the calling
// arm has deliberately chosen not to implement" — real Stata options. So a
// prefix matching one member of each set is ambiguous IN STATA, and the two sets
// have to be searched TOGETHER. Which set the single winner lands in still
// decides the WORDING: a `known` hit is honoured, an `unimplemented` hit gets
// the honest "valid Stata option, not yet implemented" refusal.
//
// One-sided in the safe direction: searching a superset can only turn a
// resolution into an ambiguity, i.e. an accept into a reject. It never invents
// one.
std::string resolveOptionAbbrevAcross(const std::string& key,
                                      const std::unordered_set<std::string>& known,
                                      const std::unordered_set<std::string>& unimplemented) {
  if (known.count(key) || unimplemented.count(key)) return key;  // exact spelling wins, always
  if (key.size() < 2) return key;                                // below the floor — leave as-is
  const std::string* hit = nullptr;
  for (const auto* set : {&known, &unimplemented}) {
    for (const auto& c : *set) {
      if (c.size() > key.size() && c.compare(0, key.size(), key) == 0) {
        if (hit != nullptr && *hit != c) return key;  // ambiguous across the REAL vocabulary
        hit = &c;
      }
    }
  }
  return hit != nullptr ? *hit : key;
}

namespace {
// BUG-233 guard state — see errors.h. Atomic because dispatch can run off the
// UI thread; relaxed is enough, this is a diagnostic counter, not a fence.
std::atomic<unsigned long> g_lateResolutions{0};
std::mutex g_lateMu;
std::string g_lastLate;
}  // namespace

unsigned long optNetLateResolutionCount() {
  return g_lateResolutions.load(std::memory_order_relaxed);
}

void resetOptNetLateResolutionCount() {
  g_lateResolutions.store(0, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_lateMu);
  g_lastLate.clear();
}

std::string optNetLastLateResolution() {
  std::lock_guard<std::mutex> lk(g_lateMu);
  return g_lastLate;
}

std::optional<nlohmann::json> rejectUnknownOptions(
    const std::vector<std::pair<std::string, OptValue>>& opts,
    const std::unordered_set<std::string>& known,
    const std::unordered_set<std::string>& knownButUnimplemented) {
  for (const auto& [kRaw, v] : opts) {
    // resolve against BOTH vocabularies: an abbreviation of an unimplemented-but-real Stata
    // option must still get the honest "not yet implemented" wording, not "not allowed".
    //
    // BUG-264/BUG-335: unless the key is marked exactOnly. That mark means the
    // dispatcher's oracle-measured minimums table has ALREADY resolved every
    // abbreviation Stata accepts for this command, so anything still unresolved
    // here is a spelling Stata itself refuses — and this internal prefix
    // resolution is precisely the over-acceptance BUG-264 was opened for
    // (`save filename, r` silently meaning `replace`). Skipping it is what
    // narrows the surface; the first attempt at the flip only skipped
    // canonicalizeForCommand and therefore changed nothing, because THIS is the
    // decision point.
    // B4 P0-B: ONE search across both vocabularies, not two in sequence — see
    // resolveOptionAbbrevAcross. This must stay in step with
    // canonicalizeForCommand's step 2, or the BUG-233 late-resolution guard
    // below starts firing on keys that are merely resolved differently here.
    const std::string k = v.exactOnly
                        ? kRaw
                        : known.count(kRaw) ? kRaw
                        : resolveOptionAbbrevAcross(kRaw, known, knownButUnimplemented);
    // BUG-233 guard: `k != kRaw` means an abbreviation reached this net still
    // spelled the way the user typed it — i.e. canonicalizeForCommand did not
    // cover this net, and every raw-key comparison in the handler has already
    // missed it (or is about to). Behaviour below is unchanged; this only
    // records that the invariant broke, so a self-test can fail on it.
    if (k != kRaw) {
      g_lateResolutions.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lk(g_lateMu);
      g_lastLate = kRaw + "->" + k;
    }
    if (known.count(k)) continue;
    if (knownButUnimplemented.count(k)) {
      return engineError("Option `" + k + "` is a valid Stata option that this version of "
                          "PIET does not yet implement.",
                          "unsupported_option");
    }
    return engineError("option `" + k + "` not allowed", "unsupported_option");
  }
  return std::nullopt;
}

// --- BUG-233: the option vocabulary registry (see errors.h for the design) ---
namespace {
// Function-local static: safe to touch from any TU's static initializers, which
// is exactly when registerOptVocab() runs. A namespace-scope map would be a
// cross-TU initialization-order bug.
std::map<std::string, OptVocab>& optVocabRegistry() {
  static std::map<std::string, OptVocab> kReg;
  return kReg;
}
}  // namespace

const OptVocab& registerOptVocab(std::initializer_list<const char*> cmds, OptVocab v) {
  auto& reg = optVocabRegistry();
  const OptVocab* last = nullptr;
  for (const char* c : cmds) {
    OptVocab& slot = reg[std::string(c)];
    // UNION, never overwrite: two nets can serve one command token (csdid and
    // its estat sub-net). A prefix that is unique in one but ambiguous in the
    // union resolves to nothing and is left alone — today's behaviour, never a
    // mis-bind.
    slot.known.insert(v.known.begin(), v.known.end());
    slot.unimplemented.insert(v.unimplemented.begin(), v.unimplemented.end());
    // Aliases union the same way. A key registered by two nets with DIFFERENT
    // targets keeps the first (insert, not assign) — the union rule above says
    // a merge may never mis-bind, and silently re-pointing a declared spelling
    // is exactly a mis-bind.
    slot.aliases.insert(v.aliases.begin(), v.aliases.end());
    last = &slot;
  }
  if (last != nullptr) return *last;
  // No tokens declared (a net not reachable by a command name of its own —
  // it still wants one owned copy of its set, so hand back a stable empty-key
  // slot rather than a dangling reference).
  return reg[std::string()];
}

const OptVocab* findOptVocab(const std::string& cmd) {
  const auto& reg = optVocabRegistry();
  auto it = reg.find(cmd);
  return it == reg.end() ? nullptr : &it->second;
}

void canonicalizeForCommand(std::vector<std::pair<std::string, OptValue>>& opts,
                            const std::string& cmd) {
  if (opts.empty()) return;
  const OptVocab* v = findOptVocab(cmd);
  if (v == nullptr) return;
  for (auto& kv : opts) {
    // Step 0 (BUG-430): a DECLARED alias, from the package's own `syntax` line.
    // It runs before the prefix steps because it exists precisely for the keys
    // those cannot resolve — `n` prefixes both `neighbors` and `nearcount`, so
    // step 2 would call it ambiguous and reject, while geonear.ado's
    // `Neighbors(...)` declares it. An exact member of `known` never reaches
    // here (step 1 owns it), so an alias can never shadow a real option name.
    if (!v->aliases.empty() && v->known.count(kv.first) == 0) {
      auto al = v->aliases.find(kv.first);
      if (al != v->aliases.end()) {
        kv.first = al->second;
        continue;
      }
    }
    // EXACTLY rejectUnknownOptions' own three-step order, and it must stay
    // exactly that or this stops being one-sided:
    //   1. an exact member of `known` is already canonical — never touch it;
    //   2. else a unique prefix of `known`;
    //   3. else a unique prefix of the unimplemented vocabulary, so an
    //      abbreviation of a real-but-unwired Stata option still reaches the
    //      honest "not yet implemented" wording rather than "not allowed".
    //
    // Step 1 is not an optimization. Dropping it cost a freeze-matrix row this
    // stint: did_imputation has `autosample` in `known` AND `autosample_off` in
    // `unimplemented`, so a fallback that ran whenever step 2 was a no-op
    // rewrote the fully-spelled, fully-implemented `autosample` into
    // `autosample_off` and turned a working command into an honest-looking
    // reject. An exact spelling is never an abbreviation of anything.
    if (v->known.count(kv.first) != 0) continue;
    // B4 P0-B: steps 2 and 3 are ONE search across both vocabularies, because
    // ambiguity is a fact about Stata's option list and `unimplemented` holds
    // real Stata options. Searching `known` first and only then falling back
    // let a rival in `unimplemented` sit out the uniqueness test, so `export
    // delimited ..., no` — r(198) in StataBE, ambiguous among nolabel /
    // novarnames / noquote — resolved to `nolabel` and was HONOURED.
    kv.first = resolveOptionAbbrevAcross(kv.first, v->known, v->unimplemented);
  }
}

std::vector<std::string> registeredOptVocabCommands() {
  std::vector<std::string> out;
  for (const auto& kv : optVocabRegistry()) {
    if (!kv.first.empty()) out.push_back(kv.first);
  }
  return out;
}

}  // namespace piet::eval
