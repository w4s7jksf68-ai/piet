// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// filelist
// ===========================================================================
//
// The filesystem, as a dataset: one row per file, with dirname/filename/fsize.
// REPLACES what is in memory, which is why `filelist` is in the dispatcher's
// kNoDataCommands list and runs with nothing loaded.
//
// THE ECHOED PATH IS THE ONE THE USER TYPED (defect class 5). `resolveUserPath`
// is used to WALK the tree and nowhere else; every dirname cell and the count
// line are built from the typed string, so a do-file that later matches on
// `dirname == "data"` still matches.
// `globMatch` moved to piet/strutil.h so esttab/estout's coefficient selection
// can use the SAME matcher (BUG-496). Body unchanged; `using` keeps the call
// sites below spelled as they were.
using piet::strutil::globMatch;

ojson runFilelist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFilelist.known,
                                            kVocabFilelist.unimplemented))
    return ojson(*err);
  if (!parsed.varlist.empty())
    return utilSyntax("filelist: syntax is `filelist [, directory(dir) pattern(glob) "
                      "norecursive]` -- it takes no varlist.");

  const std::string typedDir = optStr(parsed.opts, "directory").value_or(std::string("."));
  const std::string pattern = optStr(parsed.opts, "pattern").value_or(std::string("*"));
  const bool recursive = !hasFlag(parsed.opts, "norecursive");
  const std::string resolved = resolveUserPath(session, typedDir);

  std::error_code ec;
  if (!std::filesystem::is_directory(cecon::to_native_path(resolved), ec) || ec)
    return utilErrorRc("filelist: `" + typedDir + "' is not a directory.", 601);

  struct FileRow {
    std::string dirname;
    std::string filename;
    double fsize = 0.0;
  };
  std::vector<FileRow> found;

  // The dirname a row reports = the directory the USER named, plus the
  // subdirectory the file was found under, so a `directory("data")` scan
  // reports `data` and `data/raw` -- never the absolute path resolveUserPath
  // produced.
  auto record = [&](const std::filesystem::path& full) {
    std::error_code fec;
    if (!std::filesystem::is_regular_file(full, fec) || fec) return;
    const std::string name = full.filename().string();
    if (!globMatch(pattern, name)) return;
    std::error_code rec;
    std::filesystem::path rel =
        std::filesystem::relative(full.parent_path(), cecon::to_native_path(resolved), rec);
    std::string dirname = typedDir;
    if (!rec && !rel.empty() && rel.string() != ".") dirname = typedDir + "/" + rel.generic_string();
    std::error_code sec;
    const auto sz = std::filesystem::file_size(full, sec);
    found.push_back(FileRow{dirname, name,
                            sec ? std::numeric_limits<double>::quiet_NaN()
                                : static_cast<double>(sz)});
  };

  try {
    if (recursive) {
      for (std::filesystem::recursive_directory_iterator
               it(cecon::to_native_path(resolved), std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           it != end; it.increment(ec)) {
        if (ec) break;
        record(it->path());
      }
    } else {
      for (std::filesystem::directory_iterator
               it(cecon::to_native_path(resolved), std::filesystem::directory_options::skip_permission_denied, ec),
           end;
           it != end; it.increment(ec)) {
        if (ec) break;
        record(it->path());
      }
    }
  } catch (const std::exception& e) {
    return utilError(std::string("filelist: could not read `") + typedDir + "' -- " + e.what(),
                     "data_io");
  }

  std::sort(found.begin(), found.end(), [](const FileRow& a, const FileRow& b) {
    if (a.dirname != b.dirname) return a.dirname < b.dirname;
    return a.filename < b.filename;
  });

  if (found.empty()) {
    // No rows means no dataset: `cobra convert` on a header-only CSV would
    // produce a zero-observation frame that every later command then treats as
    // real data. r(2000) ("no observations") says what happened instead.
    session.eval.rValues.set("N", 0.0);
    return utilErrorRc("filelist: no files matching `" + pattern + "' found in `" + typedDir + "'.",
                       2000);
  }

  CsvTable t;
  t.names = {"dirname", "filename", "fsize"};
  t.isFactor = {1, 1, 0};
  t.rows.reserve(found.size());
  for (const auto& f : found)
    t.rows.push_back({f.dirname, f.filename, numCell(f.fsize)});

  std::string hbin;
  if (auto err = writeAndConvert(session, t, "filelist", &hbin)) return *err;

  // Adopted the way `use` adopts: the resident chain is DROPPED (a load from a
  // freshly-built file never continues it) and the display metadata plane is
  // cleared, because this dataset declares no labels of its own.
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.invalidateDatastore();

  session.eval.rValues.set("N", static_cast<double>(found.size()));
  ojson out = ojson::array();
  out.push_back(displayBlock("Number of files found = " + intText(
                                 static_cast<int64_t>(found.size())) + "\n"));
  out.push_back(mutationResult(session, "use"));
  return out;
}


}  // namespace piet::handlers
