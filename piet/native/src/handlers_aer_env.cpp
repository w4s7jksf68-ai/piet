// AER-SCOPE1 bundle D (ENV, 2026-09-03) -- the environment verbs the ninety-
// paper AER corpus opens with, split out of handlers_m3.cpp so that file's
// diff stays about data verbs (same reason handlers_data_verbs*.cpp and
// handlers_frames.cpp were split out before it).
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
#include "piet/handlers.h"

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

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// BUG-233 / FIX:OPTNET-ABBREV-SWEEP: a net that does not register its
// vocabulary here is a net the dispatcher's single resolution point cannot
// canonicalize for, which is precisely how an ACCEPTED abbreviation reaches a
// handler that then compares raw keys and drops it. Registered up front, read
// back by the reject calls below, so each spelling is still written once.
const eval::OptVocab& kVocabType =
    eval::registerOptVocab({"type"}, {{"showtabs", "starbang", "lines"}, {}});
// Both spellings register, because both are registry commands routed to
// runDir: a vocabulary filed under "dir" alone leaves `ls x, wid' with no
// canonicalization at the dispatcher's single resolution point, so the
// abbreviation is ACCEPTED by rejectUnknownOptions and then dropped by this
// handler's `k == "wide"' compare -- the exact BUG-233 shape.
const eval::OptVocab& kVocabDir = eval::registerOptVocab({"dir", "ls"}, {{"wide"}, {}});
const eval::OptVocab& kVocabFindfile = eval::registerOptVocab({"findfile"}, {{"path", "all"}, {}});

