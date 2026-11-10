// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/io.h"
#include "piet/user_paths.h"

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

// outsheet writes through writeDelimitedFile with its own switches: `nonames`
// drops the header row (w2ar dx_outs_noname, log:893), `noquote` drops the
// quotes it otherwise puts round every string (log:865). `names` IS the
// default and is therefore honoured by construction.
const piet::eval::OptVocab& kVocabOutsheet = piet::eval::registerOptVocab(
    {"outsheet"}, {{"comma", "replace", "noquote", "nolabel", "names", "nonames", "delimiter",
                    "delim"},
                   {"double"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// outsheet
// ===========================================================================
//
// Decision 2. `export delimited` with the defaults swapped: Stata's `outsheet`
// writes TAB-separated by default (`comma` switches it) and writes the variable
// names as row 1. Everything else -- the value-label rendering, the missing
// spelling, the sibling-temp + rename commit -- is runExportDelimited's, so the
// two commands cannot disagree about the bytes.
ojson runOutsheet(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutsheet.known,
                                            kVocabOutsheet.unimplemented)) {
    return ojson(*err);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &varlist,
                  &pathArg)) {
    return ioSyntax("outsheet: missing the file. Usage: outsheet [varlist] using \"file\" "
                    "[, comma replace noquote nolabel nonames delimiter(D)]");
  }

  // outsheet's own rules (w2ar log:810-929): TAB by default (`comma`,
  // `delimiter()` change it), every numeric through its display format
  // (`1.235e+10` for a %10.0g double), strings and label texts ALWAYS quoted
  // with an inner quote left as is (`"q"r"`, log:815), dates formatted and
  // bare; `noquote` drops the quotes (log:865) and `nonames` the header
  // (log:893). `if` restricts the rows (dx_outs_if, log:838).
  DelimWriteSpec spec;
  spec.verb = "outsheet";
  spec.delim = '\t';
  {
    std::string derr;
    if (!readDelimiterOpt(parsed.opts, &spec.delim, &derr)) return ioError(derr, "unsupported_option");
  }
  if (hasFlag(parsed.opts, "comma")) spec.delim = ',';
  spec.noLabel = hasFlag(parsed.opts, "nolabel");
  spec.header = !hasFlag(parsed.opts, "nonames");
  spec.dataFmt = true;
  spec.quote = hasFlag(parsed.opts, "noquote") ? DelimWriteSpec::Quote::kNone
                                               : DelimWriteSpec::Quote::kAllVerbatim;

  const bool replace = hasFlag(parsed.opts, "replace");
  std::string resolved = resolveUserPath(session, pathArg);
  // "If filename is specified without a suffix, .out is assumed" (outsheet
  // help).
  assumeSuffix(&resolved, &pathArg, ".out");
  if (auto err = refuseExisting(resolved, pathArg, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "outsheet", resolved, pathArg)) return *err;

  spec.shown = pathArg;  // the writer names the file as typed (BUG-410)
  return writeDelimitedFile(varlist, resolved, spec, parsed.ifExpr, parsed.inRange, session);
}

}  // namespace piet::handlers
