// BUG-343 — Stata's `file` command family, text mode.
//
// WHY IT EXISTS AT ALL. `file open|write|read|close` had no row in the 162-entry
// command registry: a do-file that logged its own progress, or built a report,
// stopped with an honest unsupported-command error. It was found the way gaps
// usually are — a probe fixture authored ahead of its feature assumed the
// surrounding language worked, and it did not.
//
// SCOPE, chosen and named rather than drifted into: TEXT mode only.
//   * `file open <h> using "<path>", {read|write|text|replace|append}`
//   * `file write <h> ("expr"|`"…"'|_n|_newline|_tab|_char(N)) …`
//   * `file read <h> <localname>`   → one line into the local, r(eof) on EOF
//   * `file close <h>` / `file close _all`
// Everything else Stata's `file` can do — `binary`, `seek`, `set`, `sersetfile`,
// `query`, formatted `%fmt` writes — is REFUSED BY NAME. That is the honest
// half of the gate-① contract: what the engine cannot do, the product says
// plainly, and a silently-ignored `seek` would corrupt a file offset without a
// word. Widening this file later is a feature; pretending here would be a defect.
//
// Handle state lives on SessionState::fileHandles for the same reason postfile's
// does: Stata's file I/O is a named-handle protocol whose state outlives the
// statement that opened it.
#include "piet/handlers.h"

#include <cctype>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