std::string trim(const std::string& s) { return strutil::trim(s); }
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson errBlock(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// The exact-rc shape eval::coarseRc honours (BUG-187): a call site that KNOWS
// the Stata code says so in `reason` as "r(NNN)", and `capture` hands that
// number back instead of the generic 1 the message-sniffing fallback yields.
ojson errBlockRc(const std::string& message, const std::string& rc) {
  ojson b = errBlock(message);
  b["reason"] = rc;
  return b;
}

std::string dequote(const std::string& sIn) {
  std::string s = trim(sIn);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::string sessionCwd(const SessionState& session) {
  namespace fs = std::filesystem;
  if (session.cwd.has_value() && !session.cwd->empty()) return *session.cwd;
  std::error_code ec;
  fs::path p = fs::current_path(ec);
  return ec ? std::string(".") : p.u8string();
}

// The single argument of a one-argument file command, taken off the RAW text
// when the tokenized varlist did not survive (quoted paths with spaces). The
// comma cut is deliberate and its own limit: an option list starts there, and a
// filename containing a comma is not a shape this corpus produces -- the
// varlist path, which is tried first by every caller, handles it if it ever is.
std::string argAfterCommand(const std::string& raw) {
  std::string s = trim(raw);
  const size_t sp = s.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  s = trim(s.substr(sp + 1));
  const size_t comma = s.find(',');
  if (comma != std::string::npos) s = trim(s.substr(0, comma));
  return dequote(s);
}

// The first whitespace-delimited word of a string. Used only where a
// subcommand is being recovered from raw text: taking the whole tail would
// make `net install foo`'s sub read as "install foo" and print it back.
std::string firstWord(const std::string& s) {
  const std::string t = trim(s);
  const size_t sp = t.find_first_of(" \t");
  return sp == std::string::npos ? t : t.substr(0, sp);
}

// Whitespace tokenizer that keeps a double-quoted run together. `quoted`
// records that the token carried quotes, which is what stops `"-rf"` (a
// deliberately quoted FILENAME) from being read as a flag two functions down.
struct ShellTok {
  std::string text;
  bool quoted = false;
};

std::vector<ShellTok> tokenizeShellArgs(const std::string& s) {
  std::vector<ShellTok> out;
  ShellTok cur;
  bool inTok = false;
  bool inQuote = false;
  for (const char c : s) {
    if (c == '"') {
      inQuote = !inQuote;
      cur.quoted = true;
      inTok = true;
      continue;
    }
    if (!inQuote && (c == ' ' || c == '\t')) {
      if (inTok) {
        out.push_back(cur);
        cur = ShellTok();
        inTok = false;
      }
      continue;
    }
    cur.text.push_back(c);
    inTok = true;
  }
  if (inTok) out.push_back(cur);
  return out;
}

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

// Iterative `*`/`?` matcher (no character classes, no `**`). Linear in the
// worst case that matters here; a recursive one would be shorter but a
// pathological pattern in a do-file would then cost exponential time.
bool globMatch(const std::string& pat, const std::string& name) {
  size_t p = 0;
  size_t n = 0;
  size_t star = std::string::npos;
  size_t mark = 0;
  while (n < name.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
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
void seedSysdir(SessionState& session) {
  if (!session.sysdir.empty()) return;
  const std::string bin = binaryDir();
  session.sysdir["STATA"] = bin;
  session.sysdir["BASE"] = bin;
  session.sysdir["SITE"] = bin;
  session.sysdir["PLUS"] = "~/ado/plus/";
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

// `error #`'s message table. Deliberately a fixed table and not a lookup into
// some shared registry: these are Stata's OWN canned strings for the codes a
// do-file raises by hand, and the empty entries (498/499, and anything absent)
// are Stata's real behaviour -- `error 499` prints nothing and sets _rc, which
// is why the rc travels in `reason` rather than being inferred from the text.
std::string errorCodeMessage(long long code) {
  switch (code) {
    case 1: return "You pressed Break";
    case 100: return "varlist required";
    case 102: return "too few variables specified";
    case 109: return "type mismatch";
    case 111: return "not found";
    case 119: return "statement out of context";
    case 190: return "request may not be combined with by";
    case 197: return "invalid syntax";
    case 198: return "invalid syntax";
    case 199: return "unrecognized command";
    case 301: return "last estimates not found";
    case 416: return "missing values encountered";
    case 430: return "convergence not achieved";
    case 459: return "something that should be true of your data is not";
    case 601: return "file not found";
    case 602: return "file already exists";
    case 603: return "file could not be opened";
    case 693: return "could not remove/create directory";
    case 2000: return "no observations";
    case 2001: return "insufficient observations";
    default: return "";  // 498/499 included: Stata prints nothing, rc still set
  }
}

bool parseLongLong(const std::string& s, long long& out) {
  const std::string t = trim(s);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') i = 1;
  if (i >= t.size()) return false;
  for (size_t k = i; k < t.size(); ++k) {
    if (!std::isdigit(static_cast<unsigned char>(t[k]))) return false;
  }
  try {
    out = std::stoll(t);
  } catch (const std::exception&) {
    return false;
  }
  return true;
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

// ---------------------------------------------------------------------------
// type FILE [, showtabs starbang lines(#)]
//
// ONE display block, not one per line: the log tee renders blocks, and a
// thousand-block `type` of a log file would be a thousand tee'd records of one
// user action.
nlohmann::ordered_json runType(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session) {
  namespace fs = std::filesystem;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabType.known)) return ojson(*err);

  std::string arg = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);
  if (arg.empty()) return errBlock("type: filename required", "syntax_error");

  bool showtabs = false;
  bool starbang = false;
  long long maxLines = -1;
  for (const auto& [k, v] : opts) {
    if (k == "showtabs" && v.isBool) showtabs = v.boolValue;
    else if (k == "starbang" && v.isBool) starbang = v.boolValue;
    else if (k == "lines") {
      if (!parseLongLong(v.strValue, maxLines) || maxLines < 0) {
        return errBlock("type: lines() requires a non-negative integer", "syntax_error");
      }
    }
  }

  const std::string path = resolveUserPath(session, arg);
  const fs::path np = cecon::to_native_path(path);
  std::error_code ec;
  if (!fs::exists(np, ec) || ec) return errBlockRc("file " + path + " not found", "r(601)");

  std::ifstream in(np, std::ios::binary);
  if (!in) return errBlockRc("file " + path + " could not be opened", "r(603)");

  // 4 MiB + 1: reading one byte past the cap is what distinguishes "the file
  // ended" from "we stopped", without a second stat that could race the read.
  constexpr std::size_t kCap = 4u * 1024u * 1024u;
  std::string data(kCap + 1, '\0');
  in.read(&data[0], static_cast<std::streamsize>(kCap + 1));
  const std::size_t got = static_cast<std::size_t>(in.gcount());
  data.resize(got);
  const bool truncated = got > kCap;
  if (truncated) data.resize(kCap);

  // Binary sniff BEFORE any line work: dumping a .dta into the console is the
  // failure mode this guard exists for -- it emits control bytes that can
  // reconfigure the terminal, and the do-file learns nothing from it either way.
  const std::size_t sniff = data.size() < 1024 ? data.size() : 1024;
  const std::size_t nul = data.find('\0');
  if (nul != std::string::npos && nul < sniff) {
    std::error_code sec;
    const auto bytes = fs::file_size(np, sec);
    return displayBlock("(type: binary file -- " +
                        (sec ? std::string("?") : std::to_string(bytes)) + " bytes)");
  }

  std::vector<std::string> lines;
  {
    std::string cur;
    for (const char c : data) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else if (c != '\r') {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) lines.push_back(cur);  // a file ending in \n has no extra blank line
  }

  // starbang first, lines(#) second: `lines(20)` on a starbang'd file means
  // "twenty of the lines you are SHOWING me", which is the only reading under
  // which combining the two options is useful at all.
  if (starbang) {
    std::vector<std::string> kept;
    for (const auto& l : lines) {
      if (l.rfind("*!", 0) == 0) kept.push_back(l);
    }
    lines.swap(kept);
  }
  if (maxLines >= 0 && static_cast<std::size_t>(maxLines) < lines.size()) {
    lines.resize(static_cast<std::size_t>(maxLines));
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    if (!showtabs) {
      text += lines[i];
      continue;
    }
    for (const char c : lines[i]) {
      if (c == '\t') text += "<T>";
      else text.push_back(c);
    }
  }
  if (truncated) {
    if (!text.empty()) text += "\n";
    text += "(type: output truncated at 4 MiB)";
  }
  return displayBlock(text);
}

// ---------------------------------------------------------------------------
// dir [PATH|GLOB] [, wide]
nlohmann::ordered_json runDir(const std::string& raw, const std::vector<std::string>& varlist,
                              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              SessionState& session) {
  namespace fs = std::filesystem;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabDir.known)) return ojson(*err);

  bool wide = false;
  for (const auto& [k, v] : opts) {
    if (k == "wide" && v.isBool) wide = v.boolValue;
  }

  std::string arg = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);

  std::string dirPath;
  std::string pattern = "*";
  if (arg.empty()) {
    dirPath = sessionCwd(session);
  } else if (arg.find('*') != std::string::npos || arg.find('?') != std::string::npos) {
    const fs::path p = cecon::to_native_path(resolveUserPath(session, arg));
    dirPath = p.parent_path().u8string();
    pattern = p.filename().u8string();
    if (dirPath.empty()) dirPath = sessionCwd(session);
  } else {
    const std::string resolved = resolveUserPath(session, arg);
    const fs::path np = cecon::to_native_path(resolved);
    std::error_code ec;
    if (fs::is_directory(np, ec) && !ec) {
      dirPath = resolved;
    } else if (fs::exists(np, ec) && !ec) {
      // `dir report.tex` names one file. Listing its PARENT instead would be a
      // different, much larger answer to the question that was asked.
      dirPath = np.parent_path().u8string();
      pattern = np.filename().u8string();
    } else {
      return errBlockRc("directory " + resolved + " not found", "r(601)");
    }
  }

  std::error_code ec;
  const fs::path base = cecon::to_native_path(dirPath);
  if (!fs::is_directory(base, ec) || ec) {
    return errBlockRc("directory " + dirPath + " not found", "r(601)");
  }
  std::vector<std::pair<std::string, bool>> entries;  // name, isDir
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string nm = it->path().filename().u8string();
    if (!globMatch(pattern, nm)) continue;
    std::error_code dec;
    entries.emplace_back(nm, fs::is_directory(it->path(), dec) && !dec);
  }
  std::sort(entries.begin(), entries.end(),
            [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
              return a.first < b.first;
            });

  std::string text;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (i) text += "\n";
    text += entries[i].first;
    if (entries[i].second) text += "/";
  }

  if (!wide) return displayBlock(text);
  // `wide` is accepted so a corpus do-file does not halt on it, but accepting
  // it silently would make PIET's answer look like Stata's multi-column one.
  // The note says which layout the caller actually got -- appended to the SAME
  // block, because the dispatcher's dir/ls arm pushes this return value as one
  // element and a two-block array would arrive nested and render as nothing.
  if (!text.empty()) text += "\n";
  text += "(dir: option wide ignored -- one entry per line)";
  return displayBlock(text);
}

