// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle D (ENV, 2026-09-03) -- the environment verbs the ninety-
// paper AER corpus opens with.
//
// The bundle's design decision, stated once here because every function below
// inherits it: PIET spawns NO OS process, ever. `shell` is the only command in
// the corpus that would otherwise need one, and a refusal there stops the
// do-file on a line whose PURPOSE was almost always to tidy scratch files
// (twelve of the ninety packages call shell; nearly all of them for rm/mkdir/
// copy). So shell is TRANSLATED, verb by verb, onto the same std::filesystem
// operations runErase/runMkdir/runRmdir already use, and anything outside that
// allowlist is refused BY NAME rather than silently ignored. The translation is
// announced in a display block every time, because "the do-file's rm actually
// deleted something" is exactly the fact a reader of the log needs and the one
// a silent success would hide.
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

// Names the first UNQUOTED shell operator, or "" when there is none. These are
// refused rather than approximated: a pipe or a redirect means the do-file
// wanted a process's OUTPUT, and there is no honest translation of that onto a
// file operation -- translating only the left half would run a destructive verb
// and then discard the half the author actually read.
std::string unquotedOperator(const std::string& s) {
  bool inQuote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      inQuote = !inQuote;
      continue;
    }
    if (inQuote) continue;
    if (c == '|') return "|";
    if (c == '>') return ">";
    if (c == '<') return "<";
    if (c == ';') return ";";
    if (c == '`') return "`";
    if (c == '&' && i + 1 < s.size() && s[i + 1] == '&') return "&&";
  }
  return "";
}

// Resolves one user-written path and expands `*`/`?` in its LAST component by
// scanning the parent directory. Globbing a directory component is deliberately
// not supported: no package in the corpus writes one, and a half-implemented
// recursive walk would differ from a real shell in ways nothing here measures.
//
// An unmatched glob comes back as the literal resolved path, matching a POSIX
// shell's own nullglob-off default -- and, more usefully, letting the caller
// report "file <pattern> not found" instead of silently doing nothing.
std::vector<std::string> expandPathArg(const SessionState& session, const std::string& rawPath) {
  namespace fs = std::filesystem;
  const std::string resolved = resolveUserPath(session, rawPath);
  if (resolved.find('*') == std::string::npos && resolved.find('?') == std::string::npos) {
    return {resolved};
  }
  const fs::path p = cecon::to_native_path(resolved);
  const fs::path dir = p.parent_path();
  const std::string pattern = p.filename().u8string();
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string nm = it->path().filename().u8string();
    if (globMatch(pattern, nm)) out.push_back(it->path().u8string());
  }
  std::sort(out.begin(), out.end());
  if (out.empty()) out.push_back(resolved);
  return out;
}

// cp/mv semantics: a destination that ALREADY IS a directory receives the
// source under its own name. Without this, `cp a.dta out/` would try to
// overwrite the directory itself and fail with an error naming the wrong path.
std::string destinationFor(const std::string& dest, const std::string& src) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::is_directory(cecon::to_native_path(dest), ec) && !ec) {
    return (cecon::to_native_path(dest) / cecon::to_native_path(src).filename()).u8string();
  }
  return dest;
}

// The translation note names each path the way the do-file wrote it. Echoing
// the RESOLVED path instead would make the note machine-specific -- and the
// note is pinned by a freeze fixture, so an absolute scratch path would make
// that fixture non-reproducible on any other checkout. A path outside the
// session cwd keeps its absolute form, which is the edge case where seeing the
// whole path is exactly the point (`shell rm -rf /tmp/x`).
std::string noteRelative(const SessionState& session, const std::string& path) {
  std::string base = sessionCwd(session);
  if (base.empty()) return path;
  if (base.back() != '/' && base.back() != '\\') base += "/";
  if (path.size() > base.size() && path.compare(0, base.size(), base) == 0) {
    return path.substr(base.size());
  }
  return path;
}

std::string joinSpace(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += " ";
    out += v[i];
  }
  return out;
}

const char* const kShellAllowlistText =
    "shell: `%VERB%' is outside PIET's allowlist (rm rmdir mkdir cp mv touch echo ls dir cat "
    "type del md copy move xcopy). PIET runs no OS process; allowlisted verbs are translated "
    "to PIET file operations.";

}  // namespace

