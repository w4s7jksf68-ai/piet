// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data observation expansion.
#include "data_io_internal.h"
#include "piet/handlers/data_values.h"
#include "piet/handlers/observations.h"
#include "piet/eval/macros.h"  // evalArith: a name expand cannot read
namespace piet::handlers {
using namespace data_io_detail;


// ---------------------------------------------------------------------------
// `isid VARLIST` — uniqueness assertion (P1 wave). PIET-native: the engine has
// no isid verb, and the check is a single pass over the key columns' mmap'd
// payloads, so shelling out would cost a scratch .hbin for nothing.
//
// Oracle (StataBE 19.5, auto.dta): `isid make` → silent, rc 0; `isid foreign` →
// "variable foreign does not uniquely identify the observations", r(459);
// `isid nosuchvar` → r(111). Stata names ONE offending variable list in the
// message and pluralizes on arity, which is reproduced below.
//
// Missing values are ordinary key values here (Stata's isid does not exempt
// them), and each key cell is compared by its exact f64 bits so the extended
// missings `.a`-`.z` (data/missing.h) stay distinct from `.` — the same
// kind-exact identity the expression layer uses.
// The uniqueness check of isid.ado (CheckUniq) over the observations `ds` holds: nullopt when
// the key identifies them, else the r(459) / r(111) refusal.
static std::optional<ojson> isidCheck(Datastore* ds, const std::vector<std::string>& varlist,
                                      bool missok) {
  if (varlist.empty()) return engineErrorBlock("isid: variable list required");

  for (const auto& v : varlist) {
    if (!ds->has(v)) {
      // r(111) class — same wording shape as Stata's "variable X not found".
      return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    }
  }

  const int64_t n = ds->nRows();
  if (n == 0) {  // no observation is trivially unique (one is checked for a missing key)
    return std::nullopt;
  }

  // Build one 64-bit hash per row over the key tuple, then find duplicates by
  // hashing rows into buckets and confirming byte equality inside a bucket
  // (a hash collision must never be reported as a duplicate).
  struct KeyCol {
    const double* f64 = nullptr;
    const int32_t* codes = nullptr;
  };
  std::vector<KeyCol> keys;
  keys.reserve(varlist.size());
  for (const auto& v : varlist) {
    const cecon::Column& c = ds->col(v);
    KeyCol kc;
    if (c.dtype == cecon::DType::Factor) {
      kc.codes = c.codes();
    } else {
      kc.f64 = c.f64();
    }
    keys.push_back(kc);
  }

  // isid.ado CheckUniq: without `missok' a missing key value is r(459) before
  // uniqueness is looked at ("... should never be missing"; markout, strok: a
  // numeric `.'-`.z' or an empty string). w2ah_data_verbs.log ah_isid_missing:
  // rc 459 on a key whose third value is `.' -- main accepted it at rc 0.
  // BUG-1135: `missok' skips that check and a missing value is then one more key
  // value (two `.' keys do not identify their rows).
  for (size_t k = 0; k < keys.size() && !missok; ++k) {
    for (int64_t i = 0; i < n; ++i) {
      const bool miss = keys[k].codes != nullptr ? keys[k].codes[i] < 0
                                                 : std::isnan(keys[k].f64[i]);
      if (miss) {
        std::string joined;
        for (size_t j = 0; j < varlist.size(); ++j) {
          if (j) joined += " ";
          joined += varlist[j];
        }
        return rcErrorBlock((varlist.size() == 1 ? "variable " : "variables ") + joined +
                                " should never be missing",
                            459);
      }
    }
  }

  auto cellBits = [&keys](size_t k, int64_t row) -> uint64_t {
    if (keys[k].codes != nullptr) return static_cast<uint64_t>(
        static_cast<uint32_t>(keys[k].codes[row]));
    uint64_t bits;
    std::memcpy(&bits, keys[k].f64 + row, sizeof bits);
    return bits;
  };
  auto rowsEqual = [&](int64_t a, int64_t b) {
    for (size_t k = 0; k < keys.size(); ++k)
      if (cellBits(k, a) != cellBits(k, b)) return false;
    return true;
  };

  std::unordered_map<uint64_t, std::vector<int64_t>> buckets;
  buckets.reserve(static_cast<size_t>(n) * 2);
  for (int64_t i = 0; i < n; ++i) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a offset basis
    for (size_t k = 0; k < keys.size(); ++k) {
      h ^= cellBits(k, i);
      h *= 1099511628211ull;
    }
    auto& bucket = buckets[h];
    for (int64_t prior : bucket) {
      if (rowsEqual(prior, i)) {
        std::string joined;
        for (size_t j = 0; j < varlist.size(); ++j) {
          if (j) joined += " ";
          joined += varlist[j];
        }
        return rcErrorBlock(
            (varlist.size() == 1 ? "variable " : "variables ") + joined +
                " do" + (varlist.size() == 1 ? "es" : "") +
                " not uniquely identify the observations",
            459);
      }
    }
    bucket.push_back(i);
  }
  return std::nullopt;
}


