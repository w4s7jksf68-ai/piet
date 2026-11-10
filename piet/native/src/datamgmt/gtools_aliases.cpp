// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// gtools / ftools -- AER-SCOPE1 bundle C (2026-09-03).
//
// Ten community command names that are drop-in FAST replacements for commands
// PIET already implements. The campaign therefore spends ONE translation step
// instead of ten reimplementations: fold the head onto the native name, and
// decide -- by name, never by silence -- what happens to every option gtools
// adds on top of it.
//
// The three-way split, and why each arm is what it is:
//
//   DROP     The option describes SPEED or PRESENTATION only. `fast`, `verbose`,
//            `silent`, `nochecks`. The native answer is identical without them,
//            so forwarding them to a native net that has never heard of them
//            would turn a correct command into r(198) for no reason.
//
//   RENAME   Same meaning, different spelling (`greshape, by()` is `reshape,
//            i()`). Renaming is not cosmetic: the native handler compares
//            canonical keys, so an unrenamed key is ACCEPTED by nobody or, far
//            worse, accepted by the reject net and then never read.
//
//   REFUSE   Forwarding would change the ANSWER rather than the speed.
//            `gcollapse, merge` is a collapse-then-merge-back, i.e. not a
//            collapse at all; `gquantiles, by()` wants per-group cutpoints,
//            which is a different set of numbers under the same variable name.
//            These are refused BY NAME with rc 199, never dropped.
//
// Everything not in one of those three lists passes through UNTOUCHED, on
// purpose: the native command's own reject net is the grammar of record. A shim
// that also enumerated the native surface would be a second grammar, and two
// grammars drift.
//
// WHAT THIS FILE IS NOT. It is not a place to be clever about options the
// native command already handles. The only keys named below are the ones gtools
// ADDS or SPELLS DIFFERENTLY.
#include "piet/handlers/gtools_aliases.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "gtools_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace gtools_detail;