// ---------------------------------------------------------------------------
// shell
//
// The allowlist is not a convenience: it is the boundary between "the effect
// the do-file depended on happened" and "a process ran". Every accepted verb
// below maps onto the SAME std::filesystem calls runErase/runMkdir/runRmdir
// make, so shell can never reach further into the machine than PIET's own file
// commands already do, and every refusal names the verb it refused so a reader
// can tell a gap from a policy.
//
// Windows spellings (del/md/rd/copy/move/ren/xcopy and the /f /q /s switches)
// are in the same table rather than a separate branch because AER replication
// packages are written on both platforms and a Windows-authored `del *.tmp` is
// the same intent as a Unix `rm -f *.tmp`.
nlohmann::ordered_json runShell(const std::string& args, SessionState& session) {
  namespace fs = std::filesystem;
  const std::string line = trim(args);

  if (line.empty()) {
    return displayBlock("(shell: interactive shell is not available -- PIET runs no OS process)");
  }
  if (const std::string op = unquotedOperator(line); !op.empty()) {
    // "unsupported_command", not the default "engine_error": eval::coarseRc
    // maps that code to rc 199 and everything else here to the generic 1, and
    // `capture shell ...` + `_rc == 199` is what the corpus branches on. The
    // message names a file operation, so the engine_error path's own message
    // sniffing would not rescue it either.
    ojson b = errBlock("shell: the `" + op +
                       "' operator is outside PIET's allowlist. PIET runs no OS process; "
                       "allowlisted verbs are translated to PIET file operations.",
                       "unsupported_command");
    b["reason"] = "refused:shell_operator";
    return b;
  }

  const std::vector<ShellTok> toks = tokenizeShellArgs(line);
  if (toks.empty()) {
    return displayBlock("(shell: interactive shell is not available -- PIET runs no OS process)");
  }
  const std::string verb = lower(toks[0].text);

  auto refuseVerb = [&verb]() {
    std::string msg = kShellAllowlistText;
    const size_t at = msg.find("%VERB%");
    msg = msg.substr(0, at) + verb + msg.substr(at + 6);
    ojson b = errBlock(msg, "unsupported_command");  // rc 199 (see the operator refusal above)
    b["reason"] = "refused:shell_allowlist";
    return b;
  };

  // `echo` and `cd` consume their tail verbatim, so they are answered before
  // the flag scan -- `echo -n hi` must not lose its "-n" to a flag parser.
  if (verb == "echo") {
    std::vector<std::string> words;
    for (size_t i = 1; i < toks.size(); ++i) words.push_back(toks[i].text);
    std::string text = "(shell " + line + ": translated -> echo)";
    const std::string said = joinSpace(words);
    if (!said.empty()) text += "\n" + said;
    return displayBlock(text);  // one block -- see the tail of this function
  }
  if (verb == "cd") {
    // A translation would be a lie: the child shell's cwd dies with the child,
    // and silently moving PIET's own cwd instead would change where every LATER
    // command in the do-file reads and writes.
    return displayBlock("(shell cd: has no lasting effect -- use PIET's cd)");
  }

  bool force = false;
  bool recursive = false;
  bool parents = false;
  std::vector<std::string> operands;
  for (size_t i = 1; i < toks.size(); ++i) {
    const std::string& t = toks[i].text;
    const std::string lt = lower(t);
    if (!toks[i].quoted && lt.size() > 2 && lt.compare(0, 2, "--") == 0) {
      // Long flags are matched WHOLE. Folding them into the short-flag
      // character scan below would read the `r` in `--force` as -r.
      if (lt == "--force") force = true;
      else if (lt == "--recursive") recursive = true;
      else if (lt == "--parents") parents = true;
      continue;
    }
    if (!toks[i].quoted && lt.size() >= 2 && lt[0] == '-') {
      for (size_t k = 1; k < lt.size(); ++k) {
        if (lt[k] == 'f') force = true;
        else if (lt[k] == 'r') recursive = true;  // -R lowercased to here too
        else if (lt[k] == 'p') parents = true;
      }
      continue;
    }
    // Only the EXACT Windows switches count. Matching any "/x" token would eat
    // an absolute POSIX path whose first component happens to be one letter.
    if (!toks[i].quoted && (lt == "/f" || lt == "/q" || lt == "/y" || lt == "/s" || lt == "/e" ||
                            lt == "/i")) {
      if (lt == "/f" || lt == "/q" || lt == "/y") force = true;  // /q and /y suppress the prompt
      if (lt == "/s" || lt == "/e") recursive = true;
      continue;
    }
    operands.push_back(t);
  }

  std::vector<std::string> touched;   // resolved paths the translation acted on
  std::vector<ojson> extra;           // blocks from a delegated verb (ls/cat)
  std::string pietVerb;

  if (verb == "rm" || verb == "del" || verb == "erase") {
    pietVerb = "erase";
    if (operands.empty()) return errBlock("shell rm: filename required", "syntax_error");
    for (const auto& o : operands) {
      for (const auto& p : expandPathArg(session, o)) {
        const fs::path np = cecon::to_native_path(p);
        std::error_code ec;
        if (!fs::exists(np, ec)) {
          // -f's whole purpose in a teardown line is "do not stop if it is
          // already gone"; without it the do-file expects r(601) and a halt.
          if (force) continue;
          return errBlockRc("file " + p + " not found", "r(601)");
        }
        if (fs::is_directory(np, ec)) {
          if (!recursive) return errBlockRc("could not remove directory " + p, "r(693)");
          fs::remove_all(np, ec);
          if (ec) return errBlockRc("could not remove directory " + p, "r(693)");
        } else {
          fs::remove(np, ec);
          if (ec) return errBlock("erase: " + ec.message());
        }
        touched.push_back(p);
      }
    }
  } else if (verb == "rmdir" || verb == "rd") {
    pietVerb = "rmdir";
    if (operands.empty()) return errBlock("shell rmdir: directory name required", "syntax_error");
    for (const auto& o : operands) {
      for (const auto& p : expandPathArg(session, o)) {
        const fs::path np = cecon::to_native_path(p);
        std::error_code ec;
        // The is_directory guard is the same one runRmdir documents: remove()
        // would happily delete a plain FILE, which `rmdir` must never do.
        if (!fs::is_directory(np, ec) || ec) {
          return errBlockRc("could not remove directory " + p, "r(693)");
        }
        if (recursive) {
          fs::remove_all(np, ec);
          if (ec) return errBlockRc("could not remove directory " + p, "r(693)");
        } else if (!fs::remove(np, ec) || ec) {
          return errBlockRc("could not remove directory " + p, "r(693)");
        }
        touched.push_back(p);
      }
    }
  } else if (verb == "mkdir" || verb == "md") {
    pietVerb = "mkdir";
    if (operands.empty()) return errBlock("shell mkdir: directory name required", "syntax_error");
    for (const auto& o : operands) {
      const std::string p = resolveUserPath(session, o);
      const fs::path np = cecon::to_native_path(p);
      std::error_code ec;
      const bool made = parents ? fs::create_directories(np, ec) : fs::create_directory(np, ec);
      if (ec || !made) {
        // create_directories returns false (with no error) when the whole
        // chain already existed, which is exactly what -p asks to tolerate.
        std::error_code ec2;
        if (!(parents && !ec && fs::is_directory(np, ec2) && !ec2)) {
          return errBlockRc("could not create directory " + p, "r(693)");
        }
      }
      touched.push_back(p);
    }
  } else if (verb == "cp" || verb == "copy" || verb == "xcopy") {
    pietVerb = "copy";
    if (operands.size() < 2) return errBlock("shell copy: source and target required",
                                             "syntax_error");
    // xcopy IS the recursive spelling on Windows; a package that wrote it did
    // not also write /s, so treating it as a plain file copy would move one
    // file where a whole tree was expected.
    const bool rec = recursive || verb == "xcopy";
    const std::string dest = resolveUserPath(session, operands.back());
    std::vector<std::string> srcs;
    for (size_t i = 0; i + 1 < operands.size(); ++i) {
      for (const auto& s : expandPathArg(session, operands[i])) srcs.push_back(s);
    }
    for (const auto& s : srcs) {
      const fs::path sn = cecon::to_native_path(s);
      std::error_code ec;
      if (!fs::exists(sn, ec)) return errBlockRc("file " + s + " not found", "r(601)");
      const std::string tgt = destinationFor(dest, s);
      const fs::path tn = cecon::to_native_path(tgt);
      if (rec) {
        fs::copy(sn, tn, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
      } else {
        fs::copy_file(sn, tn, fs::copy_options::overwrite_existing, ec);
      }
      if (ec) return errBlock("copy: " + ec.message());
      touched.push_back(s);
    }
    touched.push_back(dest);
  } else if (verb == "mv" || verb == "move" || verb == "ren") {
    pietVerb = "rename";
    if (operands.size() < 2) return errBlock("shell move: source and target required",
                                             "syntax_error");
    const std::string dest = resolveUserPath(session, operands.back());
    std::vector<std::string> srcs;
    for (size_t i = 0; i + 1 < operands.size(); ++i) {
      for (const auto& s : expandPathArg(session, operands[i])) srcs.push_back(s);
    }
    for (const auto& s : srcs) {
      const fs::path sn = cecon::to_native_path(s);
      std::error_code ec;
      if (!fs::exists(sn, ec)) return errBlockRc("file " + s + " not found", "r(601)");
      const std::string tgt = destinationFor(dest, s);
      const fs::path tn = cecon::to_native_path(tgt);
      fs::rename(sn, tn, ec);
      if (ec == std::errc::cross_device_link) {
        // The scratch tree and the user's project routinely sit on different
        // volumes here (the USB scratch relocation), so EXDEV is the common
        // case, not an exotic one: rename cannot cross a mount, copy+remove
        // can, and a do-file's `mv` must survive it.
        ec.clear();
        fs::copy(sn, tn, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::remove_all(sn, ec);
      }
      if (ec) return errBlock("move: " + ec.message());
      touched.push_back(s);
    }
    touched.push_back(dest);
  } else if (verb == "touch") {
    pietVerb = "touch";
    if (operands.empty()) return errBlock("shell touch: filename required", "syntax_error");
    for (const auto& o : operands) {
      for (const auto& p : expandPathArg(session, o)) {
        const fs::path np = cecon::to_native_path(p);
        std::error_code ec;
        if (!fs::exists(np, ec)) {
          std::ofstream f(np, std::ios::binary);
          if (!f) return errBlockRc("file " + p + " could not be opened", "r(603)");
        } else {
          fs::last_write_time(np, fs::file_time_type::clock::now(), ec);
          if (ec) return errBlock("touch: " + ec.message());
        }
        touched.push_back(p);
      }
    }
  } else if (verb == "ls" || verb == "dir") {
    pietVerb = "dir";
    const std::string arg = operands.empty() ? std::string() : operands[0];
    std::vector<std::string> vl;
    if (!arg.empty()) vl.push_back(arg);
    ojson inner = runDir("dir " + arg, vl, {}, session);
    if (inner.is_object() && inner.value("type", "") == "error") return inner;
    if (inner.is_array()) {
      for (const auto& b : inner) extra.push_back(b);
    } else if (!inner.is_null()) {
      extra.push_back(inner);
    }
    if (!arg.empty()) touched.push_back(resolveUserPath(session, arg));
  } else if (verb == "cat" || verb == "type") {
    pietVerb = "type";
    if (operands.empty()) return errBlock("shell cat: filename required", "syntax_error");
    for (const auto& o : operands) {
      std::vector<std::string> vl{o};
      ojson inner = runType("type " + o, vl, {}, session);
      if (inner.is_object() && inner.value("type", "") == "error") return inner;
      if (inner.is_array()) {
        for (const auto& b : inner) extra.push_back(b);
      } else if (!inner.is_null()) {
        extra.push_back(inner);
      }
      touched.push_back(resolveUserPath(session, o));
    }
  } else {
    return refuseVerb();
  }

  std::vector<std::string> shown;
  shown.reserve(touched.size());
  for (const auto& p : touched) shown.push_back(noteRelative(session, p));
  std::string note = "(shell " + line + ": translated -> " + pietVerb;
  if (!shown.empty()) note += " " + joinSpace(shown);
  note += ")";

  // ONE display block, never an array of them. The dispatcher's shell arm
  // pushes this return value as a SINGLE element (the house `blocks.push_back(
  // ojson(runShell(...)))` shape), so an array would land where a block object
  // belongs: the renderer reads no "type" off it and drops the whole thing --
  // silently losing the very announcement this translation exists to make.
  std::string text = note;
  for (const auto& b : extra) {
    const std::string t = b.value("text", std::string());
    if (t.empty()) continue;
    text += "\n" + t;
  }
  return displayBlock(text);
}


}  // namespace piet::handlers