// `isid varlist using FILE [, missok]`: isid.ado reads the file in place of the data in memory and
// leaves r(N) = its observations. Its `sort` option rewrites the file, which this build does not do.
ojson runIsidUsing(const std::vector<std::string>& varlist, const std::string& usingFile,
                   SessionState& session, bool missok, bool sort) {
  if (varlist.empty()) return rcErrorBlock("varlist required", 100);
  if (sort)
    return ojson(eval::unsupportedOption(
        "sort", "isid ... using ..., sort rewrites the using file sorted, which this build does not do"));
  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingFile);
  if (!ur.ok) return ur.errBlock;
  Datastore usingDs(ur.hbinPath);
  if (auto bad = isidCheck(&usingDs, varlist, missok)) return *bad;
  if (!missok) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(usingDs.nRows()));
    eval::declareRPosted(session.eval, "isid", {"N"});
  }
  return ojson::array();
}

ojson runIsid(const std::vector<std::string>& varlist, SessionState& session, bool missok,
              bool sort) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (auto bad = isidCheck(ds, varlist, missok)) return *bad;
  if (!sort) return ojson::array();  // unique: silent, rc 0 (Stata prints nothing)
  // BUG-1135: `sort' then sorts by the varlist unless the sort marker already
  // begins with it, and says which: "(data now sorted by x)" / "(data already
  // sorted by x)" (isid.ado; StataBE 19.5).
  std::string joined;
  for (const auto& v : varlist) joined += (joined.empty() ? "" : " ") + v;
  std::vector<std::string> marker;
  {
    std::istringstream in(session.eval.sortedBy ? session.eval.sortedBy() : std::string());
    for (std::string k; in >> k;) marker.push_back(k);
  }
  const bool already = varlist.size() <= marker.size() &&
                       std::equal(varlist.begin(), varlist.end(), marker.begin());
  ojson out = ojson::array();
  if (!already) {
    ojson sorted = runSort(varlist, session);
    if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    out.push_back(std::move(sorted));
  }
  ojson note;
  note["type"] = "display";
  note["class"] = "note";
  note["text"] = "(data " + std::string(already ? "already" : "now") + " sorted by " + joined + ")";
  out.push_back(std::move(note));
  return out;
}


// ---------------------------------------------------------------------------
// P1 wave data verbs — expand / fillin / cross / contract. Each is a thin
// forwarder onto the engine subcommand of the same name (the joinby shape:
// validate here, transform there). The engine's own doc banners carry the
// StataBE 19.5 oracle pins.

ojson runExpand(const std::string& countArg, const std::optional<std::string>& genVar,
                SessionState& session) {
  return runExpandIf(countArg, genVar, std::nullopt, std::nullopt, session);
}