// ---------------------------------------------------------------------------
// net / ado -- accept-notes. Never refuse: these sit in the PREAMBLE of the
// packages that use them, so a refusal costs the whole do-file, while an
// honest note costs one line and leaves the analysis to run against whatever
// is already on disk.
nlohmann::ordered_json runNet(const std::string& raw, const std::vector<std::string>& varlist) {
  const std::string sub =
      varlist.empty() ? lower(firstWord(argAfterCommand(raw))) : lower(varlist[0]);
  if (sub == "install") {
    const std::string name = varlist.size() > 1 ? dequote(varlist[1]) : "";
    return displayBlock("(net install " + name + ": PIET is offline -- skipped)");
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
// PIET runs no ado-files, so this can never change what EXECUTES. What it can
// do is record where the package believed its own files live, which is the one
// thing a later `findfile` actually reads back -- an accept-and-remember note
// instead of an accept-and-forget one.
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
  // golden that pins it (`adopath + "ado"` prints /Volumes/.../ado on one
  // machine and /home/... on the next), and the resolved form is not lost -- a
  // bare `adopath` prints exactly what was recorded.
  return displayBlock("(adopath " + op + " " + pathArg +
                      ": recorded for findfile -- PIET runs no ado-files)");
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
                      ": recorded for findfile -- PIET runs no ado-files)");
}

