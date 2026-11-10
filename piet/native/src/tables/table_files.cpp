// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table files.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// File writing
// ===========================================================================

// SIBLING TEMP + rename(2), never a write into the destination. Copied in shape
// from `export delimited`'s writer (fileio/delimited_export.cpp): a
// failure never leaves a truncated file where a readable one used to be, and a
// destination that is ALSO a source (outreg2's `append` ledger) is safe because
// the caller has already read it fully into memory before calling this.
//
// `userPath` is what the user TYPED and is the only spelling that ever reaches
// the output; `resolved` is what gets opened. Bundle D echoed the resolved
// absolute path and baked a build machine's workdir into a golden.
ojson writeTextFileAtomic(const std::string& cmd, const std::string& resolved,
                          const std::string& userPath, const std::string& bytes) {
  const std::string tmp = resolved + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary);
    if (!f) return tblError(cmd + ": cannot write " + userPath, "data_io");
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath + " (disk full?)", "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    return tblError(cmd + ": failed writing " + userPath, "data_io");
  }
  return ojson();  // null == success; callers test .is_null()
}


std::string readWholeFile(const std::string& path, bool* okOut) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) {
    *okOut = false;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *okOut = true;
  return ss.str();
}


// The existence/replace/append gate every file-writing verb here shares.
// Returns a non-null error block when the write must not happen.
ojson guardOverwrite(const std::string& cmd, const std::string& resolved,
                     const std::string& userPath, bool replace, bool append) {
  if (cecon::native_exists(resolved) && !replace && !append) {
    // coarseRc maps "file ... already exists" to rc 602, which is Stata's own
    // code for this, so the wording is load-bearing rather than decorative.
    return tblError(cmd + ": file " + userPath +
                    " already exists (add `replace` to overwrite)");
  }
  return ojson();
}


// `using f` with no extension gets one. Stata's table commands each have their
// own default and getting it wrong writes a file the user cannot find.
std::string ensureExt(const std::string& path, const std::string& ext) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return path;
  return path + ext;
}


std::string lowerExt(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return strutil::lower(path.substr(dot));
}
}