// `expand N [if] [in]`: the observations the qualifier selects become N copies
// each, the others stay once (w2_esample.log:1076 E6 `expand 2 if id==3`: rc 0,
// _N 61 -> 67). Built as filter -> expand the selected rows N-1 times -> append
// those copies after the data, which is where Stata puts created observations
// and the order Frame::expand writes them in (rep by rep, each in data order).
ojson runExpandIf(const std::string& countArg, const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  std::string n = trim(countArg);
  if (n.empty()) {
    return engineErrorBlock("expand: specify how many copies, e.g. `expand 2`");
  }
  // Stata accepts an EXPRESSION here (a per-observation count). This build
  // implements the constant form only and says so rather than guessing.
  // A bare name that is neither a variable nor a value the expression evaluator
  // knows is r(111) "NAME not found" (vs-stata bank `expand nosuchvar`).
  if (strutil::isValidStataIdentifier(n) && !session.datastore()->has(n)) {
    bool isStr = false;
    if (!eval::evalArith(n, &session.eval, nullptr, &isStr).has_value())
      return eval::errorBlock("engine_error", n + " not found", std::nullopt, std::nullopt,
                              std::nullopt, "r(111)");
  }
  bool constantCount = true;
  for (char ch : n)
    if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '-' && ch != '+') constantCount = false;
  if (!constantCount) {
    // A per-observation count: a number or an expression of the variables. Counts that are
    // missing, zero or negative leave the observation as it is, a noninteger count is rounded,
    // and the copies are added after the data, one round of them at a time (vs-stata bank).
    if (genVar.has_value() && !genVar->empty()) {
      if (auto r = strutil::newVarNameRefusal(*genVar, /*resultRows=*/true)) return nameRefusalBlock(*r);
      ojson guard = newVarGuardDV(session, "expand", *genVar);
      if (!guard.is_null() && !guard.empty()) return guard;
    }
    const std::string savedLeaf = session.leafCmdExpanded;
    // expand is not an r-class command: the helper commands below must not leave their r() behind.
    struct RRestore {
      SessionState& s;
      eval::OrderedFloatDict saved;
      explicit RRestore(SessionState& ss) : s(ss), saved(ss.eval.rValues) {}
      ~RRestore() { s.eval.rValues = saved; }
    } rRestore(session);
    const std::string C = "__piet_ex_c", S = "__piet_ex_s";
    auto run = [&](const std::string& cmd) -> std::optional<ojson> {
      ojson b = piet::dispatch("quietly " + cmd, session);
      session.leafCmdExpanded = savedLeaf;
      if (b.is_array()) {
        for (const auto& x : b)
          if (x.is_object() && x.value("type", std::string()) == "error") return x;
      } else if (b.is_object() && b.value("type", std::string()) == "error") {
        return b;
      }
      return std::nullopt;
    };
    auto countOf = [&](const std::string& cond) -> int64_t {
      (void)run("count if " + cond);
      const double* nv = session.eval.rValues.find("N");
      return nv != nullptr ? static_cast<int64_t>(*nv) : 0;
    };
    auto cleanup = [&]() {
      for (const char* v : {"__piet_ex_c", "__piet_ex_s"}) {
        Datastore* dd = session.datastore();
        if (dd != nullptr && dd->has(v)) (void)run(std::string("drop ") + v);
      }
    };
    const int64_t rowsBefore0 = session.datastore()->nRows();
    std::string qual;
    if (ifExpr.has_value() && !trim(*ifExpr).empty()) qual += " if (" + *ifExpr + ")";
    if (inRange.has_value() && !trim(*inRange).empty()) qual += " in " + *inRange;
    std::optional<ojson> failed = run("generate double " + C + " = (" + n + ")");
    if (!failed) failed = run("generate byte " + S + " = 1" + qual);
    if (failed) {
      cleanup();
      return *failed;
    }
    (void)run("replace " + S + " = 0 if missing(" + S + ")");
    const int64_t nMissing = countOf(S + " == 1 & missing(" + C + ")");
    const int64_t nNonint = countOf(S + " == 1 & !missing(" + C + ") & " + C + " != floor(" + C + ")");
    (void)run("replace " + C + " = round(" + C + ") if " + S + " == 1 & !missing(" + C + ")");
    const int64_t nNeg = countOf(S + " == 1 & !missing(" + C + ") & " + C + " < 0");
    const int64_t nZero = countOf(S + " == 1 & !missing(" + C + ") & " + C + " == 0");
    (void)run("replace " + C + " = 1 if " + S + " == 0 | missing(" + C + ") | " + C + " < 1");
    (void)run("summarize " + C + ", meanonly");
    const double* mx = session.eval.rValues.find("max");
    const int64_t maxCopies = mx != nullptr ? static_cast<int64_t>(*mx) : 1;
    ojson blocksOut = ojson::array();
    auto note = [&](int64_t k, const char* what, const char* tail) {
      if (k <= 0) return;
      ojson d;
      d["type"] = "display";
      d["text"] = "(" + std::to_string(k) + " " + what + (k == 1 ? "" : "s") + " " + tail + ")";
      blocksOut.push_back(std::move(d));
    };
    auto ignoredNote = [&](int64_t k, const char* what) {
      if (k <= 0) return;
      ojson d;
      d["type"] = "display";
      d["text"] = "(" + std::to_string(k) + " " + what + " count" + (k == 1 ? "" : "s") +
                  " ignored; observation" + (k == 1 ? "" : "s") + " not deleted)";
      blocksOut.push_back(std::move(d));
    };
    ignoredNote(nMissing, "missing");
    note(nNonint, "noninteger count", "rounded to integer");
    ignoredNote(nNeg, "negative");
    ignoredNote(nZero, "zero");
    // The copies: round k holds every observation whose count reaches k, in data order.
    std::string headPath = session.datastore()->path();
    std::string cur = headPath;
    for (int64_t k = 2; k <= maxCopies; ++k) {
      InRangeFilterResult fr;
      try {
        fr = applyIfAndInFilter(session, headPath, C + " >= " + std::to_string(k), std::nullopt);
      } catch (const CobraError& ce) {
        cleanup();
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      if (!fr.ok) {
        cleanup();
        return inRangeRejectError(fr, std::string(), "expand");
      }
      const std::string outK = nextPath(session.workdir(), "expand_round");
      const CobraRunResult ar = runCobra({"append", "--data", cur, "--using", fr.dataPath, "--out", outK,
                                          "--type-clash", "error"});
      if (!ar.ok) {
        cleanup();
        CobraError ce = errorFromRaw(ar);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      cur = outK;
    }
    if (cur != headPath) session.load(cur);
    cleanup();
    if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
      bool any = false;
      for (size_t i = static_cast<size_t>(rowsBefore0); i < cells->size(); ++i) {
        any = any || (*cells)[i] != 0.0;
        (*cells)[i] = 0.0;
      }
      if (any) (void)esampleRewriteHeadMask(session, *cells);
    }
    if (genVar.has_value() && !genVar->empty()) {
      ojson r = runGen(*genVar + " = _n > " + std::to_string(rowsBefore0), std::nullopt, session);
      if (r.is_object() && r.value("type", std::string()) == "error") return r;
      session.varMeta[*genVar].stataType = "byte";
      session.setVarLabel(*genVar, "Expanded observation type");
    }
    blocksOut.push_back(mutationResult(session, "expand",
                                       genVar.has_value() && !genVar->empty()
                                           ? std::vector<std::string>{*genVar}
                                           : std::vector<std::string>{}));
    return blocksOut;
  }
  if (genVar.has_value() && !genVar->empty()) {
    // BUG-1120 review: the new-variable rule with _se and the r(table) row names:
    // `expand 2, gen(_cons)' is "_cons invalid varname" r(198) (StataBE 19.5).
    if (auto r = strutil::newVarNameRefusal(*genVar, /*resultRows=*/true))
      return nameRefusalBlock(*r);
    ojson guard = newVarGuardDV(session, "expand", *genVar);
    if (!guard.is_null() && !guard.empty()) return guard;
  }
  const int64_t rowsBefore = session.datastore()->nRows();
  std::string outPath = nextPath(session.workdir(), "expand");
  const bool qualified = (ifExpr.has_value() && !ifExpr->empty()) ||
                         (inRange.has_value() && !inRange->empty());
  long long copies = 0;
  try {
    copies = std::stoll(n);
  } catch (const std::exception&) {
    copies = 0;  // the engine names the malformed count below
  }
  CobraRunResult cr;
  if (!qualified || copies <= 1) {
    // N <= 1 leaves every observation once whatever the qualifier selects, but
    // the range is still read: `expand 1 in 9` on 3 rows is "observation numbers
    // out of range" r(198) (StataBE 19.5, BUG-1115).
    if (inRange.has_value() && !inRange->empty()) {
      const InRangeBounds rr = parseInRangeBounds(*inRange, rowsBefore);
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "expand");
      }
    }
    SessionState::ResidentEligible residentExpand(session);  // BUG-394
    cr = runCobra({"expand", "--data", *session.currentHbin(), "--n", n, "--out", outPath});
  } else {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *session.currentHbin(), ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "expand");
    std::string created = fr.dataPath;
    if (copies > 2) {
      created = nextPath(session.workdir(), "expand_copies");
      const CobraRunResult er = runCobra({"expand", "--data", fr.dataPath, "--n",
                                          std::to_string(copies - 1), "--out", created});
      if (!er.ok) {
        CobraError ce = errorFromRaw(er);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
    }
    session.ensureMaterializedHead("expand-if");
    SessionState::ResidentEligible residentExpand(session);  // the append's --out is the head
    cr = runCobra({"append", "--data", *session.currentHbin(), "--using", created, "--out",
                   outPath, "--type-clash", "error"});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  // e(sample) T2: a created observation is outside the estimation sample; the
  // originals keep their marker (w2_esample.log:1076 E6 `0|0|28|67|`; StataBE
  // capture w2_esample_copies X1 `old=23|created=0`). The copies carried the
  // mask like any column, so their cells are rewritten to 0.
  if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
    bool any = false;
    for (size_t i = static_cast<size_t>(rowsBefore); i < cells->size(); ++i) {
      any = any || (*cells)[i] != 0.0;
      (*cells)[i] = 0.0;
    }
    if (any && !esampleRewriteHeadMask(session, *cells)) {
      return engineErrorBlock(
          "expand: the observations were created, but the estimation-sample marker could not "
          "be cleared on them; e(sample) is not reliable until the next estimation.");
    }
  }
  // B4-W3b `generate(newvar)`. Built from `_n > <rows before>` AFTER the
  // expansion, because that is the only fact that distinguishes an original
  // from a copy: the copies are byte-identical to their sources in every other
  // column. Checked BEFORE the engine call (above) so a name collision cannot
  // leave the dataset expanded but unmarked.
  if (genVar.has_value() && !genVar->empty()) {
    ojson r = runGen(*genVar + " = _n > " + std::to_string(rowsBefore), std::nullopt, session);
    if (r.is_object() && r.value("type", std::string()) == "error") return r;
    session.varMeta[*genVar].stataType = "byte";
    session.setVarLabel(*genVar, "Expanded observation type");
  }
  return mutationResult(session, "expand", genVar.has_value() && !genVar->empty()
                                               ? std::vector<std::string>{*genVar}
                                               : std::vector<std::string>{});
}