// ---------------------------------------------------------------------------
// findfile FNAME [, path(dirlist) all]
nlohmann::ordered_json runFindfile(const std::string& raw, const std::vector<std::string>& varlist,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session) {
  namespace fs = std::filesystem;
  if (auto err = eval::rejectUnknownOptions(opts, kVocabFindfile.known)) return ojson(*err);

  const std::string name = varlist.empty() ? argAfterCommand(raw) : dequote(varlist[0]);
  if (name.empty()) return errBlock("findfile: filename required", "syntax_error");

  bool all = false;
  std::string pathOpt;
  for (const auto& [k, v] : opts) {
    if (k == "all" && v.isBool) all = v.boolValue;
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

  std::vector<std::string> hits;
  for (const auto& entry : searchList) {
    // "." is resolved HERE, not when it was recorded: cwd moves, and an entry
    // frozen at record time would search a directory the do-file has left.
    const std::string dir = (entry == ".") ? sessionCwd(session) : resolveUserPath(session, entry);
    const fs::path cand = cecon::to_native_path(dir) / cecon::to_native_path(name);
    std::error_code ec;
    if (fs::is_regular_file(cand, ec) && !ec) {
      hits.push_back(cand.u8string());
      if (!all) break;
    }
  }

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

// ---------------------------------------------------------------------------
// sleep #
//
// No upper bound on purpose: a corpus do-file that sleeps is waiting on
// something the author knew about, and a cap would turn a long wait into a
// silently different program rather than a slow one.
nlohmann::ordered_json runSleep(const std::vector<std::string>& varlist) {
  long long ms = 0;
  if (varlist.empty() || !parseLongLong(varlist[0], ms) || ms < 0) {
    return errBlock("sleep: milliseconds must be a non-negative integer", "syntax_error");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return ojson::array();  // Stata: silent
}

// ---------------------------------------------------------------------------
// error #
//
// The rc travels in `reason` rather than being inferred from the message,
// because half this table's entries share wording with each other (197 and 198
// are both "invalid syntax") and two of them have no wording at all -- so the
// text cannot carry the number and `capture`'s _rc must not be a guess.
nlohmann::ordered_json runErrorCode(const std::vector<std::string>& varlist) {
  long long code = 0;
  if (varlist.empty() || !parseLongLong(varlist[0], code)) {
    return errBlock("error: return code must be a number", "syntax_error");
  }
  if (code == 0) return ojson::array();  // Stata: `error 0' does nothing at all
  ojson b = errBlock(errorCodeMessage(code));
  b["reason"] = "r(" + std::to_string(code) + ")";
  return b;
}

// ---------------------------------------------------------------------------
// frames reset
//
// Routed through runFrameDrop rather than clearing session.frames directly:
// that handler owns the dangling-link cleanup AND the scratch reclaim (BUG-389
// -- a forgotten map entry leaves a file no ledger knows, which is the LEAK
// AUDIT's ORPHAN class). Duplicating the teardown here would regrow it.
nlohmann::ordered_json runFramesReset(SessionState& session) {
  // Names collected FIRST: runFrameDrop erases from session.frames, so
  // iterating the map while dropping would invalidate the iterator mid-loop.
  std::vector<std::string> names;
  for (const auto& [name, path] : session.frames) {
    (void)path;
    if (name == "default") continue;  // the default frame is the session itself
    names.push_back(name);
  }
  ojson lastError = ojson::array();
  for (const auto& name : names) {
    // Keep going after a failure: stopping on the first one would leave the
    // rest of the frames alive AND their scratch files unreclaimed, which is
    // the opposite of what `frames reset` was asked to do.
    ojson r = runFrameDrop(name, session);
    if (r.is_object() && r.value("type", "") == "error") lastError = r;
  }
  return lastError;
}

}  // namespace piet::handlers