namespace {

using ojson = nlohmann::ordered_json;

ojson err(const std::string& msg, const std::string& code = "") {
  return code.empty() ? eval::engineError(msg) : eval::engineError(msg, code);
}

std::string trimc(const std::string& s) { return strutil::trim(s); }

// Strip one layer of Stata quoting: "text" or compound `"text"'.
std::string dequote(const std::string& raw) {
  std::string s = trimc(raw);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

// Split a `file write` payload into its tokens, keeping quoted runs whole. Stata
// separates write items by whitespace but a quoted string may contain spaces, so
// a plain istringstream would shred `"a b"` into two items and emit them with a
// space that was never asked for.
std::vector<std::string> splitWriteItems(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    const size_t start = i;
    if (s.compare(i, 2, "`\"") == 0) {  // compound quote: run to the matching "'
      const size_t end = s.find("\"'", i + 2);
      i = (end == std::string::npos) ? s.size() : end + 2;
    } else if (s[i] == '"') {
      const size_t end = s.find('"', i + 1);
      i = (end == std::string::npos) ? s.size() : end + 1;
    } else {
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    out.push_back(s.substr(start, i - start));
  }
  return out;
}

}  // namespace

ojson runFileCmd(const std::string& rawLine, SessionState& session) {
  std::string rest = trimc(rawLine);
  {
    static const std::regex reKw(R"(^file\s+)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(rest, m, reKw))
      return err("file: internal parse error (keyword missing)");
    rest = trimc(rest.substr(static_cast<size_t>(m.length(0))));
  }
  if (rest.empty())
    return err("file: a subcommand is required (open|write|read|close)", "syntax_error");

  // sub-verb
  size_t sp = rest.find_first_of(" \t");
  const std::string sub = strutil::lower(sp == std::string::npos ? rest : rest.substr(0, sp));
  rest = (sp == std::string::npos) ? std::string() : trimc(rest.substr(sp));

  // Refuse the unimplemented sub-verbs by NAME — never a generic "unknown".
  if (sub == "seek" || sub == "set" || sub == "query" || sub == "sersetfile")
    return err("file " + sub + " is not implemented in this build (text-mode "
               "open/write/read/close only)",
               "unsupported_command");
  if (sub != "open" && sub != "write" && sub != "read" && sub != "close")
    return err("file: unknown subcommand `" + sub + "` (expected open|write|read|close)",
               "syntax_error");

  // ---- close ---------------------------------------------------------------
  if (sub == "close") {
    const std::string h = trimc(rest);
    if (h.empty()) return err("file close: a handle name is required", "syntax_error");
    if (strutil::lower(h) == "_all") {
      for (auto& kv : session.fileHandles)
        if (kv.second.fp != nullptr) std::fclose(kv.second.fp);
      session.fileHandles.clear();
      return ojson();
    }
    auto it = session.fileHandles.find(h);
    if (it == session.fileHandles.end())
      return err("file close: `" + h + "` is not open", "file_not_open");
    if (it->second.fp != nullptr) std::fclose(it->second.fp);
    session.fileHandles.erase(it);
    return ojson();
  }

  // Every remaining sub-verb starts with a handle name.
  sp = rest.find_first_of(" \t");
  const std::string handle = trimc(sp == std::string::npos ? rest : rest.substr(0, sp));
  rest = (sp == std::string::npos) ? std::string() : trimc(rest.substr(sp));
  if (handle.empty()) return err("file " + sub + ": a handle name is required", "syntax_error");

  // ---- open ----------------------------------------------------------------
  if (sub == "open") {
    if (session.fileHandles.count(handle) > 0)
      return err("file open: handle `" + handle + "` is already open (close it first)",
                 "file_already_open");
    static const std::regex reUsing(R"(^using\s+)", std::regex::icase);
    std::smatch mu;
    if (!std::regex_search(rest, mu, reUsing))
      return err("file open: `using <filename>` is required", "syntax_error");
    rest = trimc(rest.substr(static_cast<size_t>(mu.length(0))));

    std::string opts;
    const size_t comma = rest.find(',');
    if (comma != std::string::npos) {
      opts = strutil::lower(trimc(rest.substr(comma + 1)));
      rest = trimc(rest.substr(0, comma));
    }
    const std::string path = piet::resolveUserPath(session, dequote(rest));
    if (path.empty()) return err("file open: a filename is required", "syntax_error");

    bool wantRead = opts.find("read") != std::string::npos;
    bool wantWrite = opts.find("write") != std::string::npos;
    const bool wantAppend = opts.find("append") != std::string::npos;
    const bool wantReplace = opts.find("replace") != std::string::npos;
    if (opts.find("binary") != std::string::npos)
      return err("file open: `binary` mode is not implemented in this build (text mode only)",
                 "unsupported_option");
    if (wantAppend) wantWrite = true;
    if (!wantRead && !wantWrite) wantRead = true;  // Stata's default is read
    if (wantRead && wantWrite)
      return err("file open: `read` and `write` are mutually exclusive", "syntax_error");

    SessionState::FileHandle fh;
    fh.path = path;
    fh.write = wantWrite;
    fh.append = wantAppend;
    if (wantWrite) {
      // Without `replace` Stata refuses to clobber an existing file — the same
      // no-silent-overwrite rule every other write verb in this codebase honours.
      if (!wantAppend && !wantReplace) {
        if (std::FILE* probe = std::fopen(path.c_str(), "r")) {
          std::fclose(probe);
          return err("file open: `" + path + "` already exists (specify `replace`)",
                     "file_exists");
        }
      }
      fh.fp = std::fopen(path.c_str(), wantAppend ? "a" : "w");
    } else {
      fh.fp = std::fopen(path.c_str(), "r");
    }
    if (fh.fp == nullptr)
      return err("file open: could not open `" + path + "`", "file_open_failed");
    session.fileHandles[handle] = fh;
    return ojson();
  }

  auto it = session.fileHandles.find(handle);
  if (it == session.fileHandles.end())
    return err("file " + sub + ": `" + handle + "` is not open", "file_not_open");
  SessionState::FileHandle& fh = it->second;

  // ---- write ---------------------------------------------------------------
  if (sub == "write") {
    if (!fh.write) return err("file write: `" + handle + "` is open for reading", "file_wrong_mode");
    std::string out;
    for (const std::string& item : splitWriteItems(rest)) {
      const std::string low = strutil::lower(item);
      if (low == "_n" || low == "_newline") {
        out += "\n";
        continue;
      }
      if (low == "_tab") {
        out += "\t";
        continue;
      }
      static const std::regex reChar(R"(^_char\((\d+)\)$)", std::regex::icase);
      std::smatch mc;
      if (std::regex_match(item, mc, reChar)) {
        out += static_cast<char>(std::stoi(mc[1].str()) & 0xFF);
        continue;
      }
      if (!item.empty() && item[0] == '%')
        return err("file write: formatted (`" + item + "`) writes are not implemented in this "
                   "build — write a pre-formatted string instead",
                   "unsupported_option");
      out += dequote(item);
    }
    if (std::fwrite(out.data(), 1, out.size(), fh.fp) != out.size())
      return err("file write: write to `" + fh.path + "` failed", "file_write_failed");
    return ojson();
  }

  // ---- read ----------------------------------------------------------------
  // `file read <h> <localname>`: one LINE into the local, r(eof) set to 0/1.
  if (fh.write) return err("file read: `" + handle + "` is open for writing", "file_wrong_mode");
  const std::string localName = trimc(rest);
  if (localName.empty())
    return err("file read: a local macro name to read into is required", "syntax_error");

  std::string line;
  int c = 0;
  bool any = false;
  while ((c = std::fgetc(fh.fp)) != EOF) {
    any = true;
    if (c == '\n') break;
    if (c != '\r') line.push_back(static_cast<char>(c));
  }
  fh.eof = !any;
  session.eval.locals.set(localName, fh.eof ? std::string() : line);
  // r(eof) is what a Stata read loop tests. Setting it through the same r-store
  // every r-class command uses is the whole wiring — BUG-344's lesson was that a
  // value computed but never published is a value that does not exist.
  piet::eval::OrderedFloatDict r;
  r.set("eof", fh.eof ? 1.0 : 0.0);
  session.eval.rValues = r;
  return ojson();
}

}  // namespace piet::handlers
