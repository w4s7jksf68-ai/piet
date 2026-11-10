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

// `text` only ever meant anything for a URL transfer (it selects the transfer
// mode), and a URL source is refused outright below -- so for the local copy
// this command performs it is honoured by construction, as is `asis`, which
// names the byte-for-byte copy this does. `public` changes the destination's
// PERMISSIONS, which this does not do, so it is refused rather than dropped.
const piet::eval::OptVocab& kVocabCopy =
    piet::eval::registerOptVocab({"copy"}, {{"replace", "text", "asis"}, {"public"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace file_command_detail;

// ===========================================================================
// copy
// ===========================================================================
//
// Decision 8. A local file copy. A URL source is refused BY NAME rather than
// attempted: this build makes no network request from a Stata verb, and a
// `copy` that quietly produced nothing (or, worse, an empty file) would be the
// silent-failure shape.
ojson runCopy(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCopy.known,
                                            kVocabCopy.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.size() != 2) {
    return ioSyntax("copy: expected two filenames. Usage: copy from to [, replace]");
  }
  const std::string srcSpelling = words[0];
  const std::string dstSpelling = words[1];

  {
    const std::string low = lowerOf(srcSpelling);
    if (low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0 ||
        low.compare(0, 6, "ftp://") == 0) {
      return ioRefusal("copy", "no_network",
                       "copy: URL sources are not supported in this build (no network access from "
                       "Stata verbs)");
    }
  }

  const std::string src = resolveUserPath(session, srcSpelling);
  std::string dstSpell = dstSpelling;
  std::string dst = resolveUserPath(session, dstSpelling);
  if (!pathExists(src)) return ioErrorRc("file " + typedUserPath(srcSpelling) + " not found", 601);
  {
    std::error_code ec;
    if (std::filesystem::is_directory(cecon::to_native_path(dst), ec) && !ec) {
      const std::filesystem::path p(cecon::to_native_path(src));
      dst = (std::filesystem::path(cecon::to_native_path(dst)) / p.filename()).string();
      dstSpell = dstSpelling + "/" + p.filename().string();
    }
  }
  if (sameFile(src, dst)) {
    return ioError("copy: the source and the destination are the same file (" + srcSpelling + ")");
  }
  const bool replace = hasFlag(parsed.opts, "replace");
  if (auto err = refuseExisting(dst, dstSpell, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "copy", dst, dstSpell)) return *err;

  // Sibling temp + rename, so a failed copy never leaves a half file where a
  // readable one used to be.
  const std::string tmp = dst + ".piet_tmp";
  {
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(tmp), ec);
    ec.clear();
    std::filesystem::copy_file(cecon::to_native_path(src), cecon::to_native_path(tmp),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError("copy: " + srcSpelling + " -> " + dstSpell + ": " + ec.message(), "data_io");
    }
  }
  {
    std::error_code ec;
    std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(dst), ec);
    if (ec) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError("copy: " + srcSpelling + " -> " + dstSpell + ": " + ec.message(), "data_io");
    }
  }
  return silent();  // Stata's copy prints nothing
}

}  // namespace piet::handlers
