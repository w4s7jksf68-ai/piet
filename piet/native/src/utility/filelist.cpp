// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"
#include "piet/handlers/descriptive.h"  // postDescribeResults: filelist.ado ends in `des`
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
#include "piet/dispatcher.h"  // save(): the dispatched `save`, `preserve` and `restore`
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
  // save(file): the list goes to disk and the data in memory are left as they
  // were; `replace` is for that file (filelist.ado: preserve, build, save).
  const std::string saveTo = optStr(parsed.opts, "save").value_or(std::string());
  const bool replaceSaved = hasFlag(parsed.opts, "replace");
  const bool hadData = session.hasData();
  if (!saveTo.empty() && hadData) {
    ojson kept = piet::dispatch("preserve", session);
    if (kept.is_object() && kept.value("type", std::string()) == "error") return kept;
  }

  // A directory that is not there lists nothing: the answer is the empty list,
  // not an error (bank 5e8c2a189cc936cd: rc 0, "Number of files found = 0").
  std::error_code ec;
  const bool dirExists = std::filesystem::is_directory(cecon::to_native_path(resolved), ec) && !ec;

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
    if (!dirExists) {
      // nothing to walk
    } else if (recursive) {
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

  // filelist.ado ends with `qui compress`, `qui leftalign` and `format %15.0fc
  // fsize`: each string is the str# of its longest value (str1 at least) shown
  // left-aligned, and fsize the smallest integer type that holds every size
  // (byte -127..100, int -32767..32740, long -2147483647..2147483620), double
  // when one does not fit or is unknown. leftalign's last step is `des` on the
  // realigned strings, so r() is describe's (the vs-stata truth bank: r(N) 4,
  // r(k) 3, r(width) 11, r(changed) 1 after a four-file filelist).
  {
    size_t wDir = 1, wFile = 1;
    double lo = 0.0, hi = 0.0;
    bool integral = true;
    for (const auto& f : found) {
      wDir = std::max(wDir, f.dirname.size());
      wFile = std::max(wFile, f.filename.size());
      if (std::isnan(f.fsize) || f.fsize != std::floor(f.fsize)) integral = false;
      lo = std::min(lo, f.fsize);
      hi = std::max(hi, f.fsize);
    }
    std::string sizeType = "double";
    if (integral && !found.empty()) {
      if (lo >= -127 && hi <= 100) sizeType = "byte";
      else if (lo >= -32767 && hi <= 32740) sizeType = "int";
      else if (lo >= -2147483647.0 && hi <= 2147483620.0) sizeType = "long";
    }
    auto strMeta = [&](const std::string& name, size_t w) {
      VarMeta& m = session.varMeta[name];
      m.stataType = "str" + std::to_string(w);
      m.format = "%-" + std::to_string(w) + "s";
    };
    strMeta("dirname", std::min<size_t>(wDir, 2045));
    strMeta("filename", std::min<size_t>(wFile, 2045));
    VarMeta& sm = session.varMeta["fsize"];
    sm.stataType = sizeType;
    sm.format = "%15.0fc";
  }
  postDescribeResults(session, "filelist");
  ojson out = ojson::array();
  out.push_back(displayBlock("Number of files found = " + intText(
                                 static_cast<int64_t>(found.size())) + "\n"));
  if (saveTo.empty()) {
    out.push_back(mutationResult(session, "use"));
    return out;
  }
  ojson saved = piet::dispatch("save \"" + saveTo + "\"" + (replaceSaved ? ", replace" : ""), session);
  ojson back = hadData ? piet::dispatch("restore", session) : piet::dispatch("clear", session);
  if (saved.is_array()) {
    for (const auto& b : saved) out.push_back(b);
  } else {
    out.push_back(saved);
  }
  if (back.is_object() && back.value("type", std::string()) == "error") out.push_back(back);
  return out;
}


}  // namespace piet::handlers