namespace {

// dispatcher.cpp's rawArgsAfterCmd, kept LOCAL. That copy lives in dispatcher's
// anonymous namespace and is not linkable from here; the rule it implements
// ("everything after the leading identifier token") carries no command-word
// knowledge at all, so this is not a second copy of a spelling set -- which is
// the thing BUG-232/BUG-251 was actually about. environment/logging_commands.cpp::runLog made
// the same call for the same reason.
//
// The npos discipline is load-bearing and is copied verbatim: a bare one-token
// line (`gisid`) has nothing after the identifier, and `substr(npos)` throws
// rather than returning empty. That threw once already, on the line `u`.
std::string rawArgsAfterCmdLocal(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return std::string();  // no leading identifier token at all
  size_t a = raw.find_first_not_of(" \t", e);
  return (a == std::string::npos) ? std::string() : strutil::trim(raw.substr(a));
}

// The pre-comma head of an argument string. eval::findUnquotedComma is the
// SAME scanner the tokenizer used to build parsed.opts from this very line, so
// reusing it guarantees the head this function returns and the options the
// caller walks came from one split point rather than two that can disagree.
// It is also already paren- and quote-aware, which is what keeps
// `percentiles(10, 50)` in one piece.
std::string headBeforeOpts(const std::string& args) {
  const long long comma = eval::findUnquotedComma(args);
  if (comma < 0) return strutil::trim(args);
  return strutil::trim(args.substr(0, static_cast<size_t>(comma)));
}

// Render an option list back into Stata option-tail text.
//
// Reassembly is NOT optional bookkeeping. runCollapse re-parses parsed.raw with
// its own `\bcollapse\b(.+?)` regex to recover the stat list, and runMerge
// re-parses it with `^\s*merge\s+(1:1|m:1|1:m)` to recover the merge type. An
// untouched raw line would still say `gcollapse` / `fmerge` there, and those
// two regexes would then match nothing (merge: an immediate syntax error) or
// the wrong span (collapse). The rewritten head has to reach them.
//
// eval::parseOpts only ever mints OptValue::ofBool(true) for a bare flag
// (tokenizer.cpp:373), so `isBool` means "present" and needs no value.
std::string serialiseOpts(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string out;
  for (const auto& kv : opts) {
    if (!out.empty()) out += " ";
    out += kv.first;
    if (!kv.second.isBool) out += "(" + kv.second.strValue + ")";
  }
  return out;
}

// ---------------------------------------------------------------------------
// (1)(2) The alias table, its option policy, and its option vocabulary.
// ---------------------------------------------------------------------------

struct AliasPolicy {
  std::string native;  // the command the head folds onto
  // The native command's OWN option vocabulary, COPIED rather than read back
  // from eval::findOptVocab(native) at registration time. Static-init order
  // across translation units is unspecified, and this file's registration must
  // be complete before dispatcher.cpp's canonicalizeForCommand runs on the
  // FIRST gtools line -- so a lazy read is too late and an eager read could see
  // an empty vocabulary. An empty vocabulary here is exactly defect class 3
  // (the option resolves to nothing, the reject net still accepts the prefix,
  // and the handler's canonical-key compare never matches), so the copy is the
  // safe side of that trade. Sources, to be edited in step with these:
  //   collapse   -- dispatcher.cpp's kVocabCollapse; the collapse arm runs a
  //                 net of its own per flavor, and runCollapse reads the
  //                 option keys the dispatcher resolved.
  //   egen       -- egen.cpp:38
  //   duplicates, levelsof, xtile -- dispatcher.cpp's file-scope block
  //   isid       -- dispatcher.cpp's arm rejects every option (empty vocabulary)
  //   reshape, merge -- data_reshape.cpp's and data_merge.cpp's file-scope blocks
  std::vector<std::string> nativeKnown;
  std::vector<std::string> nativeUnimplemented;
  // Surface gtools ADDS that the native command has no equivalent for and that
  // this file itself reads (the gquantiles mode words, gstats' own options).
  std::vector<std::string> extraKnown;
  std::vector<std::string> extraUnimplemented;
  std::vector<std::string> drop;
  std::vector<std::pair<std::string, std::string>> rename;  // from -> to
  std::vector<std::string> refuse;
};

const std::unordered_map<std::string, AliasPolicy>& aliasPolicies() {
  static const std::unordered_map<std::string, AliasPolicy> kTable = [] {
    std::unordered_map<std::string, AliasPolicy> t;

    // gcollapse / fcollapse -> collapse.
    // `merge` heads the refuse list because it is the one that looks most like a
    // modifier and is least like one: it collapses and then merges the result
    // BACK onto the uncollapsed data, so the row count after it is the row count
    // before, which is the opposite of what every consumer of a collapse assumes.
    //
    // `double` asks for a different storage width of the collapsed results,
    // which PIET cannot honour. It is refused by name rather than dropped:
    // dropping it would report success for something it did not do, a silent
    // answer change rather than a silent speed change.
    // `cw` is casewise deletion in all three (gcollapse.ado and fcollapse.ado
    // both declare it); runCollapse reads it off the option keys
    // (w2au_collapse_family.log cz_g_cw rc 0). `fast` and `verbose` reach the
    // collapse arm, which reads neither but answers either given an argument
    // r(198), as gtools does (BUG-1100, StataBE 19.5: `option fast()
    // incorrectly specified`, `option verbose() incorrectly specified`).
    const AliasPolicy collapseLike{
        "collapse",
        {"by", "cw"},
        {},
        {"fast", "verbose"},
        {},
        {},
        {},
        {"merge", "labelformat", "labelprogram", "freq", "rawstat", "unsorted", "missing",
         "wild", "sumcheck", "double", "pool", "register"}};
    t.emplace("gcollapse", collapseLike);
    // fcollapse (ftools, fcollapse) is its own row (BUG-1100): `fast`
    // reaches the collapse arm, where `smart` reads it, and nocompress (and its
    // positive spelling `compress`, the default), append and smart are served
    // there; merge, freq/freqvar(), register(), pool() and method() are refused
    // by name. gtools' own words are no fcollapse options at all, so the
    // collapse arm's net answers them r(198), as fcollapse does. No option
    // abbreviates below its syntax-line minimum (dispatcher.cpp
    // kOptMinCompleteLater), so a key reaches this row spelled in full.
    // `verbose` reaches the arm too, which prints nothing for it and answers
    // `verbose(1)` or `v(1)` r(198), as fcollapse does.
    t.emplace("fcollapse", AliasPolicy{"collapse",
                                       {"by", "cw", "fast"},
                                       {},
                                       {"nocompress", "compress", "append", "smart", "verbose"},
                                       {},
                                       {},
                                       {},
                                       {"merge", "freq", "freqvar", "register", "pool", "method"}});

    // gegen -> egen. `replace` overwrites an existing target, `counts`/`fill`
    // change what is computed; none of the three is a speed knob.
    t.emplace("gegen", AliasPolicy{"egen",
                                   {"by", "missing", "p"},
                                   {},
                                   {},
                                   {},
                                   {},
                                   {},
                                   {"replace", "counts", "fill"}});

    t.emplace("gduplicates", AliasPolicy{"duplicates",
                                         {"gen", "generate"},
                                         {},
                                         {},
                                         {},
                                         {"force"},
                                         {},
                                         {}});

    // glevelsof -> levelsof. `numfmt` changes how each level is SPELLED in the
    // returned macro, `unsorted` changes the order it is returned in, and `gen`
    // creates a variable levelsof would not have created: all three are visible
    // in the answer, none is a speed knob.
    t.emplace("glevelsof", AliasPolicy{"levelsof",
                                       {"local", "clean", "missing", "separate"},
                                       {},
                                       {},
                                       {},
                                       {"silent"},
                                       {},
                                       {"numfmt", "unsorted", "gen"}});

    // gisid -> isid. `missok` makes missing values legal keys, which is the
    // difference between "this is an id" and "this is not".
    t.emplace("gisid", AliasPolicy{"isid", {}, {}, {}, {}, {"fast"}, {}, {"missok"}});

    // greshape -> reshape. by()/keys() are i()/j() under different names.
    // `dropmiss` silently deletes rows reshape would have kept.
    t.emplace("greshape", AliasPolicy{"reshape",
                                      {"i", "j"},
                                      {},
                                      {},
                                      {},
                                      {"string", "nochecks", "fast"},
                                      {{"by", "i"}, {"keys", "j"}},
                                      {"dropmiss"}});

    // fmerge -> merge. uniqmaster/uniqusing assert a uniqueness the native
    // merge does not check, so accepting them would promise a check nobody
    // runs; `nolabel` changes the labels carried across.
    t.emplace("fmerge", AliasPolicy{"merge",
                                    {"keepusing", "nogenerate", "keep", "assert"},
                                    {"generate", "nolabel", "nonotes", "update", "replace",
                                     "noreport", "force", "sorted"},
                                    {},
                                    {},
                                    {},
                                    {},
                                    {"uniqmaster", "uniqusing", "nolabel"}});

    // gquantiles. The native twin depends on the MODE word (see readModeWord
    // below): xtile has one, _pctile and pctile do not and get their own arm in
    // this file. The vocabulary is registered once, for the alias name, and
    // covers both destinations.
    //
    // n()/nq() are gtools spellings of nquantiles(). `strict` only controls
    // whether gtools errors on a degenerate request, not what the answer is.
    // Everything in the refuse list changes the numbers: by() computes
    // per-group cutpoints, altdef switches to the alternate quantile
    // definition, cutpoints/cutoffs/cutquantiles/quantiles supply the bin edges
    // from somewhere other than the data, genp/binfreq/minmax create extra
    // variables, method picks a different algorithm, replace overwrites, and
    // dedup drops duplicate cutpoints so the bin COUNT stops matching nq.
    t.emplace("gquantiles",
              AliasPolicy{"xtile",
                          {"nquantiles", "cutpoints"},
                          {"altdef"},
                          {"percentiles", "xtile", "_pctile", "pctile", "n", "nq"},
                          {},
                          {"strict"},
                          {{"n", "nquantiles"}, {"nq", "nquantiles"}},
                          {"by", "altdef", "cutpoints", "quantiles", "cutoffs", "cutquantiles",
                           "genp", "binfreq", "method", "replace", "dedup", "minmax"}});

    // gstats is NOT an alias: it is a multiplex over two different native
    // handlers plus a set of refused sub-commands, and it keeps its own name all
    // the way to its own arm. It is in this table only so its options get a
    // vocabulary (defect class 3) and so the raw line is reassembled by the same
    // code path as every other row. by() is UNIMPLEMENTED rather than unknown:
    // runWinsor2 recognises it and refuses it, so "not yet implemented" is the
    // honest wording and "not allowed" is not.
    //
    // `label` is DROPPED, not refused. It only controls whether gstats attaches
    // a variable label to the winsorized output, which no number in the block
    // depends on -- and a refusal for a presentation-only key is the same size
    // of mistake as silently ignoring one that changes the answer, in the other
    // direction. The drop is alias-wide rather than winsor-only, so `gstats sum
    // ..., label` is accepted-and-dropped where gtools would reject it; that
    // widening is deliberate and is the cheaper half of the trade.
    t.emplace("gstats", AliasPolicy{"gstats",
                                    {},
                                    {},
                                    {"cuts", "suffix", "trim", "replace", "gen", "detail"},
                                    {"by"},
                                    {"label"},
                                    {},
                                    {}});
    return t;
  }();
  return kTable;
}

const AliasPolicy* findPolicy(const std::string& name) {
  const auto& t = aliasPolicies();
  auto it = t.find(name);
  return it == t.end() ? nullptr : &it->second;
}

// Defect class 3, closed for all ten names.
//
// Without a registered vocabulary under the ALIAS token, dispatcher.cpp's
// canonicalizeForCommand (which runs on `cmd` BEFORE this file's translation
// step) has nothing to resolve against: `gquantiles x, _pctile nq(4)` reaches
// the walk below with the key still spelled `nq`, `gstats winsor x, cut(1 99)`
// with `cut`, and the canonical-key comparisons never match. Meanwhile
// rejectUnknownOptions downstream still ACCEPTS the prefix. That combination is
// the shape that shipped `ivreg, ro` running without robust standard errors.
//
// known = native pass-through + what gtools adds + what we drop + what we
// rename FROM, minus anything refused; unimplemented = the native command's own
// unimplemented set + the refused keys. The refused keys have to be resolvable
// too, or `gcollapse, labelfor(...)` would slip past the refusal by being an
// abbreviation of a name the vocabulary never heard of.
bool registerGtoolsVocabs() {
  for (const auto& kv : aliasPolicies()) {
    const AliasPolicy& p = kv.second;
    std::unordered_set<std::string> refused(p.refuse.begin(), p.refuse.end());
    eval::OptVocab v;
    for (const auto& k : p.nativeKnown) {
      if (refused.count(k) == 0) v.known.insert(k);
    }
    for (const auto& k : p.extraKnown) {
      if (refused.count(k) == 0) v.known.insert(k);
    }
    for (const auto& k : p.drop) v.known.insert(k);
    for (const auto& r : p.rename) v.known.insert(r.first);
    for (const auto& k : p.nativeUnimplemented) v.unimplemented.insert(k);
    for (const auto& k : p.extraUnimplemented) v.unimplemented.insert(k);
    for (const auto& k : p.refuse) v.unimplemented.insert(k);
    // registerOptVocab takes an initializer_list<const char*>, so the token has
    // to outlive the call; kv.first is a key in the function-local static map
    // above and does.
    eval::registerOptVocab({kv.first.c_str()}, std::move(v));
  }
  return true;
}

// Static init, deliberately: dispatcher.cpp canonicalizes options at line ~1841
// and only reaches isGtoolsAlias at ~1888, so a registration deferred to first
// use would miss the first gtools command of the session.
const bool kGtoolsVocabsRegistered = registerGtoolsVocabs();

// Refusals for a translated option. The token is `alias, key()` so a reader
// sees WHICH option on WHICH command was refused, and the reason field carries
// `refused:<alias>_<key>` so the registry drift guard can tell a refusal apart
// from an unrecognised token.
//
// eval::unsupportedCommand emits code "unsupported_command", which is the ONE
// code eval::coarseRc answers 199 for. Building this with the default
// "engine_error" would silently downgrade `capture gcollapse ..., merge(x)` to
// _rc 1, and a do-file branching on 199 would take the wrong branch.
std::string refusalRecipe(const std::string& alias, const std::string& key) {
  if ((alias == "gcollapse" || alias == "fcollapse") && key == "merge") {
    return " `merge` collapses and then merges the result back onto the "
           "uncollapsed data, so the row count is unchanged -- that is not a collapse. Write it "
           "out: `preserve`, then the `collapse`, then `save` to a tempfile, then `restore`, then "
           "`merge m:1 <by-vars> using <tempfile>`.";
  }
  return std::string();
}

ojson refuseOption(const std::string& alias, const std::string& key) {
  ojson block = ojson(eval::unsupportedCommand(alias + ", " + key + "()", std::nullopt,
                                                std::nullopt, "refused:" + alias + "_" + key,
                                                eval::UnsupportedCommandKind::kNotImplemented));
  const std::string recipe = refusalRecipe(alias, key);
  // Appended to the message the helper already built rather than passed as a
  // new field: `message` is the one string every renderer shows, and a key the
  // block schema does not carry would be dropped on the floor.
  if (!recipe.empty() && block.contains("message") && block["message"].is_string()) {
    block["message"] = block["message"].get<std::string>() + recipe;
  }
  return block;
}

// Stata's xtile defaults to nquantiles(2); the engine's --xtile flag defaults
// to 4. That is BUG-370, and it is why the xtile rewrite below ALWAYS emits an
// explicit nquantiles() instead of forwarding the user's option list as typed:
// a missing nq() would be answered by the engine's 4 and reported as a success.
// Same constant, same reason, as kXtileDefaultNq in data_io_internal.h.
constexpr long long kGquantilesDefaultNq = 2;

}  // namespace

