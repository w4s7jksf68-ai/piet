// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Where ado files and packages live: sysdir, adopath, findfile, the ado-autoload
// lookup (findOnAdopath), and the offline net / ado commands.
#include "piet/handlers/session_commands.h"
#include "piet/user_paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "piet/bundle_paths.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path / env_utf8 (BUG-050/051)
#include "environment_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace environment_detail;

namespace {

const eval::OptVocab& kVocabFindfile = eval::registerOptVocab({"findfile"}, {{"path", "all", "nodescend"}, {}});

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// The first whitespace-delimited word of a string. Used only where a
// subcommand is being recovered from raw text: taking the whole tail would
// make `net install foo`'s sub read as "install foo" and print it back.
std::string firstWord(const std::string& s) {
  const std::string t = trim(s);
  const size_t sp = t.find_first_of(" \t");
  return sp == std::string::npos ? t : t.substr(0, sp);
}

// The directory the running binary lives in, with a trailing separator. Seeds
// STATA/BASE/SITE below: PIET has no Stata installation to point at, and its
// own install root is the only directory that is both real and stable across
// the dev tree, a packaged .app and a headless acceptance run.
std::string binaryDir() {
  namespace fs = std::filesystem;
  std::string dir;
  if (std::optional<std::string> exe = piet::executablePath()) {
    dir = fs::path(*exe).parent_path().u8string();
  }
  if (dir.empty()) {
    std::error_code ec;
    const fs::path p = fs::current_path(ec);
    if (!ec) dir = p.u8string();
  }
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += "/";
  return dir;
}

const char* const kSysdirNames[] = {"STATA", "BASE", "SITE", "PLUS", "PERSONAL", "OLDPLACE"};

bool isSysdirName(const std::string& name) {
  for (const char* n : kSysdirNames) {
    if (name == n) return true;
  }
  return false;
}

// The `~`-prefixed seeds are stored UNEXPANDED on purpose: resolveUserPath
// expands `~` at lookup time, so findfile still finds them, and bare `sysdir`
// prints the short form a Stata user recognises rather than this machine's
// home directory.
// PLUS is Stata's platform default ([P] sysdir): on macOS
// ~/Library/Application Support/Stata/ado/plus/ (the oracle machine's `sysdir`,
// stata_probe6 of the g07 diagnosis: PLUS ~/Library/Application
// Support/Stata/ado/plus/), where installed community ados live -- findfile
// boottest.ado answers .../plus/b/boottest.ado (w2t_boottest_family.log:131);
// ~/ado/plus/ is the Unix default. PERSONAL stays ~/ado/personal/ on every
// platform although macOS Stata's is ~/Documents/Stata/ado/personal/: findfile
// searches PERSONAL before PLUS, and a stat inside ~/Documents asks macOS for
// Documents-folder access (TCC) on every headless run. Known gap: `sysdir`
// prints PERSONAL differently from macOS Stata.
void seedSysdir(SessionState& session) {
  if (!session.sysdir.empty()) return;
  const std::string bin = binaryDir();
  session.sysdir["STATA"] = bin;
  session.sysdir["BASE"] = bin;
  session.sysdir["SITE"] = bin;
#ifdef __APPLE__
  session.sysdir["PLUS"] = "~/Library/Application Support/Stata/ado/plus/";
#else
  session.sysdir["PLUS"] = "~/ado/plus/";
#endif
  session.sysdir["PERSONAL"] = "~/ado/personal/";
  session.sysdir["OLDPLACE"] = "~/ado/";
}

// Stata's own default adopath order. Seeding it BEFORE any `adopath +` (not
// only inside findfile) is load-bearing: `adopath + X` on an empty list would
// otherwise leave X as the ONLY search entry, so a later findfile would miss
// files the default path would have found -- an append silently behaving like
// a replace.
void ensureAdopath(SessionState& session) {
  if (!session.adopath.empty()) return;
  seedSysdir(session);
  static const char* const kOrder[] = {"BASE", "SITE", ".", "PERSONAL", "PLUS", "OLDPLACE"};
  for (const char* key : kOrder) {
    if (std::string(key) == ".") {
      session.adopath.push_back(".");
      continue;
    }
    auto it = session.sysdir.find(key);
    if (it != session.sysdir.end() && !it->second.empty()) session.adopath.push_back(it->second);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// net / ado -- accept-notes. Never refuse: these sit in the PREAMBLE of the
// packages that use them, so a refusal costs the whole do-file, while an
// honest note costs one line and leaves the analysis to run against whatever
// is already on disk.
//
// BUG-1461: `net install NAME, from(DIR)` naming a LOCAL directory does what
// Stata does, because there the answer is knowable offline: a relative DIR is
// r(198) "DIR/ is not an absolute path or a URL" (a leading ~ is the home
// directory), a directory with no stata.toc is r(601), one whose NAME.pkg is
// absent is r(601) "could not load NAME.pkg from DIR/", and otherwise the
// package's `f'/`F' files are copied into the `net set ado' directory (PLUS by
// default) under their first letter, `_' for a leading underscore. A file the
// package lists but DIR lacks is r(601) and copies nothing; one already there
// with other content is r(602) unless `replace'; identical ones are "up to date"
// (StataBE 19.5, 2026-11-03). Stata's stata.trk installation record is not kept,
// as PIET's `ado' keeps no package list. A URL keeps the offline note.
//
// `net from DIR' on a local DIR lists its stata.toc and makes DIR the site a
// later `net install NAME' (no from()) installs from; a refused `net from'
// keeps the previous site ("current site is still ..."). A URL site, or none,
// keeps the offline note.
namespace {
// `net set ado` target and the current `net from` site, kept beside the sysdir
// names; the lower-case keys never collide with Stata's upper-case ones.
constexpr const char* kNetAdoKey = "net_ado";
constexpr const char* kNetFromKey = "net_from";

std::string withSlash(std::string d) {
  if (d.empty() || d.back() != '/') d += '/';
  return d;
}

std::string readWhole(const std::string& path) {
  std::ifstream in(cecon::to_native_path(path), std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A local site's directory with its trailing slash, or the r(198)/r(601)
// refusal `net from`/`net install, from()` give when it is not one.
std::optional<nlohmann::ordered_json> localSite(const std::string& from, SessionState& session,
                                                std::string& dir) {
  namespace fs = std::filesystem;
  const std::string src = (!from.empty() && from[0] == '~') ? resolveUserPath(session, from) : from;
  dir = withSlash(src);
  if (!fs::path(cecon::to_native_path(src)).is_absolute())
    return errBlockRc(dir + " is not an absolute path or a URL", "r(198)");
  std::error_code ec;
  if (!fs::is_regular_file(cecon::to_native_path(dir + "stata.toc"), ec)) {
    std::string text = "file " + dir + "stata.toc not found\n" + dir +
                       " either\n  1)  does not exist or\n  2)  is not a Stata download "
                       "directory (has no stata.toc file).";
    if (session.sysdir.count(kNetFromKey))
      text += "\n\ncurrent site is still " + session.sysdir[kNetFromKey];
    return errBlockRc(text, "r(601)");
  }
  return std::nullopt;
}

// `net from DIR`: the toc's `d' lines between rules, then its `p' packages.
nlohmann::ordered_json netFromLocal(const std::string& from, SessionState& session) {
  std::string dir;
  if (auto refused = localSite(from, session, dir)) return *refused;
  session.sysdir[kNetFromKey] = dir;
  const std::string rule(78, '-');
  std::string text = rule + "\n" + dir, packages;
  std::istringstream toc(readWhole(dir + "stata.toc"));
  for (std::string line; std::getline(toc, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 2 || (line[1] != ' ' && line[1] != '\t')) continue;
    const std::string rest = trim(line.substr(2));
    if (line[0] == 'd') {
      text += "\n" + rest;
    } else if (line[0] == 'p') {
      const std::string pkg = firstWord(rest);
      std::string desc = trim(rest.substr(pkg.size()));
      std::string padded = pkg;
      if (padded.size() < 18) padded.resize(18, ' ');
      packages += "\n    " + padded + desc;
    }
  }
  text += "\n" + rule;
  if (!packages.empty()) text += "\n\nPACKAGES you could -net describe-:" + packages + "\n" + rule;
  return displayBlock(text);
}

nlohmann::ordered_json netInstallLocal(const std::string& name, const std::string& from,
                                       bool replace, SessionState& session) {
  namespace fs = std::filesystem;
  std::string dir;
  if (auto refused = localSite(from, session, dir)) return *refused;
  std::error_code ec;
  if (!fs::is_regular_file(cecon::to_native_path(dir + name + ".pkg"), ec))
    return errBlockRc("file " + dir + name + ".pkg not found\ncould not load " + name +
                          ".pkg from " + dir,
                      "r(601)");

  std::vector<std::string> files;
  {
    std::istringstream pkg(readWhole(dir + name + ".pkg"));
    for (std::string line; std::getline(pkg, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() > 2 && (line[0] == 'f' || line[0] == 'F') && (line[1] == ' ' || line[1] == '\t'))
        files.push_back(trim(line.substr(2)));
    }
  }
  seedSysdir(session);
  std::string target = session.sysdir.count(kNetAdoKey) ? session.sysdir[kNetAdoKey] : "PLUS";
  if (session.sysdir.count(target)) target = session.sysdir[target];
  target = withSlash(resolveUserPath(session, target));

  std::string text = "checking " + name + " consistency and verifying not already installed...";
  struct Copy {
    std::string from, to;
    bool exists;
  };
  std::vector<Copy> copies;
  std::vector<std::string> differ;
  for (const auto& f : files) {
    const std::string from1 = dir + f;
    if (!fs::is_regular_file(cecon::to_native_path(from1), ec))
      return errBlockRc(text + "\nfile " + from1 + " not found\ncould not copy " + from1 +
                            "\n(no action taken)",
                        "r(601)");
    const std::string base = fs::path(cecon::to_native_path(f)).filename().u8string();
    const std::string letter = base.empty() || base[0] == '_'
                                   ? std::string("_")
                                   : std::string(1, static_cast<char>(std::tolower(
                                                        static_cast<unsigned char>(base[0]))));
    const std::string to = target + letter + "/" + base;
    const bool exists = fs::is_regular_file(cecon::to_native_path(to), ec);
    if (exists && readWhole(to) == readWhole(from1)) continue;
    if (exists) differ.push_back(to);
    copies.push_back({from1, to, exists});
  }
  if (!differ.empty() && !replace) {
    text += "\nthe following files already exist and are different:";
    for (const auto& d : differ) text += "\n    " + d;
    return errBlockRc(text + "\nno files installed or copied\n(no action taken)", "r(602)");
  }
  if (copies.empty()) return displayBlock(text + "\nall files already exist and are up to date.");
  if (!differ.empty()) {
    text += "\nthe following files will be replaced:";
    for (const auto& d : differ) text += "\n    " + d;
  }
  text += "\ninstalling into " + target + "...";
  for (const auto& c : copies) {
    const fs::path to = cecon::to_native_path(c.to);
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(cecon::to_native_path(c.from), to, fs::copy_options::overwrite_existing, ec);
    if (ec) return errBlockRc(text + "\ncould not copy " + c.from, "r(603)");
  }
  return displayBlock(text + "\ninstallation complete.");
}
}  // namespace

nlohmann::ordered_json runNet(const std::string& raw, const std::vector<std::string>& varlist,
                              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              SessionState& session) {
  const std::string sub =
      varlist.empty() ? lower(firstWord(argAfterCommand(raw))) : lower(varlist[0]);
  if (sub == "install") {
    const std::string name = varlist.size() > 1 ? dequote(varlist[1]) : "";
    std::string from;
    bool replace = false;
    for (const auto& [k, v] : opts) {
      if (k == "from" && !v.isBool) from = dequote(trim(v.strValue));
      if (k == "replace") replace = true;
    }
    if (from.empty() && session.sysdir.count(kNetFromKey)) from = session.sysdir[kNetFromKey];
    if (!from.empty() && from.find("://") == std::string::npos)
      return netInstallLocal(name, from, replace, session);
    return displayBlock("(net install " + name + ": PIET is offline -- skipped)");
  }
  if (sub == "from" && varlist.size() > 1) {
    const std::string where = dequote(varlist[1]);
    if (where.find("://") == std::string::npos) return netFromLocal(where, session);
    session.sysdir.erase(kNetFromKey);  // a URL site: later installs are offline notes
  }
  // `net set ado DIR|SYSDIRNAME`: where a local install copies to; silent, as in Stata.
  if (sub == "set" && varlist.size() > 2 && lower(varlist[1]) == "ado") {
    seedSysdir(session);  // a first entry would otherwise stop the seeding
    const std::string where = dequote(varlist[2]);
    const std::string up = upper(where);
    session.sysdir[kNetAdoKey] = isSysdirName(up) ? up : resolveUserPath(session, where);
    return nlohmann::ordered_json::array();
  }
  if (sub.empty()) return displayBlock("(net: PIET is offline -- skipped)");
  return displayBlock("(net " + sub + ": PIET is offline -- skipped)");
}

nlohmann::ordered_json runAdo(const std::string& raw, const std::vector<std::string>& varlist) {
  const std::string sub =
      varlist.empty() ? lower(firstWord(argAfterCommand(raw))) : lower(varlist[0]);
  const std::string name = varlist.size() > 1 ? dequote(varlist[1]) : "";
  if (sub == "uninstall") {
    return displayBlock("(ado uninstall " + name +
                        ": PIET has no ado directory -- skipped)");
  }
  if (sub.empty() || sub == "dir") {
    return displayBlock("(ado dir: PIET has no ado directory -- nothing installed)");
  }
  std::string what = sub;
  if (!name.empty()) what += " " + name;
  return displayBlock("(ado " + what + ": PIET has no ado directory -- skipped)");
}

// ---------------------------------------------------------------------------
// adopath [+|++|- PATH]
//
// Records where the package believed its own files live. A later `findfile`
// reads the list back, and so does the dispatcher's ado autoload (BUG-442):
// a command the package ships as `NAME.ado` in a directory it added here is
// found and run, as in Stata.
nlohmann::ordered_json runAdopath(const std::string& raw, const std::vector<std::string>& varlist,
                                  SessionState& session) {
  ensureAdopath(session);

  std::vector<std::string> toks;
  for (const auto& v : varlist) toks.push_back(dequote(v));
  if (toks.empty()) {
    for (const auto& t : tokenizeShellArgs(argAfterCommand(raw))) {
      if (!t.text.empty()) toks.push_back(t.text);
    }
  }

  if (toks.empty()) {
    std::string text;
    for (size_t i = 0; i < session.adopath.size(); ++i) {
      if (i) text += "\n";
      text += "  [" + std::to_string(i + 1) + "]  \"" + session.adopath[i] + "\"";
    }
    if (text.empty()) text = "  (adopath is empty)";
    return displayBlock(text);
  }

  // `adopath +"/x"` and `adopath + "/x"` are the same command; the op is split
  // off the first token rather than assumed to be a token of its own.
  const std::string tok0 = toks[0];
  size_t k = 0;
  while (k < tok0.size() && (tok0[k] == '+' || tok0[k] == '-')) ++k;
  const std::string op = tok0.substr(0, k);
  if (op != "+" && op != "++" && op != "-") {
    return errBlock("adopath: expected `+', `++' or `-' before a path", "syntax_error");
  }
  std::string pathArg = trim(tok0.substr(k));
  if (pathArg.empty() && toks.size() > 1) pathArg = toks[1];
  pathArg = dequote(pathArg);
  if (pathArg.empty()) return errBlock("adopath " + op + ": path required", "syntax_error");

  const std::string resolved = resolveUserPath(session, pathArg);
  auto& list = session.adopath;
  auto found = std::find(list.begin(), list.end(), resolved);
  if (op == "+") {
    // Appending a path already on the list would give findfile two chances to
    // find the same file and change nothing else, so the existing position wins.
    if (found == list.end()) list.push_back(resolved);
  } else if (op == "++") {
    if (found != list.end()) list.erase(found);
    list.insert(list.begin(), resolved);
  } else {
    list.erase(std::remove(list.begin(), list.end(), resolved), list.end());
  }
  // The note names the path AS WRITTEN; the list holds the RESOLVED one. A note
  // that echoed the resolution would bake the capture machine's cwd into every
  // golden that pins it (`adopath + "ado"` prints /mnt/.../ado on one
  // machine and /home/... on the next), and the resolved form is not lost -- a
  // bare `adopath` prints exactly what was recorded.
  return displayBlock("(adopath " + op + " " + pathArg +
                      ": recorded for findfile and ado autoload)");
}

// ---------------------------------------------------------------------------
// sysdir [set NAME "PATH"]
nlohmann::ordered_json runSysdir(const std::string& raw, const std::vector<std::string>& varlist,
                                 SessionState& session) {
  seedSysdir(session);

  std::vector<std::string> toks;
  for (const auto& v : varlist) toks.push_back(v);
  if (toks.empty()) {
    for (const auto& t : tokenizeShellArgs(argAfterCommand(raw))) {
      if (!t.text.empty()) toks.push_back(t.text);
    }
  }
  const std::string sub = toks.empty() ? "" : lower(toks[0]);

  if (sub.empty() || sub == "list") {
    std::string text;
    for (size_t i = 0; i < sizeof(kSysdirNames) / sizeof(kSysdirNames[0]); ++i) {
      const std::string name = kSysdirNames[i];
      // Right-aligned in a width-8 field: "PERSONAL" is exactly 8 characters,
      // which is why Stata's own column is that wide.
      const std::string pad(name.size() < 8 ? 8 - name.size() : 0, ' ');
      if (i) text += "\n";
      text += pad + name + ":  " + session.sysdir[name];
    }
    return displayBlock(text);
  }

  if (sub != "set") {
    return errBlock("sysdir: expected `set' or no subcommand", "syntax_error");
  }
  if (toks.size() < 2) return errBlock("sysdir set: directory name required", "syntax_error");
  const std::string name = upper(dequote(toks[1]));
  if (!isSysdirName(name)) {
    return errBlock("sysdir set: unknown directory name `" + dequote(toks[1]) +
                    "' (STATA BASE SITE PLUS PERSONAL OLDPLACE)",
                    "syntax_error");
  }
  // Taken off the RAW text, not from toks[2]: the path is routinely quoted and
  // contains spaces, and rebuilding it from tokens would lose them.
  std::string pathArg;
  {
    const std::string tail = argAfterCommand(raw);  // everything after "sysdir"
    const std::string lt = lower(tail);
    const size_t setAt = lt.find("set");
    if (setAt != std::string::npos) {
      std::string rest = trim(tail.substr(setAt + 3));
      const size_t sp = rest.find_first_of(" \t");
      pathArg = (sp == std::string::npos) ? "" : dequote(trim(rest.substr(sp + 1)));
    }
  }
  if (pathArg.empty() && toks.size() > 2) pathArg = dequote(toks[2]);
  if (pathArg.empty()) return errBlock("sysdir set " + name + ": path required", "syntax_error");

  // Resolved at RECORD time (like adopath): a relative entry resolved later
  // would follow the session cwd around and point somewhere the user never
  // named. resolveUserPath still handles the `~` in the seeded defaults, which
  // are deliberately left unexpanded.
  const std::string resolved = resolveUserPath(session, pathArg);
  session.sysdir[name] = resolved;
  // As written, not as resolved -- same reproducible-golden reason as adopath
  // above; bare `sysdir` reports the resolved directory that was recorded.
  return displayBlock("(sysdir set " + name + " " + pathArg +
                      ": recorded for findfile)");
}

namespace {

// The directories findfile searches, in order, for NAME: each entry itself and
// then its letter subdirectory. Shared with the dispatcher's ado autoload, so
// a command is found exactly where findfile would report it.
std::vector<std::string> searchAdoDirs(const SessionState& session,
                                       const std::vector<std::string>& searchList,
                                       const std::string& name, bool nodescend, bool all) {
  // Each directory is searched itself and then its letter subdirectory ([P]
  // findfile, p.pdf pp.246-247: "nodescend specifies that findfile not follow
  // Stata's normal practice of searching in letter subdirectories of
  // directories in the path, as well as in the directories themselves"); the
  // letter is the file name's first character, lower-cased ([P] macro, adosubdir:
  // "Typically, the directory name would be the first letter of filename").
  // An installed ado lives there: findfile boottest.ado is
  // .../ado/plus/b/boottest.ado on the oracle machine (w2t_boottest_family.log:131).
  namespace fs = std::filesystem;
  std::string letter;
  if (!nodescend) {
    const std::string base = fs::path(cecon::to_native_path(name)).filename().u8string();
    if (!base.empty() && base.size() == name.size()) {
      const unsigned char c0 = static_cast<unsigned char>(base[0]);
      if (std::isalnum(c0) || c0 == '_') letter = std::string(1, static_cast<char>(std::tolower(c0)));
    }
  }
  std::vector<std::string> hits;
  for (const auto& entry : searchList) {
    // "." is resolved HERE, not when it was recorded: cwd moves, and an entry
    // frozen at record time would search a directory the do-file has left.
    const std::string dir = (entry == ".") ? sessionCwd(session) : resolveUserPath(session, entry);
    std::vector<fs::path> cands = {cecon::to_native_path(dir) / cecon::to_native_path(name)};
    if (!letter.empty())
      cands.push_back(cecon::to_native_path(dir) / letter / cecon::to_native_path(name));
    bool stop = false;
    for (const auto& cand : cands) {
      std::error_code ec;
      if (fs::is_regular_file(cand, ec) && !ec) {
        hits.push_back(cand.u8string());
        if (!all) {
          stop = true;
          break;
        }
      }
    }
    if (stop) break;
  }

  return hits;
}

}  // namespace

// ---------------------------------------------------------------------------
// findfile FNAME [, path(dirlist) all]
nlohmann::ordered_json runFindfile(const std::string& raw, const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(opts, kVocabFindfile.known)) return ojson(*err);

  const std::string name = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);
  if (name.empty()) return errBlock("findfile: filename required", "syntax_error");

  bool all = false;
  bool nodescend = false;
  std::string pathOpt;
  for (const auto& [k, v] : opts) {
    if (k == "all" && v.isBool) all = v.boolValue;
    else if (k == "nodescend" && v.isBool) nodescend = v.boolValue;
    else if (k == "path") pathOpt = v.strValue;
  }

  std::vector<std::string> searchList;
  if (!pathOpt.empty()) {
    // path() REPLACES the search list (Stata's own semantics). Entries are
    // separated by ";" or whitespace; both spellings appear in the corpus.
    std::string cur;
    for (const char c : pathOpt) {
      if (c == ';' || c == ' ' || c == '\t') {
        if (!cur.empty()) searchList.push_back(dequote(cur));
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) searchList.push_back(dequote(cur));
  } else {
    ensureAdopath(session);
    searchList = session.adopath;
  }

  const std::vector<std::string> hits = searchAdoDirs(session, searchList, name, nodescend, all);

  if (hits.empty()) return errBlockRc("file \"" + name + "\" not found", "r(601)");

  if (all && hits.size() > 1) {
    // r(fn) carries a LIST here, so each path is quoted -- an unquoted join
    // would be unparseable the moment one directory contains a space.
    std::string joined;
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i) joined += " ";
      joined += "\"" + hits[i] + "\"";
    }
    session.eval.rStrings.set("fn", joined);
  } else {
    session.eval.rStrings.set("fn", hits[0]);
  }

  std::string text;
  for (size_t i = 0; i < hits.size(); ++i) {
    if (i) text += "\n";
    text += hits[i];
  }
  return displayBlock(text);
}

std::optional<std::string> findOnAdopath(SessionState& session, const std::string& fname) {
  ensureAdopath(session);
  // The seeded system entries (BASE/SITE are PIET's own binary directory, the
  // rest the `~` defaults) are left out: they hold this machine's installed
  // community packages, and what PIET runs must not depend on what happens to
  // be installed where it runs. A package's own code is found -- beside the
  // do-file (".") or in a directory the do-file put on the path.
  const std::string bin = binaryDir();
  std::vector<std::string> searchList;
  for (const auto& entry : session.adopath) {
    if (entry == bin || entry.rfind("~", 0) == 0) continue;
    searchList.push_back(entry);
  }
  const std::vector<std::string> hits =
      searchAdoDirs(session, searchList, fname, /*nodescend=*/false, /*all=*/false);
  if (hits.empty()) return std::nullopt;
  return hits[0];
}

}  // namespace piet::handlers
