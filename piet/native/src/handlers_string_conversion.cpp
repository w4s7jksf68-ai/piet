// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// String/number conversion commands: encode, decode, destring and tostring.
// Moved from handlers_data_verbs2.cpp without changing command behavior.
// Value-label metadata, storage types and resident-column updates remain in
// the original handlers; the shared data-verb helpers retain one definition.
#include "piet/handlers.h"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "handlers_data_verbs_internal.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

namespace {
using storage::stataDefaultFormat;
}  // namespace

// ---------------------------------------------------------------------------
// encode STRVAR, gen(NEWVAR) [label(LBLNAME)] — C++ port of encode_op.py's
// run_encode. Folded into M4a (ROADMAP M4a, blocking the debug_all.do depth
// boundary at line 157) even though it predates this campaign's explicit
// item list.
//
// L1/M3 (CSV-shim retirement): this calls cobra's native `encode` verb
// (`--var SRC --gen NEW`), which appends NEW as an f64 column (codes in
// alphabetical level order, 1-based, missing preserved) and byte-preserves
// the source and every other column — no whole-frame CSV dump, no
// re-factorization. NEW lands f64, not factor, exactly the oracle's contract
// (encode's output is a plain numeric column with a value-label ATTACHMENT).
// (History: this used to dump the whole dataset to CSV with the extra
// alphabetically-ranked column and `cobra convert` it back; superseded.)
ojson runEncode(const std::string& strvar, const std::string& newvar,
                const std::optional<std::string>& lblname, SessionState& session,
                const std::optional<std::string>& ifExpr, bool noExtend) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: encode's entry validation reads a NAME, a
  // DTYPE and another NAME — no column data — so the resident shadow card
  // answers all three, and the datastore open (cecon::Dataset::open: mmap +
  // 192-column header parse) is skipped. Same mechanism as runGsort/runOrder
  // above; the dtype half is the S4a treatment (strmutate.cpp's factorColumns),
  // where the A12 argument for trusting the card's dtype is written out in
  // full: the card's vocabulary is exactly Datastore::dtype()'s ("factor" |
  // "f64"), it is the BASE type on both sides, and any other spelling is
  // refused rather than guessed. Refusal here means falling into the else
  // branch, which is the untouched prior behavior.
  //
  // SCOPE, STATED HONESTLY. This is NOT where encode's measured ~0.24 s/call
  // goes. That is dominated by mutationResult's card rebuild rescanning all 192
  // columns for n_missing, because "encode" is missing from
  // SessionState::seedNMissForPureAdd's kPureAdd set (session_state.cpp) even
  // though encode is a textbook pure add — a one-token fix in a file this stint
  // does not touch. Removing the wasted open here does not substitute for it.
  //
  // The three facts the checks below need, resolved from the card when it can
  // supply them and from the datastore otherwise. Resolution is separated from
  // checking on purpose: the three rejections must keep firing in their
  // ORIGINAL order (not-found, then not-a-string, then already-defined), since
  // an input that trips two of them must keep returning the same message it
  // always did. Collapsing the lookups into the checks is what would silently
  // reorder them.
  bool haveFacts = false;
  bool srcExists = false;
  std::string srcDtype;
  bool newExists = false;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    const VarInfo* src = cardFind(*card, strvar);
    // An unrecognized dtype spelling is refused rather than guessed (S4a's
    // rule) — haveFacts stays false and the datastore branch re-answers all
    // three, exactly as before this fix.
    if (src == nullptr || src->dtype == "factor" || src->dtype == "f64") {
      srcExists = (src != nullptr);
      if (src != nullptr) srcDtype = src->dtype;
      newExists = (cardFind(*card, newvar) != nullptr);
      haveFacts = true;
    }
  }
  if (!haveFacts) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    srcExists = ds->has(strvar);
    if (srcExists) srcDtype = ds->dtype(strvar);
    newExists = ds->has(newvar);
  }

  // Stata's rc for each refusal (w2ah_data_verbs.log ah_encode_numeric: 107 --
  // main answered rc 7; ah_encode_exist: 110).
  if (!srcExists) {
    return rcErrorBlock("variable " + strvar + " not found", 111);
  }
  if (srcDtype != "factor") {
    return rcErrorBlock(strvar + ": not possible with numeric variable", 107);
  }
  // BUG-1096: noextend reads the label() set as it stands, so it needs one
  // (StataBE 19.5 order: the source checks, then this, then r(110)).
  if (noExtend && (!lblname.has_value() || lblname->empty())) {
    return rcErrorBlock("noextend option not allowed without label() option", 198);
  }
  // BUG-1120: after the source checks, a reserved generate() name is refused by
  // the new-variable rule with _se and the r(table) row names: "_cons invalid
  // varname", "long invalid varname", "str5 invalid variable name", r(198); _skip
  // and _all are legal here (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar, /*resultRows=*/true)) {
    return rcErrorBlock(r->message, r->rc);
  }
  if (newExists) {
    return rcErrorBlock("variable " + newvar + " already defined", 110);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // BUG-1096: under noextend every string the sample holds must already be a
  // text of the label() set (a set that does not exist has none); otherwise
  // nothing is created and the strangers are named, in sorted order, r(459):
  // "Unexpected values encountered:" / "ccc" (StataBE 19.5; "" is missing, never
  // a stranger, and rows outside `if` are not looked at).
  if (noExtend) {
    std::set<std::string> texts;
    if (auto set = session.labelDefs.find(*lblname); set != session.labelDefs.end())
      for (const auto& [code, text] : set->second) texts.insert(text);
    std::optional<Datastore> sampleFile;
    Datastore* sample = nullptr;
    if (haveIf) {
      try {
        InRangeFilterResult fr =
            applyIfAndInFilter(session, *session.currentHbin(), ifExpr, std::nullopt);
        sampleFile.emplace(fr.dataPath);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      } catch (const std::exception& e) {
        return engineErrorBlock(std::string("encode: ") + e.what());
      }
      sample = &*sampleFile;
    } else {
      sample = session.datastore();
    }
    if (sample == nullptr || !sample->has(strvar)) return eval::noDataLoaded();
    const cecon::Column& c = sample->col(strvar);
    std::set<std::string> strangers;
    for (int64_t i = 0; i < sample->nRows(); ++i) {
      const int32_t code = c.codes()[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
      std::string text(c.levels[static_cast<size_t>(code)]);
      if (!text.empty() && texts.count(text) == 0) strangers.insert(std::move(text));
    }
    if (!strangers.empty()) {
      std::string msg = "Unexpected values encountered:";
      for (const auto& s : strangers) msg += "\n" + s;
      return rcErrorBlock(msg, 459);
    }
  }

  // L1/M3 (CSV-shim retirement): cobra's native `encode` produces NEWVAR as
  // f64 with codes in ALPHABETICAL level order (1-based; missing stays
  // missing), byte-preserving the source column and every other column (no
  // whole-frame CSV dump, no re-factorization). Its stdout JSON carries the
  // alphabetical "levels" array, which drives the value-label attachment
  // below — codes and labels thus come from the SAME engine sort and always
  // agree. `--gen` lands f64, not factor: exactly encode's contract.
  //
  // BUG-042 (`encode STRVAR if COND, gen(NEWVAR)`): Stata restricts which
  // observations get a real code — excluded rows come back MISSING on NEWVAR,
  // while the level MAPPING still spans the WHOLE variable (encode's
  // value-label set is dataset-wide, not tied to one invocation's sample). So
  // encode runs on the full data (all rows -> whole-variable levels) into a
  // temp column, then a cobra `cond()` mask nulls the out-of-sample rows —
  // the identical `if`-folding runClonevar's numeric fast path uses.
  std::string encGen = haveIf ? ("__encode_full_" + scratchToken()) : newvar;
  std::string encOut = nextPath(session.workdir(), "encode");
  // *session.currentHbin() rather than ds->path() — same substitution runGsort
  // and runRename make, and the reason `ds` no longer needs to exist here.
  std::vector<std::string> encArgs = {"encode", "--data", *session.currentHbin(), "--out",
                                      encOut,   "--var",  strvar,                 "--gen", encGen};
  // C4-L4: cobra's cmd_encode rides the same §26 acquire_data/commit_frame
  // bridge as setcol (apps/cmds/cmds_data.cpp:693/:698 — read cross-lane, no
  // cobra change), so the SAME resident opt-in routes it through the frame
  // plane: virtual head, vars_delta fold, no whole-Datastore reconstruct —
  // and on a dirty virtual head no EMH materialization at the stateless
  // --data site (the a2 encode window class). The `if` mask's eval below
  // rides the same scope. Escapes as at the destring site; the stateless
  // fallback (plane down ⇒ no virtual head) is byte-identical to before.
  const bool residentEncode = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::optional<SessionState::ResidentEligible> encodeResident;
  if (residentEncode) encodeResident.emplace(session);
  nlohmann::json encJson;
  try {
    encJson = runCobraJson(encArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // Alphabetical levels straight from the engine's own encode output, so the
  // value labels below describe exactly the codes it assigned. A level that is
  // not UTF-8 arrives escaped (BUG-1372; cmd_encode) and is turned back here.
  std::vector<std::string> sortedLevels;
  if (encJson.contains("levels") && encJson.at("levels").is_array()) {
    for (const auto& lv : encJson.at("levels")) {
      sortedLevels.push_back(lv.get<std::string>());
      cecon::hbin_level_unescape(sortedLevels.back());
    }
  }

  if (haveIf) {
    // Mask out-of-sample rows to missing (in-sample rows keep encGen, itself
    // already missing where the source was missing), then drop the temp — the
    // same one-shot `--gen COND-cond() --drop TMP` shape collapse's proxy
    // step used.
    std::string finalOut = nextPath(session.workdir(), "encode");
    // BUG-369 follow-up: the mask cond() embeds the user's if, which can carry
    // a TS operator (`encode s if L.x>0, gen(v)`).
    std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + encGen + ",.)";
    std::vector<std::string> maskArgs = translate::injectTsContextForExpr(
        {"eval", "--data", encOut, "--gen", maskExpr, "--drop", encGen, "--out", finalOut},
        maskExpr, session);
    CobraRunResult cr = runCobra(maskArgs);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(finalOut);
  } else {
    session.load(encOut);
  }

  // The value-label set encode fills and attaches. Its name is label() when
  // given, else the NEW variable's (w2ah_data_verbs.log ah_encode_gen: `: value
  // label e_es' is e_es; main named it after the source, es). When the set
  // already exists its codes are reused and each new string is appended at the
  // set's largest code + 1, in sorted order (ah_encode_label_exist: ordl = {1
  // pear, 5 kiwi} gives pear 1, Pear 6, apple 7, fig 8; ah_encode_label_again
  // reuses intervention's codes). The engine numbered the sorted levels 1..K; a
  // set that does not agree with that numbering is applied by one in-place
  // engine recode of the codes.
  std::string lbl = (lblname.has_value() && !lblname->empty()) ? *lblname : newvar;
  std::map<std::string, std::string> codeMap;  // string-keyed (labelDefs)
  auto existingSet = session.labelDefs.find(lbl);
  if (existingSet != session.labelDefs.end()) codeMap = existingSet->second;
  std::map<std::string, long long> codeOfText;
  long long maxCode = 0;
  for (const auto& [codeStr, text] : codeMap) {
    long long c = 0;
    try {
      c = std::stoll(codeStr);
    } catch (const std::exception&) {
      continue;
    }
    maxCode = std::max(maxCode, c);
    auto ins = codeOfText.emplace(text, c);
    if (!ins.second && c < ins.first->second) ins.first->second = c;
  }
  std::string remapRules;
  for (size_t i = 0; i < sortedLevels.size(); ++i) {
    const long long engineCode = static_cast<long long>(i) + 1;
    long long code = 0;
    auto known = codeOfText.find(sortedLevels[i]);
    if (known != codeOfText.end()) {
      code = known->second;
    } else if (noExtend) {
      // Only rows outside the sample hold this string (checked above), and they
      // are missing: the set is not extended (StataBE 19.5: `label list` after
      // `encode s if x < 4, ... noextend` still has only the set's own codes).
      continue;
    } else {
      code = ++maxCode;
      codeMap[std::to_string(code)] = sortedLevels[i];
    }
    if (code != engineCode)
      remapRules += "(" + std::to_string(engineCode) + "=" + std::to_string(code) + ")";
  }
  if (!remapRules.empty()) {
    std::string remapOut = nextPath(session.workdir(), "encode_codes");
    std::vector<std::string> args = {"recode", "--data", *session.currentHbin(), "--var", newvar,
                                     "--rules", remapRules, "--out", remapOut};
    std::optional<SessionState::ResidentEligible> remapResident;
    if (residentEncode) remapResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(remapOut);
  }
  if (!codeMap.empty() || existingSet != session.labelDefs.end()) session.labelDefs[lbl] = codeMap;
  session.varLblname[newvar] = lbl;
  // Every variable carrying this set sees the appended codes.
  for (const auto& [var, lname] : session.varLblname) {
    if (lname != lbl) continue;
    std::map<int, std::string> numericMap;  // int-keyed (valueLabels)
    for (const auto& [codeStr, text] : codeMap) {
      try {
        numericMap[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    if (numericMap.empty()) session.valueLabels.erase(var);
    else session.valueLabels[var] = numericMap;
  }
  // encode keeps the source's variable label (ah_encode_gen: [Fruit name]).
  {
    auto srcMeta = session.varMeta.find(strvar);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      session.varMeta[newvar].label = srcMeta->second.label;
  }
  // format-display-default-lifecycle: Stata's encode creates a long that
  // displays %8.0g, not long's own %12.0g (w2af_format_lifecycle.log
  // af_encode: `long|%8.0g`, and the same pair after a save/use,
  // af_rt_created). The format is the variable's own, so it is held.
  {
    VarMeta& vm = session.varMeta[newvar];
    vm.stataType = std::string("long");
    vm.format = std::string("%8.0g");
    session.implicitFloatVars.erase(newvar);
  }

  return mutationResult(session, "encode", {newvar});
}

namespace {
// BUG-664/665: the informational line Stata's destring and tostring print
// when a variable is skipped ("... no replace"). A display block, rc 0 -- the
// command did not fail, it declined one variable and moved on.
ojson destringNoteBlock(const std::string& text) {
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

// BUG-928 (FU-750): generate() places the new variable right after its own
// source, as Stata does (w2ab_strconv_order oracle) -- so `tostring a b, gen(a_s
// b_s)` on a b c leaves a a_s b b_s c (w2ab_strconv_order so_t3_gen_two).
// setcol --new appends the column last; one engine `order --after` (the
// append generate() pattern, BUG-551) moves it, skipped when the source was
// already the last column. `mutated` is the setcol's own mutationResult; its
// varlist_after says where the new column landed, and on a move it is
// replaced by the post-order card (same action and new_vars, final order).
// The order step rides the plane the setcol used: resident when the caller
// held ResidentEligible for setcol (the card fold takes order's S1b permutation
// plan), else the stateless one-shot over the real file setcol wrote. Returns
// an error block on failure, nullopt on success.
std::optional<ojson> placeGeneratedAfterSource(SessionState& session, const std::string& verb,
                                               const std::string& source,
                                               const std::string& target, bool resident,
                                               ojson& mutated) {
  if (mutated.is_object() && mutated.contains("varlist_after") &&
      mutated["varlist_after"].is_array()) {
    const ojson& vl = mutated["varlist_after"];
    for (size_t i = 0; i + 1 < vl.size(); ++i) {
      if (vl[i].value("name", std::string()) == source &&
          vl[i + 1].value("name", std::string()) == target)
        return std::nullopt;  // already right after its source
    }
  }
  std::string ordOut = nextPath(session.workdir(), verb + "_gen_order");
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars",
                                   target,  "--after", source, "--out", ordOut};
  {
    std::optional<SessionState::ResidentEligible> orderResident;
    if (resident) orderResident.emplace(session);
    CobraRunResult ocr = runCobra(args);
    if (!ocr.ok) {
      CobraError ce = errorFromRaw(ocr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  if (!resident && !cecon::native_exists(ordOut))
    return engineErrorBlock(verb + ": failed writing output dataset (disk full or I/O error)");
  session.load(ordOut);
  mutated = mutationResult(session, verb, {target});
  return std::nullopt;
}
}  // namespace

// ---------------------------------------------------------------------------
// destring VAR, {replace | gen(NEWVAR)} [ignore("chars")] [force] [float]
// [percent] [dpcomma] -- see handlers.h. Behaviour per [D] destring (and the
// StataBE 19.5 measurements).

namespace {

// Reads a number the way Stata's real() does: an optional sign, digits with an
// optional decimal point, and an optional exponent introduced by e/E/d/D, or the
// %21x hexadecimal form. Overflow, and any magnitude not below 2^1023 (beyond
// Stata's largest double), is unreadable; underflow reads as 0.
std::optional<double> destringReadNumber(const std::string& text) {
  if (text.empty()) return std::nullopt;
  size_t pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = text[pos] == '-';
    ++pos;
  }
  auto isDec = [](char c) { return c >= '0' && c <= '9'; };
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  constexpr double kLimit = 8.98846567431158e307;  // 2^1023
  auto finish = [&](double magnitude) -> std::optional<double> {
    if (!std::isfinite(magnitude) || magnitude >= kLimit) return std::nullopt;
    if (magnitude == 0.0) return 0.0;  // -0 reads as 0
    return negative ? -magnitude : magnitude;
  };

  // Hexadecimal form, recognised by its x/X marker: `1.` (or `0.` with all-zero
  // digits for zero, as %21x prints it), one to thirteen hexadecimal digits, the
  // marker, a required sign and one to three hexadecimal exponent digits; the
  // exponent runs from -3ff (which is zero) to +3fe. StataBE,
  // oracle/cr_destring_probe3 and cr_destring_probe4.
  const size_t xAt = text.find_first_of("xX", pos);
  if (xAt != std::string::npos) {
    if (xAt < pos + 3 || xAt > pos + 15 || text[pos + 1] != '.') return std::nullopt;
    const char lead = text[pos];
    if (lead != '0' && lead != '1') return std::nullopt;
    double fraction = 0.0, scale = 1.0;
    for (size_t i = pos + 2; i < xAt; ++i) {
      const int h = hexVal(text[i]);
      if (h < 0) return std::nullopt;
      scale /= 16.0;
      fraction += h * scale;
    }
    size_t e = xAt + 1;
    if (e >= text.size() || (text[e] != '+' && text[e] != '-')) return std::nullopt;
    const bool expNeg = text[e] == '-';
    ++e;
    if (e >= text.size() || text.size() - e > 3) return std::nullopt;
    int expo = 0;
    for (; e < text.size(); ++e) {
      const int h = hexVal(text[e]);
      if (h < 0) return std::nullopt;
      expo = expo * 16 + h;
    }
    if (expNeg) expo = -expo;
    if (expo > 0x3fe || expo < -0x3ff) return std::nullopt;
    if (lead == '0') {
      if (fraction != 0.0 || expo != -0x3ff) return std::nullopt;
      return 0.0;
    }
    if (expo == -0x3ff) return 0.0;
    return finish(std::ldexp(1.0 + fraction, expo));
  }

  // Decimal form; validated here, converted by strtod in the C locale.
  std::string cleaned;
  cleaned.reserve(text.size() - pos);
  bool mantDigit = false, sawPoint = false;
  size_t i = pos;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (isDec(c)) {
      mantDigit = true;
    } else if (c == '.' && !sawPoint) {
      sawPoint = true;
    } else {
      break;
    }
    cleaned.push_back(c);
  }
  if (!mantDigit) return std::nullopt;
  if (i < text.size()) {
    const char c = text[i];
    if (c != 'e' && c != 'E' && c != 'd' && c != 'D') return std::nullopt;
    cleaned.push_back('e');
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) cleaned.push_back(text[i++]);
    bool expDigit = false;
    for (; i < text.size(); ++i) {
      if (!isDec(text[i])) return std::nullopt;
      expDigit = true;
      cleaned.push_back(text[i]);
    }
    if (!expDigit) return std::nullopt;
  }
  char* end = nullptr;
  const double magnitude = std::strtod(cleaned.c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  return finish(magnitude);  // overflow gives inf (refused); underflow gives 0
}

// `.`, `.a`-`.z` (and the empty string) are missing values, never unreadable.
bool destringIsMissingToken(const std::string& t) {
  if (t.empty() || t == ".") return true;
  return t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z';
}

// dpcomma: a period anywhere makes the value unreadable; the first comma is
// the decimal point, unless the value is a lone comma or some comma is
// directly followed by a lowercase letter (then nothing is converted and the
// value stays unreadable).
std::optional<std::string> destringApplyDpcomma(const std::string& t) {
  if (t.find('.') != std::string::npos) return std::nullopt;
  if (t == ",") return t;
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i] == ',' && t[i + 1] >= 'a' && t[i + 1] <= 'z') return t;
  }
  std::string out = t;
  const size_t comma = out.find(',');
  if (comma != std::string::npos) out[comma] = '.';
  return out;
}

enum class DestringRead { Number, Missing, Unreadable };

// One observation's text -> number / missing / unreadable, after ignore(),
// trimming, the missing tokens and dpcomma ([D] destring, Options and
// Description). `sawPercent` is set when a `%` was removed.
DestringRead destringReadCell(const std::string& raw, const std::array<bool, 256>& ignoreSet,
                              bool anyIgnore, bool dpcomma, double& value, bool& sawPercent) {
  std::string t;
  if (anyIgnore) {
    t.reserve(raw.size());
    for (char c : raw) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (ignoreSet[u]) {
        if (c == '%') sawPercent = true;
        continue;
      }
      t.push_back(c);
    }
  } else {
    t = raw;
  }
  size_t a = 0, b = t.size();
  while (a < b && (t[a] == ' ' || t[a] == '\t')) ++a;
  while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t')) --b;
  t = t.substr(a, b - a);
  if (destringIsMissingToken(t)) return DestringRead::Missing;
  if (dpcomma) {
    std::optional<std::string> conv = destringApplyDpcomma(t);
    if (!conv.has_value()) return DestringRead::Unreadable;
    t = std::move(*conv);
  }
  std::optional<double> v = destringReadNumber(t);
  if (!v.has_value()) return DestringRead::Unreadable;
  value = *v;
  return DestringRead::Number;
}

// float(): the nearest single; nullopt from 2^127 up, where Stata's float range
// ends (StataBE: under `float`, 1.7e38 reads, while 1.8e38 and 2e38% are
// unreadable, or missing under `force`).
std::optional<double> toSingle(double v) {
  if (std::fabs(v) >= 0x1p127) return std::nullopt;
  const double f = static_cast<double>(static_cast<float>(v));
  if (std::fabs(f) >= 0x1p127) return std::nullopt;
  return f;
}

// [D] destring creates double (float under `float`) and then compresses;
// [D] compress demotes to the smallest integer type that holds every value
// (ranges per [D] Data types), never double->float and never float->long.
std::string destringStorageType(const std::vector<double>& vals, bool asFloat) {
  const std::string created = asFloat ? "float" : "double";
  if (vals.empty()) return created;  // an empty variable is not compressed
  bool any = false, integral = true;
  double lo = 0, hi = 0;
  for (double v : vals) {
    if (std::isnan(v)) continue;
    if (v != std::trunc(v)) integral = false;
    if (!any) {
      lo = hi = v;
      any = true;
    } else {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  if (!any) return "byte";
  if (!integral) return created;
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (asFloat) return "float";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return "double";
}

}  // namespace

ojson runDestring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ignoreChars, bool force,
                  SessionState& session, const DestringOpts& opts) {
  if (!session.hasData()) return eval::noDataLoaded();

  // Names and types come from the resident card when one is current (it is
  // authoritative for absence as well); the datastore is opened only when
  // there is no card, or when the row strings are not cached.
  Datastore* ds = nullptr;
  auto openDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto dtypeOf = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card)
        if (vi.name == name) return vi.dtype;
      return std::nullopt;
    }
    Datastore* d = openDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };

  const std::optional<std::string> srcType = dtypeOf(varname);
  if (!srcType.has_value())
    return engineErrorBlock("destring: variable '" + varname + "' not found");
  if (doReplace == genVar.has_value())
    return engineErrorBlock("destring: specify exactly one of `replace` or `generate(newvar)`",
                            "syntax_error");

  const std::string target = doReplace ? varname : trim(*genVar);
  if (!doReplace) {
    const bool reserved = strutil::isReservedVarName(target);
    if (reserved || target == "_skip" || target == "_all")
      return rcErrorBlock("generate() contains existing variable(s) and/or illegal variable name(s)",
                          reserved ? 198 : 101);
    if (dtypeOf(target).has_value())
      return engineErrorBlock("destring: variable '" + target + "' already defined");
    if (!strutil::isValidStataIdentifier(target))
      return engineErrorBlock("destring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
  }
  const std::string op = doReplace ? "replace" : "generate";
  if (*srcType == "f64") return destringNoteBlock(varname + " already numeric; no " + op);

  // The effective ignore set: ignore()'s bytes, plus `%` under percent.
  std::array<bool, 256> ignoreSet{};
  bool anyIgnore = false;
  if (ignoreChars.has_value()) {
    for (char c : *ignoreChars) {
      ignoreSet[static_cast<unsigned char>(c)] = true;
      anyIgnore = true;
    }
  }
  if (opts.percent) {
    ignoreSet[static_cast<unsigned char>('%')] = true;
    anyIgnore = true;
  }

  // The source's row strings: the cached committed column when it is current,
  // else the datastore's codes and level table.
  int64_t n = 0;
  const std::vector<std::string>* cachedStrs = nullptr;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* levels = nullptr;
  if (const auto* cc = session.residentComputedCol(varname); cc != nullptr && cc->dtype == "factor") {
    n = cc->nRows;
    cachedStrs = &cc->strs;
  } else {
    Datastore* d = openDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    const cecon::Column& col = d->col(varname);
    codes = col.codes();
    levels = &col.levels;
  }
  static const std::string kEmpty;
  auto rowText = [&](int64_t r) -> const std::string& {
    if (cachedStrs != nullptr) return (*cachedStrs)[static_cast<size_t>(r)];
    const int32_t code = codes[r];
    if (code < 0 || static_cast<size_t>(code) >= levels->size()) return kEmpty;
    return (*levels)[static_cast<size_t>(code)];
  };

  const double kMissing = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> values(static_cast<size_t>(n), kMissing);
  bool sawPercent = false;
  for (int64_t r = 0; r < n; ++r) {
    double v = 0;
    DestringRead got =
        destringReadCell(rowText(r), ignoreSet, anyIgnore, opts.dpcomma, v, sawPercent);
    if (got == DestringRead::Number && opts.asFloat) {
      const std::optional<double> single = toSingle(v);
      if (single.has_value()) v = *single;
      else got = DestringRead::Unreadable;
    }
    if (got == DestringRead::Number) {
      values[static_cast<size_t>(r)] = v;
    } else if (got == DestringRead::Unreadable && !force) {
      return destringNoteBlock(varname +
                               (anyIgnore ? ": contains characters not specified in ignore(); no "
                                          : ": contains nonnumeric characters; no ") +
                               op);
    }
  }
  if (opts.percent && sawPercent) {
    for (double& v : values) {
      if (std::isnan(v)) continue;
      v = v / 100.0;
      if (opts.asFloat) v = toSingle(v).value_or(kMissing);
    }
  }
  const std::string storage = destringStorageType(values, opts.asFloat);

  // The engine's single-column write: one line per row, shortest round-trip
  // text, an empty line for missing.
  std::string payloadPath = nextPath(session.workdir(), "destring") + ".csv";
  {
    std::ofstream payload(cecon::to_native_path(payloadPath), std::ios::binary);
    if (!payload) return engineErrorBlock("destring: could not open scratch CSV for write");
    for (double v : values) {
      if (!std::isnan(v)) payload << eval::pythonFloatRepr(v);
      payload << '\n';
    }
    payload.flush();
    if (!payload) {
      cecon::native_unlink(payloadPath);
      return engineErrorBlock("destring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(cecon::to_native_path(payloadPath), ec);
    counters::addIpcBytes(ec ? 0 : static_cast<int64_t>(bytes), /*setcolShaped=*/true);
  }
  const bool resident =
      std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr && !residentReadDisabled();
  std::string dataPath;
  if (resident) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = openDs();
    if (d == nullptr) {
      cecon::native_unlink(payloadPath);
      return eval::noDataLoaded();
    }
    dataPath = d->path();
  }
  const std::string outPath = nextPath(session.workdir(), "destring");
  std::vector<std::string> args = {"setcol",   "--data",    dataPath, "--var",  target,
                                   "--values", payloadPath, "--dtype", "f64",   "--out",
                                   outPath};
  if (!doReplace) args.push_back("--new");
  CobraRunResult cr;
  {
    std::optional<SessionState::ResidentEligible> eligible;
    if (resident) eligible.emplace(session);
    cr = runCobra(args);
  }
  cecon::native_unlink(payloadPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!resident && !cecon::native_exists(outPath))
    return engineErrorBlock("destring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);

  {
    // The recorded type is the compressed one; the display format stays the
    // created type's default (double %10.0g, float %9.0g).
    std::optional<std::string> sourceLabel;
    if (!doReplace) {
      auto it = session.varMeta.find(varname);
      if (it != session.varMeta.end() && it->second.label.has_value() &&
          !it->second.label->empty())
        sourceLabel = it->second.label;
    }
    VarMeta& vm = session.varMeta[target];
    vm.stataType = storage;
    vm.format = stataDefaultFormat(std::string(opts.asFloat ? "float" : "double"));
    if (!doReplace) {
      if (sourceLabel.has_value()) vm.label = sourceLabel;
      else vm.label.reset();
    }
  }
  session.implicitFloatVars.erase(target);

  ojson result = doReplace ? mutationResult(session, "destring", /*newVars=*/{},
                                            /*droppedVars=*/{}, /*rewrittenVars=*/{target})
                           : mutationResult(session, "destring", {target});
  if (!doReplace) {
    if (auto err = placeGeneratedAfterSource(session, "destring", varname, target, resident, result))
      return *err;
  }
  if (resident && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol memo;
    memo.dtype = "f64";
    memo.nRows = n;
    memo.f64 = std::move(values);
    session.residentComputedColPut(target, std::move(memo));
  }
  return result;
}

// ---------------------------------------------------------------------------
// tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat] — BS-003/D4 (BETA
// B-data). PIET-NATIVE numeric->string(factor) conversion, the mirror of
// runDestring above.

namespace {
// Default (non-usedisplayformat) numeric->string rendering — mirrors
// strmutate.cpp's file-local coerceStr() convention (integer -> plain
// digits via eval::pythonIntStrFromDouble, non-integer -> eval::gFormat);
// kept as its own copy per this codebase's established per-TU convention
// (see errorFromRaw's own header note above) rather than exporting
// strmutate.cpp's file-local helper. Missing is handled by the caller
// (tostringMissingRender) before this is reached.
std::string tostringDefaultRender(double v) {
  if (eval::pyIsInteger(v) && std::fabs(v) < 1e15) {
    return eval::pythonIntStrFromDouble(v);
  }
  return eval::gFormat(v);
}

// BUG-207: what `tostring` writes for a MISSING numeric.
//
// This used to be `""` on both branches, with an in-tree comment asserting that
// was Stata-faithful. It is not, and the assertion is exactly how the defect
// survived: `fmt_g_oracle.log`'s section C only ever spoke to the
// `usedisplayformat` branch, and one branch's evidence was generalised to the
// other. Measured on BOTH branches, StataNow 19.5, marker-judged
// (`internal capture tostring_missing_oracle.log`, whole-line marker
// `TOSTRING-MISSING-CAPTURE-COMPLETE`), with `strlen` reported per cell so `""`
// and `"."` cannot be confused by eye:
//
//     branch                      42        .           .a
//     plain tostring              [42] 2    [.] 1       [.a] 2
//     , usedisplayformat          [42] 2    [.] 1       [.a] 2
//     , usedisplayformat %12.4g   [42] 2    [.] 1       [.a] 2
//
// So Stata writes the missing TOKEN, and it writes the extended CODE — a
// `tostring`-ed column keeps the 27-way distinction the numeric column had. The
// field width is NOT kept (`%12.4g` still yields `42`, not a padded field), which
// is why the width row is in the capture at all.
//
// The kind is available here: PIET carries the extended missings as NaN payload
// bits (`data/missing.h`, already included by this TU), and this engine really
// does preserve them through `replace v = .a` — live-probed this stint,
// `count if v == .a` -> 1 and `count if v == .` -> 1 on the same three-row
// column. So rendering only system missing as `.` and collapsing `.a`-`.z` onto
// it would be trading one wrong byte for a different wrong byte at a site being
// edited anyway, with the oracle for the right answer sitting in the same log.
// SCOPE NOTE: BUG-207 named the extended half as a separate claim. It is
// implemented here because it is three lines off a store this file already reads
// and an oracle row already captured — not because the entry authorised it.
//
// `missing_kind` returns -1 for anything finite or infinite, so a caller that
// reaches this for a non-missing gets the honest empty string rather than a
// bogus `.`; the one call site guards with isnan() first regardless.
std::string tostringMissingRender(double v) {
  const int kind = cecon::missing_kind(v);
  if (kind < cecon::kMissSys) return std::string();  // not missing at all
  if (kind == cecon::kMissSys) return ".";           // system missing -> `.`
  return std::string(".") + static_cast<char>('a' + (kind - cecon::kMissA));
}
}  // namespace

// BUG-371: `decode NUMVAR, generate(NEWVAR)` — the inverse of `encode`, and the
// asymmetry was the finding: a user who encoded a string could not get it back.
//
// What `encode` actually leaves behind (runEncode above) is an f64 column of
// 1-based codes PLUS a session-side value-label map — not a factor column. So
// decode is not a dtype relabel: it reads session.valueLabels[var], maps each
// row's code to its label, and writes a NEW factor column. A code with no label
// becomes missing, which is what Stata does.
ojson runDecode(const std::string& varname, const std::optional<std::string>& genVar,
                SessionState& session, size_t maxLength) {
  if (!session.hasData()) return eval::noDataLoaded();
  if (!genVar.has_value() || genVar->empty())
    return engineErrorBlock("decode: generate(newvar) is required", "syntax_error");
  const std::string target = trim(*genVar);
  if (!strutil::isValidStataIdentifier(target))
    return engineErrorBlock(
        "decode: invalid variable name `" + target + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(varname))
    return engineErrorBlock("variable " + varname + " not found", "unknown_variable");
  if (ds->has(target))
    return engineErrorBlock("decode: variable " + target + " already defined", "bad_input");
  // Stata's rc for both refusals (w2ah_data_verbs.log ah_decode_string: 108,
  // ah_decode_nolab: 182; main answered rc 1 to each).
  if (ds->dtype(varname) != "f64")
    return rcErrorBlock(varname + ": not possible with string variable", 108);

  // "Not labeled" is about the attachment: a variable carrying a value-label
  // name decodes (an unmapped code to ""), one carrying none is r(182).
  if (session.varLblname.find(varname) == session.varLblname.end())
    return rcErrorBlock(varname + " not labeled", 182);
  // An attached set that is not defined (dropped, or never defined) is
  // r(111) "value label X not found" (w2ao_label_family lb_val_undef, lb_drop
  // `dec=111`).
  {
    const std::string& lbl = session.varLblname.at(varname);
    if (session.labelDefs.find(lbl) == session.labelDefs.end())
      return rcErrorBlock("value label " + lbl + " not found", 111);
  }
  // BUG-1120: only then the generate() name, by encode's rule: "_cons invalid
  // varname", "int invalid varname", r(198) (`decode s, gen(_cons)' with s string
  // is still r(108), `decode id, gen(_rc)' with id unlabeled r(182); StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(target, /*resultRows=*/true))
    return rcErrorBlock(r->message, r->rc);
  static const std::map<int, std::string> kNoLabels;
  auto lblIt = session.valueLabels.find(varname);
  const std::map<int, std::string>& labels =
      lblIt == session.valueLabels.end() ? kNoLabels : lblIt->second;

  const int64_t n = ds->nRows();
  const double* src = ds->col(varname).f64();
  std::vector<std::string> out(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    const double v = src[r];
    if (std::isnan(v) || v != std::floor(v)) continue;  // stays "" == factor-missing
    auto it = labels.find(static_cast<int>(v));
    if (it == labels.end()) continue;  // an unlabeled code decodes to missing, matching Stata
    // BUG-1096: maxlength(#) counts bytes but never cuts inside a character; it
    // keeps the whole characters that fit (StataBE 19.5, labels "héllo" and
    // "日本語です": maxlength(4) gives "hél" and "日", maxlength(2) "h" and "").
    // A cut mid-sequence wrote invalid UTF-8 the snapshot writer then refused.
    std::string text = it->second;
    if (text.size() > maxLength) {
      size_t cut = maxLength;
      while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
      text.resize(cut);
    }
    out[static_cast<size_t>(r)] = std::move(text);
  }

  // Same one-column setcol shape runTostring uses — see its comments for the
  // resident-plane reasoning and the BUG-124 flush guard.
  std::string valuesPath = nextPath(session.workdir(), "decode") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("decode: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) csv << csvEscape(out[static_cast<size_t>(r)]) << "\n";
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("decode: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::string outPath = nextPath(session.workdir(), "decode");
  std::string dataPath = residentSetcol ? *session.currentHbin() : ds->path();
  std::vector<std::string> setArgs = {"setcol",   "--data",   dataPath, "--var",   target,
                                      "--values", valuesPath, "--dtype", "factor", "--new",
                                      "--out",    outPath};
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("decode: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // decode's new variable is a str# as wide as the longest label of the attached
  // set in bytes, used or not, and at most maxlength() (at least 1), with that
  // type's default format, and it keeps the source's variable label
  // (w2ah_data_verbs.log ah_decode_gen: str17 %17s [Region code]; ah_decode_short:
  // str2 %9s). Main recorded no type, so `: type' failed and the format read %9s.
  // StataBE 19.5, set {"ab", "c", unused "a very long unused label"}: str24, and
  // str10 under maxlength(10); "héllo"/"日本語です" under maxlength(2): str2 though
  // the longest kept text is "h". Past 2045 bytes it is strL, as every str# is: a
  // 2048-byte label (used or not) makes strL, and so does maxlength(2046) over a
  // longer one, while maxlength(2045) makes str2045.
  size_t width = 1;
  for (const auto& [code, text] : labels) width = std::max(width, text.size());
  width = std::min(width, std::max<size_t>(maxLength, 1));
  {
    VarMeta& vm = session.varMeta[target];
    vm.stataType = width > 2045 ? std::string("strL") : "str" + std::to_string(width);
    auto srcMeta = session.varMeta.find(varname);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      vm.label = srcMeta->second.label;
  }
  session.implicitFloatVars.erase(target);
  return mutationResult(session, "decode", {target});
}

ojson runTostring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar, bool useDisplayFormat,
                  SessionState& session, const TostringOpts& opts, bool* forcedLoss) {
  if (!session.hasData()) return eval::noDataLoaded();
  // C4-L4: same lazy-datastore + card-first validation as runDestring above
  // (see the destring site for the full rationale — this is its mirror).
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card) {
        if (vi.name == name) return vi.dtype;
      }
      return std::nullopt;  // a current card is authoritative for absence too
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  std::optional<std::string> srcDtype = lookupDtype(varname);
  if (!srcDtype.has_value()) {
    return engineErrorBlock("tostring: variable '" + varname + "' not found");
  }
  if (doReplace == genVar.has_value()) {
    return engineErrorBlock("tostring: specify exactly one of `replace` or `gen(newvar)`");
  }
  std::string target = doReplace ? varname : *genVar;
  if (!doReplace) {
    // BUG-1120: as destring's generate(): a reserved name or _skip/_all is the
    // ado's "generate(newvarlist) invalid", r(198) and r(101), present or not
    // (StataBE 19.5).
    if (strutil::isReservedVarName(target) || target == "_skip" || target == "_all") {
      return rcErrorBlock("generate(newvarlist) invalid",
                          strutil::isReservedVarName(target) ? 198 : 101);
    }
    if (lookupDtype(target).has_value()) {
      return engineErrorBlock("tostring: variable '" + target + "' already defined");
    }
    if (!strutil::isValidStataIdentifier(target)) {
      return engineErrorBlock("tostring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }
  const std::string genrep = doReplace ? "replace" : "generate";
  if (*srcDtype != "f64") {
    // BUG-665: tostring.ado answers "VAR already string; no replace|generate"
    // and moves on (rc 0); the error stays on the legacy (non-gated) caller.
    if (opts.stataGate) return destringNoteBlock(varname + " already string; no " + genrep);
    return engineErrorBlock("tostring: '" + varname + "' is already a string variable");
  }
  if (opts.stataGate && doReplace && !opts.force) {
    auto lblIt = session.varLblname.find(varname);
    if (lblIt != session.varLblname.end() && !lblIt->second.empty())
      return destringNoteBlock(varname + " has value label; no replace");
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();

  // Source values: the computed-column cache first (the destring→tostring
  // chain shape), else the datastore (honest flush on a dirty head).
  const SessionState::ResidentComputedCol* srcCc = session.residentComputedCol(varname);
  if (srcCc != nullptr && srcCc->dtype != "f64") srcCc = nullptr;
  int64_t n = 0;
  const double* src = nullptr;
  if (srcCc != nullptr) {
    n = srcCc->nRows;
    src = srcCc->f64.data();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    src = d->col(varname).f64();
  }

  // usedisplayformat (D6): render via the variable's OWN session display
  // format (BS-005's session.varMeta[...].format — "%9.0g" default when
  // unset, matching Datastore/VarInfo's own declared default). Reuses
  // piet::eval::applyStataFmt — note that helper only recognizes the
  // numeric %{f,g,e} families (falls back to its own generic-number
  // rendering for anything else, e.g. a date/time %fmt); a pre-existing
  // limitation of that shared helper, not newly introduced here.
  //
  // format-display-default-lifecycle: the format is the one resolver's
  // (SessionState::displayFormat): the session's own, else the one the card's
  // file states; with neither, the storage-type default below (the resolver's
  // third step, the same stataDefaultFormat table), which also covers a column
  // with no known type by rendering both readings.
  std::string fmt = "%9.0g";
  bool fmtRecorded = false;
  if (useDisplayFormat) {
    std::optional<VarInfo> vi;
    if (card != nullptr) {
      for (const auto& c : *card)
        if (c.name == varname) {
          vi = c;
          break;
        }
    } else if (Datastore* d = ensureDs()) {
      std::vector<VarInfo> fc = d->formatCard(std::vector<std::string>{varname});
      if (!fc.empty()) vi = fc.front();
    }
    auto it = session.varMeta.find(varname);
    const bool held = it != session.varMeta.end() && it->second.format.has_value() &&
                      !it->second.format->empty();
    if (held || (vi.has_value() && vi->formatStated)) {
      fmt = vi.has_value() ? session.displayFormat(*vi) : *it->second.format;
      fmtRecorded = true;
    }
  }
  // BUG-665: tostring.ado's format choice -- format() when given, the display
  // format under usedisplayformat (above), else `%12.0g` (the ado's
  // `local fmt "%12.0g"`). The legacy caller keeps its renderer.
  const bool adoFormat = opts.stataGate && !useDisplayFormat;
  if (adoFormat) fmt = opts.format.value_or("%12.0g");
  // The ado renders the STORED value: `string(v, fmt)` on a float variable
  // sees float(v). PIET holds numerics as f64, so the storage type decides
  // what is rendered. A declared VarMeta::stataType is trusted ("float" is
  // rounded, byte/int/long/double are not), and so is an undeclared
  // gen/egen's implicit float (implicitFloatVars, Stata's `generate`
  // default). merge/append record a .dta using file's types (BUG-666; append
  // rounds a float master's rows to float before widening it to double, as
  // Stata does, and drops the claim of an unclaimed master whose rows are not
  // float-exact). A
  // column with none of these (collapse outputs under a new name, columns from
  // a .csv/.hbin using file, import delimited, an undeclared gen after rename
  // or clonevar) has no known storage type: StataBE gives such shapes float,
  // double or long depending on the source.
  // For those both readings are rendered, and the command goes ahead only
  // when they agree (every value float-exact, or both skip with some row
  // irreversible under both, so that no mixed float/double truth converts
  // either). When they
  // disagree it is refused by name below rather than guessed, since either
  // guess can convert where Stata skips or skip where Stata converts.
  // A declared byte/int/long that now holds a fraction was promoted by a
  // later `replace` that left the claim unrecorded: StataBE makes byte/int
  // float and long double ("variable b was byte now float", "variable l was
  // long now double"), so that is what is read here. (BUG-471 T2's replace
  // records the widened type itself, storage::recordReplaceType.)
  enum class Storage { Float, NotFloat, Unknown };
  Storage storage = Storage::NotFloat;
  // The storage type the variable was created with (its declaration, or
  // gen's implicit float), which is what its default display format follows.
  std::optional<std::string> createdType;
  if (opts.stataGate) {
    auto it = session.varMeta.find(varname);
    if (it != session.varMeta.end() && it->second.stataType.has_value()) {
      const std::string& t = *it->second.stataType;
      createdType = t;
      storage = t == "float" ? Storage::Float : Storage::NotFloat;
      if (t == "byte" || t == "int") {
        for (int64_t r = 0; r < n; ++r) {
          if (!std::isnan(src[r]) && src[r] != std::trunc(src[r])) {
            storage = Storage::Float;
            break;
          }
        }
      }
    } else if (session.implicitFloatVars.count(varname) != 0) {
      storage = Storage::Float;
      createdType = std::string("float");
    } else {
      storage = Storage::Unknown;
    }
  }

  // usedisplayformat with no recorded display format: the default `generate`
  // gives the type the variable was CREATED with (stataDefaultFormat), so `set
  // type double` / `gen id = 12345678` / `tostring id, replace
  // usedisplayformat` renders "12345678" (%10.0g) and converts, where %9.0g
  // would render "1.23e+07" and skip. Not the current type: StataBE keeps the
  // creation format when a `replace` promotes the variable
  // (exprfns_contract_oracle.log:646/736 byte -> float keeps %8.0g, :655/737
  // int -> long keeps %8.0g), so a byte holding a fraction renders %8.0g and a
  // long holding one %12.0g. append and a widening replace
  // (storage::recordReplaceType) record the creation format themselves when
  // they change the claim. recast writes no format and cannot be
  // told apart here (an OPEN divergence: `recast double` of a float renders
  // %10.0g here, %9.0g in Stata, :735). A column with no known storage type
  // is rendered with each reading's own default (below). The legacy caller
  // keeps %9.0g.
  const bool typeDefaultFmt = opts.stataGate && useDisplayFormat && !fmtRecorded;
  if (typeDefaultFmt && storage != Storage::Unknown) fmt = stataDefaultFormat(createdType);

  struct RenderPass {
    std::vector<std::string> strings;
    bool irreversible = false;
    std::vector<char> rowBad;  // per row, filled only when asked for (trackRows)
    size_t maxLen = 1;
  };
  auto renderAll = [&](bool asFloat, const std::string& rfmt, bool trackRows) {
    RenderPass p;
    p.strings.resize(static_cast<size_t>(n));
    if (trackRows) p.rowBad.assign(static_cast<size_t>(n), 0);
    for (int64_t r = 0; r < n; ++r) {
      double v = src[r];
      if (std::isnan(v)) {
        // BUG-207: `.` / `.a`-`.z`, NOT "" — see tostringMissingRender's oracle.
        // Deliberately BEFORE the branch fork, so plain and `usedisplayformat`
        // cannot diverge here: the capture says both write the same token, and
        // one shared site is what makes that structural rather than a
        // coincidence.
        p.strings[static_cast<size_t>(r)] = tostringMissingRender(v);
        // BUG-918: the ado stores `string(v, fmt)`, so a left-justified
        // format() pads the missing token too (w2z_tostring tz_fmt_left:
        // `[.        ]`, str9). BUG-1159 (FU-753, FU-761): so does a
        // left-justified display format under usedisplayformat, which the ado
        // renders the same way (StataBE 19.5, measured 2026-11-03: `format z
        // %-9.0g` / `tostring z, replace usedisplayformat` stores ".        ",
        // str9; %-9.2f gives "3.00     " and ".        ").
        if (opts.stataGate) {
          p.strings[static_cast<size_t>(r)] =
              piet::fmt::padMissingLeftJustify(p.strings[static_cast<size_t>(r)], rfmt);
        }
        p.maxLen = std::max(p.maxLen, p.strings[static_cast<size_t>(r)].size());
        continue;
      }
      if (asFloat) v = static_cast<double>(static_cast<float>(v));
      std::string rendered;
      if (opts.stataGate) {
        // The ado stores `string(v, fmt)` as is: a left-justified format keeps
        // its trailing pad (WSR|292), which applyStataFmt already produces.
        rendered = eval::applyStataFmt(v, rfmt);
        // The ado's reversibility test: `count if v != real(temp)` (real()
        // ignores the pad).
        if (!p.irreversible || trackRows) {
          const std::string bare = trim(rendered);
          std::optional<double> back;
          if (bare.find('_') == std::string::npos) back = eval::pythonFloat(bare);
          if (!back.has_value() || *back != v) {
            p.irreversible = true;
            if (trackRows) p.rowBad[static_cast<size_t>(r)] = 1;
          }
        }
      } else {
        rendered = useDisplayFormat ? eval::applyStataFmt(v, rfmt) : tostringDefaultRender(v);
      }
      p.maxLen = std::max(p.maxLen, rendered.size());
      p.strings[static_cast<size_t>(r)] = std::move(rendered);
    }
    return p;
  };
  const bool unknown = storage == Storage::Unknown;
  RenderPass pass = renderAll(storage == Storage::Float,
                              unknown && typeDefaultFmt ? stataDefaultFormat("double") : fmt,
                              unknown && !opts.force);
  if (unknown) {
    RenderPass asFloat =
        renderAll(true, typeDefaultFmt ? stataDefaultFormat("float") : fmt, !opts.force);
    const bool convertsAsDouble = opts.force || !pass.irreversible;
    const bool convertsAsFloat = opts.force || !asFloat.irreversible;
    const bool agree = convertsAsDouble == convertsAsFloat &&
                       (!convertsAsDouble || pass.strings == asFloat.strings);
    // Both whole-column readings skip. That settles it only when some row is
    // irreversible under both, since then every truth skips. A column whose
    // rows are each reversible under one reading or the other may be held
    // mixed in Stata, the way an append that widens a float master to double
    // holds it (w2f_residuals.log:1856: master `gen x = 0.07*100` is 7, the
    // using double 0.1 stays 0.1, and tostring converts). A float master is
    // rounded at append time and claimed double, so it no longer reaches this
    // branch; only a column with no recorded type does (import delimited,
    // collapse outputs, clonevar, a renamed gen, or an unclaimed master
    // append widened). Its Stata truth is not known, and a skip here could be
    // a silent wrong answer, so it is refused by name instead (STEP 5). The
    // price: such a column whose Stata storage is uniform, and whose rows each
    // convert under exactly one reading, is refused where StataBE skips with
    // the note at rc 0.
    if (agree && !convertsAsDouble) {
      bool someRowSkipsBoth = false;
      for (int64_t r = 0; r < n && !someRowSkipsBoth; ++r) {
        const size_t i = static_cast<size_t>(r);
        if (pass.rowBad[i] && asFloat.rowBad[i]) someRowSkipsBoth = true;
      }
      if (!someRowSkipsBoth) {
        return engineErrorBlock(
            "tostring: " + varname +
                " has no recorded storage type; as float and as double it cannot be converted "
                "reversibly, but each of its values can under one of the two, so a column that "
                "holds some rows as float and others as double converts; declare it first "
                "(`recast double " + varname + "' or `recast float " + varname + "')",
            "syntax_error");
      }
    }
    if (!agree) {
      return engineErrorBlock(
          "tostring: " + varname +
              " has no recorded storage type, and as float and as double it converts "
              "differently; declare it first (`recast double " + varname + "' or `recast float " +
              varname + "')",
          "syntax_error");
    }
  }
  std::vector<std::string>& outStrings = pass.strings;
  const size_t maxLen = pass.maxLen;
  if (pass.irreversible && !opts.force) {
    // w2b_dataio.log tostring_nofmt_ctl / tostring_gen_fmt / _gen_nofmt_ctl:
    // "big cannot be converted reversibly; no replace", rc 0, big unchanged.
    return destringNoteBlock(varname + " cannot be converted reversibly; no " + genrep);
  }
  if (forcedLoss != nullptr) *forcedLoss = pass.irreversible;

  // Hand the engine's single-column `cobra setcol` verb just the rendered
  // string column instead of a whole-frame CSV + `cobra convert` round-trip
  // (which re-factorizes every untouched factor column). The rendering above
  // is unchanged; every other column/dictionary byte is preserved.
  //
  // BUG-207 changed what a missing row puts on this wire, so the transport note
  // here changed with it. It used to read: a numeric missing renders "" and
  // setcol maps an empty payload line to missing (code -1), so missing stays
  // missing. It now renders `.` / `.a`-`.z`, which setcol carries as an ORDINARY
  // FACTOR LEVEL — `Frame::read_csv_column` treats only an empty cell as missing
  // and says so in its own comment ("empty -> missing; '.' etc. stay literal
  // levels", src/data/frame.cpp), so no new transport support was needed.
  //
  // The resulting card change is the CORRECT one, not a side effect to absorb:
  // the product of `tostring` is a STRING variable, and a string variable's
  // missing is `""`, not `.`. So a Stata-faithful `tostring` of a column with
  // missings yields a string column with ZERO missing cells and a `.` level —
  // which is what n_missing/n_levels now report, and what Stata's own
  // `describe` reports for the same operation.
  bool targetPresent =
      lookupDtype(target).has_value();  // true only for `replace` (target==varname)
  std::string valuesPath = nextPath(session.workdir(), "tostring") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("tostring: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) {
      csv << csvEscape(outStrings[static_cast<size_t>(r)]) << "\n";
    }
    // BUG-124 sibling: flush + check — silent short payload = silent row loss.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("tostring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h). setcol's own
  // semantic payload is metadata, so it leaves the ratio metric and is asserted
  // as an ABSOLUTE byte budget instead (A1 1.3). What that budget catches is
  // exactly this: the scratch CSV is a full rows-scale re-serialisation handed
  // to a subprocess for what is nominally a one-column edit. Only the bytes
  // THIS process wrote are billed here -- the `--out` hbin is read back through
  // Datastore, which bills it once at the COLUMNAR site (no double count).
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }
  std::string outPath = nextPath(session.workdir(), "tostring");
  // C2-SETCOL-RESIDENT (mirror of runDestring): route this setcol through the
  // resident frame plane so session.load(outPath) takes a VIRTUAL head instead
  // of reconstructing the whole Datastore. For an in-place f64->factor change the
  // engine reports card_stable=true with a vars_delta carrying the new
  // type/n_missing AND n_levels (de-risk brief §8.2); the vars_delta fold applies
  // it, SETTING nLevels (the opposite direction of destring — the two together
  // are the A/B parity gate's symmetry check). Stateless fallback + escapes
  // behave exactly as documented at the destring site (residentSetcol resolved
  // at entry, C2-specific + C4-family escapes both honored).
  std::string dataPath;
  if (residentSetcol) {
    dataPath = *session.currentHbin();  // frame-run channel strips --data
  } else {
    Datastore* d = ensureDs();  // pre-C4 verbatim: materialize + real file
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }
  std::vector<std::string> setArgs = {"setcol",   "--data",    dataPath, "--var",  target,
                                      "--values", valuesPath,  "--dtype", "factor", "--out",
                                      outPath};
  if (!targetPresent) setArgs.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // Virtual (resident) head has no on-disk file; guard the torn-write stat to the
  // forced-stateless path only (see the destring site's note).
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("tostring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // C1-CARD-REBUILD-REDUCE A11 sibling: mirror of destring — `replace`
  // (targetPresent) rewrites `target` IN PLACE via single-column `cobra setcol`,
  // seed via rewrittenVars={target} (branch 2); `gen` appends `target`, pass it
  // in newVars (branch 1). Byte-identical vs PIET_NO_NMISS_CACHE=1.
  // BUG-665: the target is now a str# of the longest value (tostring_fmt_replace:
  // "big was double now str11"); a replaced column must not keep claiming its
  // numeric type or numeric display format.
  if (opts.stataGate) {
    // BUG-919: generate() labels the new variable the ado's way,
    // `_crcslbl NEW v` (_crcslbl.ado): the source's variable label, or the
    // source's NAME when it has none (w2z_tostring tz_gen_varlabel
    // `[Byte source]`). replace keeps the label on the replaced variable itself.
    std::optional<std::string> srcLabel;
    if (!doReplace) {
      srcLabel = varname;
      auto lit = session.varMeta.find(varname);
      if (lit != session.varMeta.end() && lit->second.label.has_value() &&
          !lit->second.label->empty())
        srcLabel = *lit->second.label;
    }
    VarMeta& vm = session.varMeta[target];
    vm.stataType = "str" + std::to_string(maxLen);
    if (doReplace) vm.format.reset();
    if (srcLabel.has_value()) vm.label = *srcLabel;
  }
  ojson tostringResult =
      targetPresent ? mutationResult(session, "tostring", /*newVars=*/{}, /*droppedVars=*/{},
                                     /*rewrittenVars=*/{target})
                    : mutationResult(session, "tostring", {target});
  if (!targetPresent) {
    if (auto err = placeGeneratedAfterSource(session, "tostring", varname, target,
                                             residentSetcol, tostringResult))
      return *err;
  }
  // C4-L4: memoize the committed row strings (UNESCAPED — csvEscape is
  // transport-only; the engine stores the unescaped value) for the chained
  // consumer. Put AFTER mutationResult (the fold erases the name first) and
  // after the generate() placement (see the destring site).
  if (residentSetcol && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol put;
    put.dtype = "factor";
    put.nRows = n;
    put.strs = std::move(outStrings);
    session.residentComputedColPut(target, std::move(put));
  }
  return tostringResult;
}

}  // namespace piet::handlers