// ---------------------------------------------------------------------------
// (1) Name folding.
// ---------------------------------------------------------------------------

bool isGtoolsAlias(const std::string& canonical) {
  // The reference is what keeps the registration from being elided as an unused
  // internal-linkage constant, and it records the ordering this file depends on:
  // the vocabularies must already be in the registry when dispatcher.cpp
  // canonicalizes options, which happens BEFORE it asks this question.
  (void)kGtoolsVocabsRegistered;
  // True for gquantiles and gstats as well, even though neither folds onto a
  // differently-named native command: both still need the translation step to
  // normalise their options and to reassemble parsed.raw.
  return findPolicy(canonical) != nullptr;
}

std::string gtoolsCanonicalName(const std::string& canonical) {
  const AliasPolicy* p = findPolicy(canonical);
  // gquantiles maps to itself here, NOT to xtile: which native command it is
  // depends on the mode word, and this fold is name-only by contract (the
  // by-prefix path calls it before any line has been parsed for options).
  if (p == nullptr) return canonical;
  if (canonical == "gquantiles" || canonical == "gstats") return canonical;
  return p->native;
}

// ---------------------------------------------------------------------------
// (2) Translation.
// ---------------------------------------------------------------------------

GtoolsTranslation translateGtoolsAlias(const eval::ParsedCommand& parsed,
                                       const std::string& canonical, SessionState& session) {
  // Unused by every row today. It stays in the signature because the dispatcher
  // hands every translation the same three arguments, and an alias whose
  // refusal depends on session state (a mode that is only legal with a panel
  // set, say) would need it without a call-site change.
  (void)session;

  GtoolsTranslation out;
  out.parsed = parsed;
  out.canonical = canonical;

  const AliasPolicy* pol = findPolicy(canonical);
  if (pol == nullptr) {
    // Unreachable through the dispatcher, which gates this call on
    // isGtoolsAlias. The identity is the only honest answer for a name this
    // file does not translate -- it is not a no-op standing in for a decision.
    return out;
  }

  const std::unordered_set<std::string> dropSet(pol->drop.begin(), pol->drop.end());
  const std::unordered_set<std::string> refuseSet(pol->refuse.begin(), pol->refuse.end());
  std::unordered_map<std::string, std::string> renameMap;
  for (const auto& r : pol->rename) renameMap.emplace(r.first, r.second);

  // The mode word decides which native command gquantiles becomes, so it is
  // read before the option walk (the walk has to know whether the mode flag is
  // an option to carry or one to strip).
  ModeWord mode;
  if (canonical == "gquantiles") {
    mode = readModeWord(parsed);
    if (mode.conflict) {
      out.error = gtErr(
          "gquantiles: `xtile`, `_pctile` and `pctile` are three different commands, not "
          "modifiers of one -- specify exactly one of them.",
          "syntax_error");
      return out;
    }
    if (mode.mode.empty()) {
      out.error = gtErr(
          "gquantiles: a mode is required -- write one of `xtile`, `_pctile` or `pctile` "
          "(syntax: `gquantiles [newvar =] varname, {xtile|_pctile|pctile} [nquantiles(#)]`).",
          "syntax_error");
      return out;
    }
  }

  // The mode flag is stripped from the option list ONLY for the xtile form: the
  // native xtile arm's reject net knows nothing about a bare `xtile` option and
  // would answer r(198) for it. The _pctile/pctile forms keep it, because it is
  // the only thing that tells runGQuantilesPctile which of the two it is.
  const bool stripMode = (canonical == "gquantiles" && mode.mode == "xtile");

  std::vector<std::pair<std::string, eval::OptValue>> translated;
  translated.reserve(parsed.opts.size());
  bool haveNquantiles = false;
  for (size_t i = 0; i < parsed.opts.size(); ++i) {
    const std::string& key = parsed.opts[i].first;
    if (stripMode && mode.fromOpts && i == mode.optsIndex) continue;
    if (refuseSet.count(key) != 0) {
      out.error = refuseOption(canonical, key);
      return out;
    }
    if (dropSet.count(key) != 0) continue;  // speed/presentation only
    auto ren = renameMap.find(key);
    const std::string outKey = (ren == renameMap.end()) ? key : ren->second;
    if (outKey == "nquantiles") haveNquantiles = true;
    translated.emplace_back(outKey, parsed.opts[i].second);
  }

  std::string native = pol->native;
  if (canonical == "gquantiles") {
    if (mode.mode == "xtile") {
      out.canonical = "xtile";
      native = "xtile";
      // BUG-370, stated as a rule rather than a fix: the handler owns the
      // default because the engine's differs from Stata's. Forwarding the
      // option list as typed answers nq(4) to a question Stata answers with 2,
      // reports success, and looks right.
      if (!haveNquantiles) {
        translated.emplace_back("nquantiles",
                                eval::OptValue::ofString(std::to_string(kGquantilesDefaultNq)));
      }
    } else {
      // _pctile / pctile have no native twin; the gquantiles arm in the
      // dispatcher routes them to runGQuantilesPctile below.
      out.canonical = "gquantiles";
      native = "gquantiles";
    }
  }

  // Rebuild the varlist, dropping the mode word when it was written before the
  // comma and this is the xtile form (the xtile arm reads joinVarlist and would
  // otherwise see "xtile new = x" and take "xtile new" for the new variable).
  std::vector<std::string> varlist = parsed.varlist;
  bool droppedFromVarlist = false;
  if (stripMode && mode.fromVarlist && mode.varlistIndex < varlist.size()) {
    varlist.erase(varlist.begin() + static_cast<std::ptrdiff_t>(mode.varlistIndex));
    droppedFromVarlist = true;
  }

  std::string head = headBeforeOpts(rawArgsAfterCmdLocal(parsed.raw));
  if (droppedFromVarlist) {
    // The head is raw text, so the token has to come out of it too or the
    // reassembled line would still read `xtile xtile new = x`. Only the FIRST
    // exact match is removed, and only for the form that strips it.
    std::vector<std::string> words = splitWords(head);
    for (size_t i = 0; i < words.size(); ++i) {
      if (strutil::lower(words[i]) != mode.mode) continue;
      words.erase(words.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    head.clear();
    for (size_t i = 0; i < words.size(); ++i) {
      if (i != 0) head += " ";
      head += words[i];
    }
  }

  std::string raw = native;
  if (!head.empty()) raw += " " + head;
  const std::string optTail = serialiseOpts(translated);
  if (!optTail.empty()) raw += ", " + optTail;

  // Both halves, or neither. dispatcher.cpp routes on the returned `canonical`
  // and the generic fallthrough REPORTS parsed.cmd, so setting only parsed.cmd
  // produced the most confusing possible symptom: an error saying `collapse` is
  // not supported, raised because the router was still looking for `gcollapse`.
  // The two fields answer different questions and have to move together.
  out.canonical = native;
  out.parsed.cmd = native;
  out.parsed.varlist = std::move(varlist);
  out.parsed.opts = std::move(translated);
  out.parsed.raw = std::move(raw);
  return out;
}

}  // namespace piet::handlers
