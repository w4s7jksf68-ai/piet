// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// tostring: numeric -> string conversion (default and usedisplayformat
// rendering, missing-value rendering).
#include "piet/handlers/data_values.h"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "datamgmt_support.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule
#include "string_conversion_internal.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;
using string_conversion_detail::destringNoteBlock;
using string_conversion_detail::placeGeneratedAfterSource;

namespace {
using storage::stataDefaultFormat;
}  // namespace

// ---------------------------------------------------------------------------
// tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat] — BS-003/D4 (BETA
// B-data). PIET-NATIVE numeric->string(factor) conversion, the mirror of
// runDestring above.

namespace {
// Default (non-usedisplayformat) numeric->string rendering — mirrors
// row_evaluation.cpp's file-local coerceStr() convention (integer -> plain
// digits via eval::pythonIntStrFromDouble, non-integer -> eval::gFormat);
// kept as its own copy per this codebase's established per-TU convention
// (see errorFromRaw's own header note above) rather than exporting
// row_evaluation.cpp's file-local helper. Missing is handled by the caller
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
