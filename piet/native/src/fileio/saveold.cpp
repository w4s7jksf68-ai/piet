// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::missing_kind -- insheet's float columns carry `.` only
#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/delimited_io.h"  // insheet: the one native delimited reader
#include "piet/display_hardening.h"  // evalScalarExpr -- `putexcel A1 = (expr)`
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt -- outfile writes through display formats
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"  // infix `in`: parseInRangeBounds
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"  // piet::fmt::general -- outfile's %10.0g
#include "piet/strutil.h"
#include "xlsx.h"
#include "file_command_support.h"

namespace {

// `nolabel`/`all`/`emptyok` change WHAT IS WRITTEN into the file, so they stay
// in the unimplemented list rather than being accepted and dropped.
const piet::eval::OptVocab& kVocabSaveold = piet::eval::registerOptVocab(
    {"saveold"}, {{"replace", "version"}, {"nolabel", "all", "emptyok", "orphans"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// saveold
// ===========================================================================
//
// Decision 5, and the whole reason this row exists. `saveold, version(12)` asks
// for a format this build cannot write; PIET writes 118 and SAYS so, rather
// than producing 118 bytes under a name that promises Stata 12. The version the
// user asked for is recorded as a dataset characteristic, so it survives into
// the file itself through the --in-meta sidecar and can be read back.
ojson runSaveold(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSaveold.known,
                                            kVocabSaveold.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.empty()) {
    return ioSyntax("saveold: missing the file. Usage: saveold \"file\" [, replace version(#)]");
  }
  if (words.size() > 1) {
    return ioSyntax("saveold: expected one filename, got `" + words[0] + " " + words[1] + "`");
  }
  std::string spelling = words[0];
  // The extension: Stata's `saveold` defaults to `.dta`, and this build CAN
  // write one, so the default is honoured rather than substituted.
  {
    const std::filesystem::path p(cecon::to_native_path(spelling));
    if (!p.has_extension()) spelling += ".dta";
  }

  const auto version = optStr(parsed.opts, "version");
  if (version.has_value() && version->empty()) {
    return ioSyntax("saveold: version() needs a value, e.g. version(12)");
  }
  if (version.has_value()) {
    // saveold writes releases for Stata 11 through 14 only; any other
    // version() is r(198) and no file is written (w2ar dx_svo_v10
    // `version(10)` 198 conf=601, log:4219).
    long long v = 0;
    if (!parseStrictInt(*version, &v) || v < 11 || v > 14) {
      return ioErrorRc("version(" + *version + ") invalid; saveold writes Stata 11 through 14 "
                       "datasets",
                       198);
    }
  }

  // The release follows version(): 11 and 12 write format 115, 13 (and the
  // bare form) 117, 14 format 118 -- genuine bytes of that release, whatever
  // the file's extension (a tempfile included), as Stata's saveold does
  // (w2ar_data_io_family.log dx_svo_019 rel 117, dx_svo_017_v12 / dx_svo_v11
  // first byte 115, dx_svo_075_v13 117, dx_svo_v14 118).
  int release = 117;
  if (version.has_value()) {
    long long v = 0;
    (void)parseStrictInt(*version, &v);
    release = v <= 12 ? 115 : (v == 13 ? 117 : 118);
  }
  // runSaveData names the spelling it was handed (`effectiveFp`) in its echo
  // and in every refusal (BUG-410), so its block goes out as it is.
  ojson saved = runSaveData(spelling, hasFlag(parsed.opts, "replace"), session, release);
  if (isErrorBlock(saved)) return saved;

  // EXACT text, pinned by the pangram section and by a do-file stdout check.
  const std::string note = "(saveold: PIET writes Stata format " + std::to_string(release) + ")";
  ojson out = ojson::array();
  out.push_back(std::move(saved));
  // `display`, NOT `echo`: type "echo" is the channel that renders the typed
  // command line back (`. saveold "f", replace`). A note sent there reads as if
  // the USER wrote it, and nothing downstream counts it as output -- which is
  // exactly how it went missing: the file was written, the sentence was built,
  // and both selftest legs saw `<no display lines>`.
  out.push_back(displayBlock(note));
  return out;
}

}  // namespace piet::handlers