ojson runFillin(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("fillin: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  // fillin.ado: `varlist(min=2)` -- the names are looked up first (`fillin
  // nosuchvar` r(111), 6db98565ca3320c5), then one variable is r(102) "too few
  // variables specified" (vs-stata bank 3a1a9a2ae6c0d349 `fillin foreign`).
  if (varlist.size() < 2)
    return eval::errorBlock("engine_error", "too few variables specified", std::nullopt,
                            std::nullopt, std::nullopt, "r(102)");
  // Then `confirm new var _fillin`: r(110) "variable _fillin already defined"
  // (vs-stata bank 647a32e95fa55565, 91e299205ce2c433).
  if (ds->has("_fillin"))
    return eval::errorBlock("engine_error", "variable _fillin already defined", std::nullopt,
                            std::nullopt, std::nullopt, "r(110)");
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "fillin");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentFillin(session);  // BUG-394
    cr = runCobra({"fillin", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  session.varMeta["_fillin"].stataType = "byte";  // vs-stata bank: describe shows _fillin as a byte
  return mutationResult(session, "fillin", {"_fillin"});
}


ojson runCross(const std::string& usingPathArg, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  // Reuse joinby's `using` resolution (.dta/.csv pre-convert + virtual-save
  // materialization) so `cross using x.dta` works exactly like `joinby ... using x.dta`.
  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;

  std::vector<std::string> newCols;
  int64_t usingRows = 0, masterRows = 0;
  std::map<std::string, UsingVarMeta> uMeta;
  {
    Datastore usingDs(ur.hbinPath);
    Datastore* master = session.datastore();
    for (const auto& nm : usingDs.names())
      if (!master->has(nm)) newCols.push_back(nm);
    usingRows = usingDs.nRows();
    masterRows = master->nRows();
    uMeta = usingVarMetaOf(session, ur.usingPath, ur.metaPath, usingDs);
  }
  // A using file with no observations stops cross.ado inside its own expand/sort
  // of the using data, on its row-number tempvar: r(111), the data as they were
  // (StataBE 19.5, named as a fresh session names it; BUG-1105).
  if (usingRows == 0) return rcErrorBlock("variable __000001 not found", 111);
  std::string outPath = nextPath(session.workdir(), "cross");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentCross(session);  // BUG-394
    cr = runCobra({"cross", "--data", *session.currentHbin(), "--using", ur.hbinPath, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  // e(sample) T2: each master observation keeps its marker on its pairing with
  // the FIRST using observation, every other pairing reads 0 (StataBE capture
  // w2_esample_copies X3 `23|183|m0=69`, X3b `k1=23|k2=0|k3=0`). The engine
  // writes those pairings first, in master order, and the rest after them
  // (Frame::cross, src/data/frame2.cpp), so rows from masterRows on read 0.
  if (std::optional<std::vector<double>> cells = esampleHeadMask(session); cells && usingRows > 1) {
    for (size_t p = static_cast<size_t>(masterRows); p < cells->size(); ++p) (*cells)[p] = 0.0;
    if (!esampleRewriteHeadMask(session, *cells)) {
      return engineErrorBlock(
          "cross: the data were formed, but the estimation-sample marker could not be "
          "cleared on the added pairings; e(sample) is not reliable until the next estimation.");
    }
  }
  adoptUsingOwnVars(session, ur, uMeta, newCols);
  return mutationResult(session, "cross", newCols);
}


// contract.ado, in the steps it takes: confirm the new names, count the observations of each
// combination of the varlist (weights add up, if/in and `nomiss` leave observations out), `zero`
// adds the combinations the sample does not have, then the cumulative and percent variables, each
// compressed. vs-stata bank: `contract foreign rep78, zero` 12 rows; all four new variables with
// zero nomiss -> 10 rows, r() the summarize of the count (percent / cpercent only).
ojson runContractFull(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr)
    return eval::noDataLoaded();
  const std::vector<std::string>& varlist = parsed.varlist;
  if (varlist.empty()) return engineErrorBlock("contract: variable list required");
  auto valued = [&](const char* key) -> std::optional<std::string> {
    for (const auto& [k, v] : parsed.opts)
      if (k == key && !v.isBool) return strutil::trim(v.strValue);
    return std::nullopt;
  };
  auto flag = [&](const char* key) {
    for (const auto& [k, v] : parsed.opts)
      if (k == key && (v.isBool ? v.boolValue : true)) return true;
    return false;
  };
  {
    Datastore* d = session.datastore();
    for (const auto& v : varlist)
      if (!d->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  const std::string freq = valued("freq").value_or("_freq");
  const std::optional<std::string> cfreq = valued("cfreq"), percent = valued("percent"),
                                   cpercent = valued("cpercent");
  // float and format() only shape the percent variables: without percent() or cpercent() they
  // are a mistake, the first checked being float (vs-stata ado: r(198) either way).
  if (!percent.has_value() && !cpercent.has_value() &&
      (flag("float") || !valued("format").value_or("").empty()))
    return rcErrorBlock("percent or cpercent must be specified", 198);
  const std::string fmt = valued("format").value_or("%8.2f");
  for (const auto& nm : std::vector<std::string>{freq, cfreq.value_or(""), percent.value_or(""),
                                                 cpercent.value_or("")}) {
    if (nm.empty()) continue;
    if (!strutil::isValidStataIdentifier(nm))
      return rcErrorBlock(nm + " invalid name", 198);
    if (session.datastore()->has(nm)) return rcErrorBlock("variable " + nm + " already defined", 110);
  }
  const std::string W = "__piet_ct_w", FILL = "_fillin";
  const std::string savedLeaf = session.leafCmdExpanded;
  auto run = [&](const std::string& cmd) -> std::optional<ojson> {
    ojson b = piet::dispatch("quietly " + cmd, session);
    session.leafCmdExpanded = savedLeaf;
    if (b.is_array()) {
      for (const auto& x : b)
        if (x.is_object() && x.value("type", std::string()) == "error") return x;
    } else if (b.is_object() && b.value("type", std::string()) == "error") {
      return b;
    }
    return std::nullopt;
  };
  // r() stays as it was unless percentages are asked for.
  eval::OrderedFloatDict savedVals = session.eval.rValues;
  const bool wantsPercent = percent.has_value() || cpercent.has_value();
  std::string cond;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    cond = "(" + *parsed.ifExpr + ")";
  if (flag("nomiss"))
    for (const auto& v : varlist) cond += (cond.empty() ? "" : " & ") + std::string("!missing(") + v + ")";
  std::string qual;
  if (!cond.empty()) qual += " if " + cond;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    qual += " in " + *parsed.inRange;
  std::string wexp = "1";
  if (parsed.weightVar.has_value() && !strutil::trim(*parsed.weightVar).empty())
    wexp = *parsed.weightVar;
  else if (parsed.weightExp.has_value() && !strutil::trim(*parsed.weightExp).empty())
    wexp = *parsed.weightExp;
  std::string keys;
  for (const auto& v : varlist) keys += (keys.empty() ? "" : " ") + v;
  std::optional<ojson> failed = run("generate double " + W + " = " + wexp);
  if (!failed) failed = run("collapse (sum) " + freq + " = " + W + qual + ", by(" + keys + ")");
  // The count carries no variable label (collapse's "(sum) ..." label would leak the scratch name).
  if (!failed) failed = run("label variable " + freq);
  if (!failed && flag("zero")) {
    failed = run("fillin " + keys);
    if (!failed) failed = run("replace " + freq + " = 0 if " + FILL + " == 1");
    if (!failed) failed = run("drop " + FILL);
  }
  std::vector<std::string> made = {freq};
  if (!failed && cfreq.has_value()) {
    failed = run("generate double " + *cfreq + " = sum(" + freq + ")");
    if (!failed) failed = run("label variable " + *cfreq + " \"Cumulative frequency\"");
    made.push_back(*cfreq);
  }
  if (!failed && wantsPercent) {
    failed = run("summarize " + freq + ", meanonly");
    const double* total = session.eval.rValues.find("sum");
    const std::string tot = total != nullptr ? eval::pythonFloatRepr(*total) : "1";
    const std::string pt = flag("float") ? "float" : "double";
    if (!failed && percent.has_value()) {
      failed = run("generate " + pt + " " + *percent + " = 100*" + freq + "/" + tot);
      if (!failed) failed = run("label variable " + *percent + " \"Percent\"");
      if (!failed) failed = run("format " + *percent + " " + fmt);
      made.push_back(*percent);
    }
    if (!failed && cpercent.has_value()) {
      failed = run("generate " + pt + " " + *cpercent + " = 100*sum(" + freq + ")/" + tot);
      if (!failed) failed = run("label variable " + *cpercent + " \"Cumulative percent\"");
      if (!failed) failed = run("format " + *cpercent + " " + fmt);
      made.push_back(*cpercent);
    }
  }
  if (!failed) {
    std::string toCompress;
    for (const auto& m : made) toCompress += " " + m;
    failed = run("compress" + toCompress);
  }
  if (!failed) failed = run("format " + freq + " %12.0g");
  if (!failed && cfreq.has_value()) failed = run("format " + *cfreq + " %12.0g");
  {
    Datastore* d = session.datastore();
    if (d != nullptr && d->has(W)) (void)run("drop " + W);
  }
  if (failed) return *failed;
  if (!wantsPercent) {
    eval::replaceRClass(session.eval);
    for (const auto& kv : savedVals.items()) session.eval.rValues.set(kv.first, kv.second);
  }
  session.datasetChanged = true;
  return mutationResult(session, "contract", made);
}


ojson runContract(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return eval::noDataLoaded();
  }
  if (varlist.empty()) return engineErrorBlock("contract: variable list required");
  Datastore* ds = session.datastore();
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
  }
  if (ds->has("_freq")) return rcErrorBlock("variable _freq already defined", 110);
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  // e(sample) T2: a result row carries its group's first observation's marker,
  // as collapse's (StataBE capture w2_esample_copies K6 `contract id` 4 of 11,
  // K7 `contract id m0` m0_diff=0).
  const EsampleGroupMarks esMarks = esampleGroupMarksBefore(session, varlist, false);
  std::string outPath = nextPath(session.workdir(), "contract");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentContract(session);  // BUG-394
    cr = runCobra({"contract", "--data", *session.currentHbin(), "--vars", joined, "--out", outPath});
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  if (!esampleGroupMarksAfter(session, esMarks, varlist, EsampleLostBy::kContract)) {
    return engineErrorBlock(
        "contract: the data were contracted, but the estimation-sample marker could not be "
        "written onto the result; e(sample) is not reliable until the next estimation.");
  }
  return mutationResult(session, "contract", {"_freq"});
}
}
