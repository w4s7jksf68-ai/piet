// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Reading the dataset a `using` clause names (merge, append, joinby, cross):
// the file's path, its variable metadata and value labels, its observation
// count, and the refusal of a delimited file. The common data-command helpers
// and their preamble come from datamgmt_support.h.
#pragma once
#include "datamgmt_support.h"

namespace piet { namespace handlers { namespace dv_detail {
// What a using file (merge / append / joinby) states about one of its
// variables: its label, format and storage type (read by usingVarMetaOf in data_append.cpp).
struct UsingVarMeta {
  std::string label;
  std::string format;
  std::optional<std::string> stataType;
};
std::map<std::string, UsingVarMeta> usingVarMetaOf(SessionState& session, const std::string& usingPath,
                                                   const std::string& usingMetaPath,
                                                   Datastore& usingDs);

// The dataset a `using` names (merge, append, joinby, cross), resolved against
// session.cwd. [D] merge/append/joinby/cross: "If filename is specified without
// an extension, .dta is assumed" -- unless the bare name is itself a file (an
// extensionless tempfile PIET wrote). `shown`, when given, takes the same
// suffix, so a missing file is reported under the name Stata looked for.
std::string resolveUsingDataset(SessionState& session, const std::string& rawPath,
                                std::string* shown = nullptr);

// The using file's value labels after merge/append/joinby/cross: each variable
// in `brought` keeps the set the using file attaches to it, the using's
// definitions are copied (when `copyDefinitions`) unless the master already
// defines the name, and every attachment then reads through its set. A .dta
// states both in its import-dta sidecar (`usingMetaPath`), a native checkpoint
// in the record kept beside its path.
void adoptUsingValueLabels(SessionState& session, const std::string& usingPath,
                           const std::string& usingMetaPath, const std::vector<std::string>& brought,
                           bool copyDefinitions);

// Stata tempfiles carry genuine DTA bytes without a .dta suffix. Match the
// same XML/legacy headers accepted by use before resolving a using file.
inline bool usingFileIsDta(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  char header[11] = {};
  input.read(header, sizeof header);
  const auto size = input.gcount();
  const bool xml = size == 11 && std::string(header, 11) == "<stata_dta>";
  const auto version = static_cast<unsigned char>(header[0]);
  const bool legacy = size >= 3 && (version == 113 || version == 114 || version == 115) &&
                      (header[1] == 1 || header[1] == 2) && header[2] == 1;
  return xml || legacy;
}

// describe using needs only the observation count in the file header. Read
// that small fixed prefix rather than converting a using file twice.
inline std::optional<int64_t> usingDtaRows(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  std::string header(256, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<size_t>(input.gcount()));
  size_t offset = 6, width = 4;
  bool little = header.size() > 1 && header[1] == 2;
  if (header.rfind("<stata_dta>", 0) == 0) {
    const bool v117 = header.find("<release>117</release>") != std::string::npos;
    if (!v117 && header.find("<release>118</release>") == std::string::npos) return std::nullopt;
    const auto n = header.find("<N>");
    if (n == std::string::npos) return std::nullopt;
    offset = n + 3;
    width = v117 ? 4 : 8;
    little = header.find("<byteorder>LSF</byteorder>") != std::string::npos;
  } else if (header.size() < 3 ||
             (static_cast<unsigned char>(header[0]) != 114 &&
              static_cast<unsigned char>(header[0]) != 115)) return std::nullopt;
  if (header.size() < offset + width) return std::nullopt;
  uint64_t count = 0;
  for (size_t i = 0; i < width; ++i) {
    const auto byte = static_cast<unsigned char>(header[offset + (little ? i : width - 1 - i)]);
    count |= static_cast<uint64_t>(byte) << (8 * i);
  }
  if (count > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  return static_cast<int64_t>(count);
}

// A delimited `using` file the engine would not convert. Stata reads no text
// file here: merge, joinby and cross of any .csv answer "file X not Stata
// format" r(610), append "file X not found" r(601). Reading one is PIET's own
// extension, so a file it cannot read faithfully -- BUG-414: a row wider than
// the names line, whose extra field the converter used to drop at rc 0 -- gets
// that command's code and Stata's words exactly, the file as typed (StataBE 19.5,
// 2026-11-03: `append using f.csv` prints only "file f.csv not found", with data
// in memory or none).
inline ojson delimitedUsingRefused(const std::string& shown, int rc) {
  return rcErrorBlock("file " + shown + (rc == 601 ? " not found" : " not Stata format"), rc);
}

}}}  // namespace piet::handlers::dv_detail
