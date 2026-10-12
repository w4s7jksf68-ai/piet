#include "piet/eval/tokenizer.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "registry.h"

namespace piet::eval {

namespace {

// ---------------------------------------------------------------------------
// String helpers (Python str.strip / lstrip / rstrip equivalents — Python's
// str.strip() strips all whitespace, which for our ASCII Stata-subset input
// is space/tab/newline/carriage-return/form-feed/vertical-tab).

bool isPySpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string lstrip(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && isPySpace(s[i])) ++i;
  return s.substr(i);
}

std::string rstrip(const std::string& s) {
  size_t j = s.size();
  while (j > 0 && isPySpace(s[j - 1])) --j;
  return s.substr(0, j);
}

std::string strip(const std::string& s) { return lstrip(rstrip(s)); }

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many toLower(...) call sites
// didn't need touching. Pure dedup, no behavior change. NOTE: NOT the same
// consolidation as strip()/lstrip()/rstrip() just above, which deliberately
// implement Python str.strip()'s wider whitespace set and are left as-is.
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

// Python str.rstrip(":") — strip trailing ':' characters only (used for the
// `quietly:` label-suffix guard).
std::string rstripChar(std::string s, char ch) {
  size_t j = s.size();
  while (j > 0 && s[j - 1] == ch) --j;
  return s.substr(0, j);
}

// ---------------------------------------------------------------------------
// Patterns for macro / control-flow detection (step 1 — runs first, raw line)

// BUG-352 helper: replace every compound-quoted span `"..."' with spaces of the
// SAME length, so a quote-blind pre-filter cannot see a macro that is really
// string content. Nesting follows scanOneToken's Case 1 rule (a nested `" opens
// a level, a "' closes one); an unterminated span masks to end of line, which is
// the conservative direction — the tripwire stays silent rather than firing on
// half a string.
std::string maskCompoundQuotes(const std::string& s) {
  std::string out = s;
  size_t i = 0;
  const size_t n = s.size();
  while (i + 1 < n) {
    if (s[i] == '`' && s[i + 1] == '"') {
      int depth = 1;
      size_t j = i + 2;
      while (j < n && depth > 0) {
        if (j + 1 < n && s[j] == '`' && s[j + 1] == '"') { depth += 1; j += 2; continue; }
        if (j + 1 < n && s[j] == '"' && s[j + 1] == '\'') { depth -= 1; j += 2; continue; }
        ++j;
      }
      for (size_t k = i; k < j && k < n; ++k) out[k] = ' ';
      i = j;
      continue;
    }
    ++i;
  }
  return out;
}

const std::regex& reLocalMacro() {
  static const std::regex r(R"(`[^']*')");
  return r;
}
const std::regex& reGlobalMacro() {
  // $name or ${name}
  static const std::regex r(R"(\$\{?[A-Za-z_][A-Za-z0-9_]*\}?)");
  return r;
}

const std::vector<std::string>& loopStmts() {
  static const std::vector<std::string> v{"foreach", "forvalues", "while"};
  return v;
}
const std::vector<std::string>& macroStmts() {
  static const std::vector<std::string> v{"local", "global", "tempvar", "macro", "tokenize"};
  return v;
}
// _PROG_STMTS is empty in the Python source (program/end handled upstream).
const std::vector<std::string>& progStmts() {
  static const std::vector<std::string> v{};
  return v;
}
const std::vector<std::string>& ctrlStmts() {
  static const std::vector<std::string> v{"else"};
  return v;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// ---------------------------------------------------------------------------
// Comment stripping (step 2)

// A /* ... */ comment is white space to Stata, newline included, so a command
// whose text continues after a `*/` on a LATER line is one command (BUG-698;
// oracle w2c_ml_getmata.do:324-325 splits a getmata getlist this way, log
// :1225). When a block opened on line A closes on line B, the text after `*/`
// is appended to A's entry with one space and scanned with the same inline
// rules (a further unclosed `/*` keeps accumulating onto A). Lines A+1..B stay
// as empty placeholders so doexec's line numbers keep matching the source. The
// whole-line `*` test applies to a line's own leading text only, never to a
// tail joined after `*/`. A `/*` that never closes still blanks to the end.
std::vector<std::string> stripComments(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  bool inBlock = false;
  size_t openIdx = 0;  // out[] entry of the line where the open block began
  // Inline /* */ and //: the text kept from `line`; sets inBlock when a `/*`
  // does not close on this line (everything after it is dropped).
  auto scanInline = [&inBlock](const std::string& line) {
    std::string result;
    size_t i = 0;
    while (i < line.size()) {
      if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
        auto end = line.find("*/", i + 2);
        if (end == std::string::npos) {
          inBlock = true;
          break;
        }
        i = end + 2;
      } else if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/' &&
                 (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
        break;
      } else {
        result.push_back(line[i]);
        ++i;
      }
    }
    return rstrip(result);
  };
  for (const auto& line : lines) {
    if (inBlock) {
      out.push_back("");  // placeholder: this line's text belongs to line A
      auto end = line.find("*/");
      if (end == std::string::npos) continue;
      inBlock = false;
      const std::string tail = lstrip(scanInline(line.substr(end + 2)));
      if (!tail.empty()) {
        std::string& head = out[openIdx];
        head = head.empty() ? tail : head + " " + tail;
      }
      continue;
    }
    // whole-line * comment
    std::string strippedLead = lstrip(line);
    if (!strippedLead.empty() && strippedLead[0] == '*') {
      out.push_back("");
      continue;
    }
    out.push_back(scanInline(line));
    if (inBlock) openIdx = out.size() - 1;
  }
  return out;
}

std::vector<std::string> joinContinuations(const std::vector<std::string>& lines) {
  std::vector<std::string> joined;
  std::string buf;
  // Python: re.match(r"^(.*?)///.*$", stripped) — lazy .*? then /// then
  // greedy .* to end of (single-line) string. Equivalent to: find the FIRST
  // occurrence of "///" and take everything before it (lazy prefix match).
  for (const auto& line : lines) {
    std::string strippedLine = rstrip(line);
    auto pos = strippedLine.find("///");
    if (pos != std::string::npos) {
      buf += strippedLine.substr(0, pos);
    } else {
      joined.push_back(buf + strippedLine);
      buf.clear();
    }
  }
  if (!buf.empty()) {
    joined.push_back(buf);
  }
  return joined;
}

// ---------------------------------------------------------------------------
// if / in / weight extraction — regexes matched with std::regex (ECMAScript
// grammar, which supports \b, non-greedy, named groups via (?:) works the
// same as Python's re module for this subset).

// NOTE: tokenizer.py also defines _RE_IF (r"\bif\s+(.+?)...$") but never
// uses it — the live if-extraction is the re.split() in _extract_qualifiers.
// The dead pattern is deliberately NOT ported (ROADMAP: don't carry dead
// code into the C++ port unverified).
const std::regex& reIn() {
  static const std::regex r(R"(\bin\s+(\S+)\s*$)");
  return r;
}
const std::regex& reWeight() {
  // Longer alternatives listed before their abbreviations (fweight before fw,
  // pweight before pw, ...) so the full-word spelling is what group 1 captures
  // when both could plausibly start a match. std::regex's ECMAScript grammar
  // backtracks (this isn't a POSIX leftmost-longest engine), so the mandatory
  // literal `=` right after the alternation would force correct disambiguation
  // even with the short forms first — but ordering longest-first is still the
  // correct general rule for alternation (no reliance on backtracking to
  // recover the right capture), and it's what a future engine/flag swap needs
  // to stay correct. project Step-1 D3: previously ONLY fw/pw/iw/aw matched, so
  // `[fweight=w]` silently fell through as an unmatched weight qualifier
  // (garbage varlist bug) — this was a real latent gap, not a style nit.
  static const std::regex r(
      R"(\[(fweight|fw|pweight|pw|iweight|iw|aweight|aw)=([^\]]+)\])");
  return r;
}
// Python: re.split(r'\s+if\s+', pre, maxsplit=1, flags=re.IGNORECASE)
const std::regex& reSplitIf() {
  static const std::regex r(R"(\s+if\s+)", std::regex::icase);
  return r;
}

struct Qualifiers {
  std::string cleanedPre;
  std::optional<std::string> ifExpr;
  std::optional<std::string> inRange;
  std::optional<std::string> weightType;
  std::optional<std::string> weightVar;
};

// EXPRLAYER-6: case-insensitive substring search, for the one pre-filter whose
// pattern carries the icase flag.
bool ciContains(const std::string& hay, const char* needle, size_t n) {
  if (hay.size() < n) return false;
  for (size_t i = 0; i + n <= hay.size(); ++i) {
    size_t k = 0;
    for (; k < n; ++k) {
      char c = hay[i + k];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != needle[k]) break;
    }
    if (k == n) return true;
  }
  return false;
}

Qualifiers extractQualifiers(std::string pre) {
  Qualifiers q;
  // weight first (most syntactically distinct) — searchUnquoted finds the
  // first match outside any quoted string (BUG-115: a qualifier-shaped
  // substring inside a quoted literal, e.g. `display "[fw=1]"`, must stay
  // string content, matching Python's re.search on the unquoted line).
  // EXPRLAYER-6 strict pre-filters — each pattern's own mandatory literal.
  // reWeight needs a '[', reIn the literal "in" (case-sensitive: the pattern
  // carries no icase flag), reSplitIf an "if" in any case. The zone profile put
  // 15.9 us/line in `1: tokenize` on S02's matrix loop, where none of the three
  // can match anything.
  std::smatch m;
  if (pre.find('[') != std::string::npos && searchUnquoted(pre, reWeight(), m)) {
    q.weightType = m[1].str();
    q.weightVar = strip(m[2].str());
    pre = pre.substr(0, m.position(0)) + pre.substr(m.position(0) + m.length(0));
  }

  // in range
  if (pre.find("in") != std::string::npos && searchUnquoted(pre, reIn(), m)) {
    q.inRange = m[1].str();
    pre = rstrip(pre.substr(0, m.position(0)));
  }

  // if expr — Python: re.split(r'\s+if\s+', pre, maxsplit=1, IGNORECASE).
  // Splits on the FIRST whitespace-delimited `if` OUTSIDE a quoted string;
  // the left half becomes the cleaned pre (rstripped), the right half the
  // if-expression (rstripped). BUG-115: `display "if you see this"` must not
  // split on the `if` embedded in the quoted literal.
  if (ciContains(pre, "if", 2) && searchUnquoted(pre, reSplitIf(), m)) {
    std::string head = pre.substr(0, static_cast<size_t>(m.position(0)));
    std::string tail = pre.substr(static_cast<size_t>(m.position(0)) + m.length(0));
    pre = rstrip(head);
    q.ifExpr = rstrip(tail);
  }

  q.cleanedPre = strip(pre);
  return q;
}

// ---------------------------------------------------------------------------
// BUG-105 secondary ②: `by`/`bysort` prefix spellings.
//
// These are the ONE class of command token dispatcher.cpp routes WITHOUT a
// registry row (registry.h's "Deliberately NOT here": by/bysort are a PREFIX
// mechanism, not a standalone command), so a registry miss on them is NOT
// evidence that PIET has never heard of the word. Kept byte-identical to
// dispatcher.cpp:1128's own list — selftest's testBUG105InQualPrescan loops all
// five and fails the moment the two drift apart.
bool isByPrefixToken(const std::string& lowerCmd) {
  return lowerCmd == "bysort" || lowerCmd == "by" || lowerCmd == "bys" || lowerCmd == "byso" ||
         lowerCmd == "bysor";
}

// BUG-105 secondary ②: "does PIET route this command token AT ALL?"
//
// Deliberately PERMISSIVE — any doubt answers TRUE. The only callers are the
// weight/`in` qualifier gates below, which use a FALSE to replace their
// feature-gap wording with "command not found", and that claim must never be
// made about a word PIET actually implements. Measured basis for trusting the
// registry here (2026-07-31, this fix): every `cmd == "..."` routing arm in
// dispatcher.cpp resolves through registry::findByName EXCEPT the five
// by-prefix spellings above, and the command words intercepted BEFORE
// tokenize() (tempfile/tempname/program/do/run/gettoken) never reach these
// gates at all — verified by probe, not by reading.
bool isRoutableCommandToken(const std::string& cmdRaw) {
  const std::string c = rstripChar(toLower(cmdRaw), ':');
  if (c.empty()) return true;
  if (registry::findByName(c) != nullptr) return true;
  return isByPrefixToken(c);
}

}  // namespace

// ---------------------------------------------------------------------------
// Option parsing (post-comma portion). Declared in tokenizer.h (moved out of
// the anonymous namespace above) so other modules that parse a comma-
// delimited option tail — e.g. twoway.cpp's per-layer interior — can reuse
// this SAME quote/paren-aware grammar. Still calls the file-local
// isPySpace/strip/toLower helpers above (same-TU internal linkage is fine).
std::vector<std::pair<std::string, OptValue>> parseOpts(const std::string& optStr) {
  std::vector<std::pair<std::string, OptValue>> opts;
  std::string s = strip(optStr);
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    while (i < n && isPySpace(s[i])) ++i;
    if (i >= n) break;
    size_t start = i;
    while (i < n && !isPySpace(s[i]) && s[i] != '(') ++i;
    std::string key = s.substr(start, i - start);
    // .lower().strip(",").strip()
    key = toLower(key);
    // strip(",") — Python strips ANY leading/trailing chars in the set {","}.
    {
      size_t a = 0, b = key.size();
      while (a < b && key[a] == ',') ++a;
      while (b > a && key[b - 1] == ',') --b;
      key = key.substr(a, b - a);
    }
    key = strip(key);
    if (key.empty()) {
      ++i;
      continue;
    }
    if (i < n && s[i] == '(') {
      int depth = 1;
      ++i;
      size_t vstart = i;
      while (i < n && depth > 0) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        ++i;
      }
      std::string val = strip(s.substr(vstart, (i - 1) - vstart));
      opts.emplace_back(key, OptValue::ofString(val));
    } else {
      opts.emplace_back(key, OptValue::ofBool(true));
    }
  }
  return opts;
}

// ---------------------------------------------------------------------------
// Public API

nlohmann::json ParsedCommand::toJson() const {
  nlohmann::json j;
  j["cmd"] = cmd;
  j["varlist"] = varlist;
  j["if_expr"] = ifExpr ? nlohmann::json(*ifExpr) : nlohmann::json(nullptr);
  j["in_range"] = inRange ? nlohmann::json(*inRange) : nlohmann::json(nullptr);
  j["weight_type"] = weightType ? nlohmann::json(*weightType) : nlohmann::json(nullptr);
  j["weight_var"] = weightVar ? nlohmann::json(*weightVar) : nlohmann::json(nullptr);
  nlohmann::json optsJ = nlohmann::json::object();
  for (const auto& [k, v] : opts) {
    optsJ[k] = v.isBool ? nlohmann::json(v.boolValue) : nlohmann::json(v.strValue);
  }
  j["opts"] = optsJ;
  j["raw"] = raw;
  return j;
}

std::pair<std::string, std::string> scanOneToken(const std::string& s, bool bind) {
  std::string src = lstrip(s);
  if (src.empty()) return {"", ""};

  size_t i = 0;
  size_t n = src.size();

  // Case 1: compound-quoted string `"..."'
  if (src.size() >= 2 && src[0] == '`' && src[1] == '"') {
    i = 2;
    int depth = 1;
    std::string tok;
    while (i < n && depth > 0) {
      if (i + 1 < n && src[i] == '`' && src[i + 1] == '"') {
        depth += 1;
        tok.push_back(src[i]);
        tok.push_back(src[i + 1]);
        i += 2;
        continue;
      }
      if (i + 1 < n && src[i] == '"' && src[i + 1] == '\'') {
        depth -= 1;
        if (depth > 0) {
          tok.push_back(src[i]);
          tok.push_back(src[i + 1]);
        }
        i += 2;
        continue;
      }
      tok.push_back(src[i]);
      ++i;
    }
    std::string result = bind ? tok : ("`\"" + tok + "\"'");
    std::string remainder = lstrip(src.substr(std::min(i, n)));
    return {result, remainder};
  }

  // Case 2: plain double-quoted string "..."
  if (src[0] == '"') {
    i = 1;
    std::string tok;
    while (i < n) {
      if (src[i] == '"') {
        ++i;
        break;
      }
      tok.push_back(src[i]);
      ++i;
    }
    std::string remainder = lstrip(src.substr(std::min(i, n)));
    return {tok, remainder};
  }

  // Case 3: bare (unquoted) token — ends at whitespace
  std::string tok;
  while (i < n && !isPySpace(src[i])) {
    tok.push_back(src[i]);
    ++i;
  }
  std::string remainder = lstrip(src.substr(std::min(i, n)));
  return {tok, remainder};
}

long long findUnquotedComma(const std::string& s) {
  int depth = 0;
  bool inQuote = false;
  int compound = 0;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    char ch = s[i];
    if (compound > 0) {
      if (i + 1 < n && s[i] == '"' && s[i + 1] == '\'') {
        compound -= 1;
        i += 2;
        continue;
      }
      if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
        compound += 1;
        i += 2;
        continue;
      }
    } else if (inQuote) {
      if (ch == '"') inQuote = false;
    } else if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
      compound += 1;
      i += 2;
      continue;
    } else if (ch == '"') {
      inQuote = true;
    } else if (ch == '(' || ch == '[') {
      depth += 1;
    } else if (ch == ')' || ch == ']') {
      depth -= 1;
    } else if (ch == ',' && depth == 0 && !inQuote && compound == 0) {
      return static_cast<long long>(i);
    }
    ++i;
  }
  return -1;
}

// Mirrors findUnquotedComma's quote/compound-quote state machine exactly
// (kept as a separate traversal, same shape, per this file's established
// "reuse the algorithm, not the loop" convention — see twoway_parse.cpp's
// scanToplevelParens) so the two never drift apart. Every character
// belonging to a quoted span — including its delimiters — is marked true.
std::vector<bool> quotedSpanMask(const std::string& s) {
  std::vector<bool> mask(s.size(), false);
  bool inQuote = false;
  int compound = 0;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    char ch = s[i];
    if (compound > 0) {
      mask[i] = true;
      if (i + 1 < n && s[i] == '"' && s[i + 1] == '\'') {
        mask[i + 1] = true;
        compound -= 1;
        i += 2;
        continue;
      }
      if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
        mask[i + 1] = true;
        compound += 1;
        i += 2;
        continue;
      }
    } else if (inQuote) {
      mask[i] = true;
      if (ch == '"') inQuote = false;
    } else if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
      mask[i] = true;
      mask[i + 1] = true;
      compound += 1;
      i += 2;
      continue;
    } else if (ch == '"') {
      mask[i] = true;
      inQuote = true;
    }
    ++i;
  }
  return mask;
}

// BUG-115: if/in/weight qualifier scanning ran quote-blind — a double-quoted
// literal whose text happened to contain " in "/" if "/a `[fw=...]`-shaped
// substring was mis-split off as a real qualifier (e.g. `display "in
// failprog"` was rejected as an unsupported `in` row-range qualifier instead
// of printing the string). std::regex has no built-in way to exclude a
// span, so this walks every candidate match left-to-right (regex_search's
// own first-match order) and returns the first one that doesn't overlap a
// quoted span — behaviourally identical to regex_search on any line with no
// quoted qualifier-shaped text, since then every match is already unquoted.
bool searchUnquoted(const std::string& s, const std::regex& re, std::smatch& out) {
  // EXPRLAYER-6: the quoted-span mask is a std::vector<bool> ALLOCATION plus a
  // full scan, and it was built before the regex had said whether there was
  // anything to mask. Three qualifier patterns run per dispatched line, so
  // every line paid three allocations to answer "no match". Built lazily now —
  // the loop below is unreachable when the iterator is already at the end, so
  // the outcome cannot change.
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  auto end = std::sregex_iterator();
  if (begin == end) return false;
  std::vector<bool> mask = quotedSpanMask(s);
  for (auto it = begin; it != end; ++it) {
    size_t pos = static_cast<size_t>(it->position(0));
    size_t len = static_cast<size_t>(it->length(0));
    bool touchesQuote = false;
    for (size_t k = pos; k < pos + len && k < mask.size(); ++k) {
      if (mask[k]) {
        touchesQuote = true;
        break;
      }
    }
    if (!touchesQuote) {
      out = *it;
      return true;
    }
  }
  return false;
}

namespace {

// B4/W1: Stata accepts ONE `if`/`in` qualifier AFTER a command's options.
// Measured on StataBE 19.5 (private oracle record, oracle
// records `summarize|trailing_if` and `summarize|trailing_in`):
//
//   summarize price, detail, if mpg<30   -> rc 0, N=67
//   summarize price, detail, in 1/10     -> rc 0, N=10
//
// and it really restricts the computation rather than being swallowed —
// `xtile q4 = price, nq(4), if foreign==1` leaves 22 of 74 rows non-missing
// (oracle `xtile|nq()_trailing_if`).
//
// Returns the index, within `post` (the text AFTER the command's first
// top-level comma), of the top-level comma that opens a trailing qualifier,
// or -1 if there is none.
//
// The walk is done with findUnquotedComma — the scanner this file already
// uses for the command's own first comma, which understands plain quotes,
// Stata compound quotes and paren/bracket nesting — one top-level comma at a
// time. A naive find(", if ") would split inside a string literal, which is
// the seeded-RED arm in selftest (`gen z = "a, if b"`).
//
// `if(` / `in(` are NOT qualifiers, so the keyword must be followed by
// WHITESPACE and then something: `nl (...) , in(b 2)` is nl's initial()
// option at its minimum abbreviation, and a keyword-only test eats it (this
// exact bug was found in the census tool during this campaign).
//
// Lowercase only. Stata's qualifier keywords are case-sensitive, and any
// other spelling is left to fall through to the ordinary option net, where it
// is honestly reported as an unknown option rather than silently accepted.
long long findTrailingQualifierComma(const std::string& post) {
  size_t base = 0;
  while (base < post.size()) {
    long long rel = findUnquotedComma(post.substr(base));
    if (rel < 0) return -1;
    const size_t at = base + static_cast<size_t>(rel);
    size_t j = at + 1;
    while (j < post.size() && isPySpace(post[j])) ++j;
    // keyword = exactly "if" or "in", then whitespace, then a non-blank tail.
    if (j + 2 < post.size() && post[j] == 'i' && (post[j + 1] == 'f' || post[j + 1] == 'n') &&
        isPySpace(post[j + 2]) && !strip(post.substr(j + 2)).empty()) {
      return static_cast<long long>(at);
    }
    base = at + 1;
  }
  return -1;
}

}  // namespace

ParsedCommand tokenize(const std::string& line) {
  std::string raw = strip(line);
  if (raw.empty()) {
    throw TokenizerError(errorBlock("syntax_error", "empty command"));
  }

  // Step 1a: #delimit (preprocessor) — reject before anything.
  auto tokens0 = splitWs(raw);
  std::string firstToken = tokens0.empty() ? "" : toLower(tokens0[0]);
  if (!firstToken.empty() && firstToken[0] == '#') {
    // Both branches produce the same result in the Python source (both call
    // unsupported_delimiter(first_token)).
    throw TokenizerError(unsupportedDelimiter(firstToken));
  }

  // Step 1b: Macro refs in the raw line (before stripping).
  // EXPRLAYER-6: `` `...' `` needs a backtick and `$NAME` needs a dollar —
  // the same strict pre-filter idiom macros.cpp already applies to the very
  // same two constructs.
  // BUG-352: the pre-filter must not read INSIDE a compound-quoted string.
  // Stata's `"..."' is how a string carries a double quote, and `[^']*' matches
  // it from the opening backtick to the first apostrophe — so
  // `gen s = `"has "inner" quotes"'` was rejected as a macro reference and the
  // do-file STOPPED, on a construct StataBE 19.5 answers rc 0 for. Mask those
  // spans (length-preserving, so any match position still refers to `raw`) and
  // run the two pre-filters on the masked copy. A real macro inside a compound
  // quote is not lost by this: the dispatcher expands macros BEFORE tokenize, so
  // anything still holding a backtick here is by definition unexpanded.
  const std::string scan = maskCompoundQuotes(raw);
  // BUG-352, second half: masking stops the WRONG diagnosis, but the construct is
  // genuinely unimplemented — the expression layer and the display path each
  // reject it further in, under messages that name neither the construct nor the
  // limit ("generate: expression evaluation failed at obs 1"). Doctrine I: what
  // the product cannot do it says plainly. So reject it HERE, by name, at the
  // earliest point, instead of failing deeper under a misleading label.
  if (scan != raw) {
    throw TokenizerError(errorBlock(
        "unsupported_syntax",
        "Stata's compound double quotes (`\"...\"') are not implemented in this version of PIET. "
        "Use a plain \"...\" string; a string that must itself contain a double quote has no "
        "substitute here yet.",
        "`\"", std::nullopt, std::nullopt, "piet_gap"));
  }
  std::smatch m;
  if (scan.find('`') != std::string::npos && std::regex_search(scan, m, reLocalMacro())) {
    throw TokenizerError(unsupportedMacro(m.str(0)));
  }
  if (scan.find('$') != std::string::npos && std::regex_search(scan, m, reGlobalMacro())) {
    throw TokenizerError(unsupportedMacro(m.str(0)));
  }

  // Step 1c: Macro/loop/program statement keywords.
  std::string cmdLower = rstripChar(firstToken, ':');
  if (contains(loopStmts(), cmdLower)) {
    throw TokenizerError(unsupportedLoop(cmdLower));
  }
  if (contains(macroStmts(), cmdLower)) {
    throw TokenizerError(unsupportedMacro(cmdLower));
  }
  if (contains(progStmts(), cmdLower)) {
    throw TokenizerError(unsupportedProgram(cmdLower));
  }
  if (contains(ctrlStmts(), cmdLower)) {
    throw TokenizerError(unsupportedLoop(cmdLower));
  }

  // Step 2: strip inline * comment / // comment.
  if (!raw.empty() && raw[0] == '*') {
    throw TokenizerError(errorBlock("syntax_error", "comment line"));
  }
  // Strip trailing // comment: re.sub(r'\s+//.*$', '', raw)
  std::string processed;
  {
    // EXPRLAYER-6: the pattern needs a literal "//".
    static const std::regex reTrailingSlashes(R"(\s+//.*$)");
    processed = raw.find("//") != std::string::npos
                    ? std::regex_replace(raw, reTrailingSlashes, "")
                    : raw;
  }

  // Step 3: check for leftover /// (guard).
  if (processed.find("///") != std::string::npos) {
    auto pos = processed.find("///");
    processed = rstrip(processed.substr(0, pos));
  }

  // Step 4: find first unparenthesised, unquoted comma.
  long long commaPos = findUnquotedComma(processed);
  std::string preComma, postComma;
  if (commaPos == -1) {
    preComma = processed;
    postComma = "";
  } else {
    preComma = processed.substr(0, static_cast<size_t>(commaPos));
    postComma = processed.substr(static_cast<size_t>(commaPos) + 1);
  }

  // Step 4b (B4/W1): a trailing `, if …` / `, in …` qualifier, i.e. one that
  // comes AFTER the options. Stata consumes EXACTLY ONE of them; see
  // findTrailingQualifierComma above for the measured basis.
  std::string trailingQual;
  bool haveTrailingQual = false;
  if (!postComma.empty()) {
    long long tq = findTrailingQualifierComma(postComma);
    if (tq >= 0) {
      haveTrailingQual = true;
      std::string tail = postComma.substr(static_cast<size_t>(tq) + 1);
      postComma = postComma.substr(0, static_cast<size_t>(tq));
      // "Exactly one" is not a simplification, it is the measured rule, and
      // the two rejects carry DIFFERENT messages:
      //
      //   summarize price, detail, if mpg<30, if mpg>10
      //       -> r(198) "option if not allowed"
      //
      // i.e. the SECOND trailing qualifier is no longer a qualifier at all —
      // it falls through to the ordinary option net and is reported as an
      // option named `if`. So the tail is cut at ITS first top-level comma
      // and everything after that goes back onto the option list, where
      // errors.cpp's rejectUnknownOptions produces exactly that sentence
      // (errors.cpp:371, modulo PIET's house backticks around the name).
      long long more = findUnquotedComma(tail);
      if (more >= 0) {
        std::string extraOpts = tail.substr(static_cast<size_t>(more) + 1);
        tail = tail.substr(0, static_cast<size_t>(more));
        postComma = strip(postComma).empty() ? extraOpts : (postComma + " " + extraOpts);
      }
      trailingQual = tail;
    }
  }

  // Step 5: extract if/in/weight from pre_comma.
  Qualifiers q = extractQualifiers(preComma);

  if (haveTrailingQual) {
    if (q.ifExpr || q.inRange) {
      // The OTHER measured reject, and it is a different one:
      //
      //   summarize price if mpg>15, detail, if mpg<30
      //       -> r(198) "invalid syntax"
      //
      // A leading qualifier plus a trailing one is rejected earlier, as
      // syntax, not as a bad option. An implementation that answers "invalid
      // syntax" for both cases is wrong in the two-trailing one, and one that
      // answers "option if not allowed" for both is wrong here.
      //
      // MEASURED CELLS: leading `if` + trailing `if` (oracle
      // `summarize|leading_if_AND_trailing_if`). The CROSS combinations
      // (leading `if` + trailing `in`, leading `in` + trailing `if`) were not
      // captured, so they are rejected too rather than guessed into
      // acceptance — a visible reject is recoverable, a silently-accepted
      // divergence is not.
      throw TokenizerError(errorBlock("syntax_error", "invalid syntax"));
    }
    // No leading qualifier, so splicing the trailing one onto the pre-comma
    // text and re-running the SAME extractor is exactly equivalent to having
    // typed it in the leading position — which is what Stata's rc-0 answers
    // above show it means. Re-deriving the if/in split here instead would be
    // a second, drifting copy of extractQualifiers.
    q = extractQualifiers(preComma + " " + trailingQual);
  }

  // Parse pre-comma into cmd + varlist.
  auto tokens = splitWs(q.cleanedPre);
  if (tokens.empty()) {
    throw TokenizerError(errorBlock("syntax_error", "empty command after stripping"));
  }
  std::string cmd = tokens[0];
  std::vector<std::string> varlist(tokens.begin() + 1, tokens.end());

  // Step 6: parse options from post_comma.
  std::vector<std::pair<std::string, OptValue>> opts;
  if (!strip(postComma).empty()) {
    opts = parseOpts(postComma);
  }

  // Guard: estimation weights are silently dropped by the engine — EXCEPT
  // for whichever weight TYPES a command's own registry row declares
  // (registry.cpp's WeightPolicy field, reached via registry::findByName()).
  //
  // P3 weights wave (project-owned engine work taught cobra real weighted
  // estimation on a wider surface): this used to be a single blanket rule
  // ("fw/fweight only, on a narrow 7-command allow-list; aw/pw/iw rejected
  // for EVERY command, zero exceptions") — that law is retired. The allow-
  // set is now genuinely per-command: None (most commands, unchanged),
  // FweightOnly (tabulate/tab1/tab2, collapse — unchanged), AweightFweight
  // (summarize/sum/su — gained [aw=]/[aweight=]), PweightOnly (the new
  // mean/proportion rows — probability weights only, matching real Stata),
  // AllWeights (regress/reg/r/feols/reghdfe — all four types forwarded;
  // cobra itself honest-rejects a weight combined with --fe, since weighted
  // HDFE absorption isn't implemented — that reject comes back from cobra,
  // not a check here). See WeightPolicy's own doc comment (registry.h) for
  // the authoritative list.
  //
  // A3 (BETA campaign, BS-096): the allow-list itself lives ONCE, in the
  // registry, so the reject message can NAME the command (fixes audit
  // g#15/SCOPE.md BS-105's "command-blind" complaint: today's message was
  // identical whether you typed `scatter … [aw=]` or `regress … [pw=]`),
  // and distinguish "this command has zero weight support" from "this
  // command supports a narrower set than the weight type you gave it".
  if (q.weightType) {
    std::string cmdLowerForWeight = toLower(cmd);
    std::string wt = toLower(*q.weightType);
    bool isAweight = (wt == "aw" || wt == "aweight");
    bool isFweight = (wt == "fw" || wt == "fweight");
    bool isPweight = (wt == "pw" || wt == "pweight");
    bool isIweight = (wt == "iw" || wt == "iweight");
    const registry::CommandSpec* weightSpec = registry::findByName(cmdLowerForWeight);
    // A registry miss (unroutable word, or one of the by/bysort spellings
    // deliberately kept out of the registry — registry.h's "Deliberately
    // NOT here" comment) defaults to None, matching the pre-P3 fallback
    // behavior exactly: no weight is legal there either way.
    registry::WeightPolicy policy = weightSpec ? weightSpec->weight : registry::WeightPolicy::None;
    bool allowed = false;
    switch (policy) {
      case registry::WeightPolicy::None: allowed = false; break;
      case registry::WeightPolicy::FweightOnly: allowed = isFweight; break;
      case registry::WeightPolicy::AweightFweight: allowed = isAweight || isFweight; break;
      case registry::WeightPolicy::PweightOnly: allowed = isPweight; break;
      case registry::WeightPolicy::AllWeights: allowed = isAweight || isFweight || isPweight || isIweight; break;
    }
    if (!allowed) {
      // BUG-105 secondary ②: this gate runs on a `cmd` that was split out only
      // moments ago and never resolved, so for an unrouted word it invents a
      // feature gap ("`xyzzy` does not support weighted estimation") for a
      // command that does not exist. Answer the FIRST fact instead — the very
      // block dispatcher.cpp's bottom fallthrough (:4227) would have produced
      // had this qualifier scan not short-circuited it. The reject itself is
      // NOT new: an unrouted word misses findByName, so `allowed` is already
      // false and this line already threw — only the message changes.
      if (!isRoutableCommandToken(cmd)) {
        throw TokenizerError(unsupportedCommand(cmd));
      }
      std::string hint;
      switch (policy) {
        case registry::WeightPolicy::FweightOnly:
          hint = "`" + cmd + "` only supports frequency weights ([fw=]/[fweight=]) in this engine; "
                 "[" + *q.weightType + "=] is not supported here. Remove the weight specification or "
                 "switch to fweight — a different weight type will change the estimation result.";
          break;
        case registry::WeightPolicy::AweightFweight:
          hint = "`" + cmd + "` only supports analytic or frequency weights ([aw=]/[aweight=]/[fw=]/"
                 "[fweight=]) in this engine; [" + *q.weightType + "=] is not supported here. Remove "
                 "the weight specification or switch to a supported weight type — a different weight "
                 "type will change the estimation result.";
          break;
        case registry::WeightPolicy::PweightOnly:
          hint = "`" + cmd + "` only supports probability weights ([pw=]/[pweight=]) in this engine; "
                 "[" + *q.weightType + "=] is not supported here. Remove the weight specification or "
                 "switch to pweight — a different weight type will change the estimation result.";
          break;
        case registry::WeightPolicy::None:
        case registry::WeightPolicy::AllWeights:
        default:
          // AllWeights never reaches this branch (allowed is true for all
          // four regex-captured spellings) — kept in the switch only so
          // every enumerator is handled explicitly; falls through to the
          // same "zero weight support" wording None always used, verbatim
          // (freeze-preserved: existing tests pin this exact string).
          hint = "`" + cmd + "` does not support weighted estimation in this engine (no "
                 "[aw=]/[fw=]/[pw=]/[iw=] support for this command). Remove the weight specification "
                 "— unweighted estimation will differ from weighted results.";
          break;
      }
      throw TokenizerError(unsupportedOption(
          "[" + *q.weightType + "=" + q.weightVar.value_or("") + "]", hint));
    }
  }

  // Guard: `in L/H` row-range has no engine counterpart, except for the
  // registry's allowInRange commands. This checks the OUTER command token
  // only, so `bysort V: gen ... in a/b` still correctly rejects here
  // (cmd == "bysort"/"by", not "gen", and bysort/by are deliberately NOT
  // registry rows — registry.h) — Stata's `in` is a GLOBAL obs range, which
  // a by-group-relative `_n` cannot express, so by/bysort stays out of the
  // legal set for EVERY command, not just these.
  //
  // A3 (BETA campaign, BS-096/D7): registry-driven instead of the old
  // hardcoded `inRangeAllowed` list — this table IS the tokenizer's `in`
  // allow-list now, not a separate copy of one.
  //
  // BS-090 (BETA campaign, Phase G): list/browse/br (native row-range
  // display filter, no engine call) and gen/generate/g/replace (BUG-077:
  // folded into the same global-_n bygen path standalone `_n`/`_N` already
  // uses — see egen.cpp's runBysortGen) were the ONLY legal commands before
  // this phase. This phase adds real `in`-range support to a further batch
  // — summarize/sum, count, ttest, corr/correlate/pwcorr, regress/reg/r/
  // feols/reghdfe, tabulate/tab/tab1/tab2, duplicates, egen, drop, keep,
  // collapse (registry.cpp's field-derivation comment has the full list and
  // the per-command mechanism) — via a DIFFERENT composition path than
  // gen/replace's: piet::handlers::applyIfAndInFilter materializes a real
  // numeric row-index column first (op-cobra's `_n`/`_N` tokens throw
  // outside a bysort context, so the naive `_n`-fold gen/replace uses can't
  // generalize to a plain --keepif call), THEN folds `in a/b` into an
  // ordinary numeric predicate on that column, ANDed with `if` — same
  // AND-composition CONTRACT as gen/replace's path, different mechanism
  // under the hood. Reject message names the command (was fully
  // command-blind before A3).
  if (q.inRange) {
    std::string cmdLowerFull = toLower(cmd);
    const registry::CommandSpec* inSpec = registry::findByName(cmdLowerFull);
    bool cmdAllowsInRange = inSpec && inSpec->allowInRange;
    if (!cmdAllowsInRange) {
      // BUG-105 secondary ② (the finding's named anchor): `l price in 1/5`
      // before the abbreviation stopgap answered "`l` does not support the `in`
      // row-range qualifier" — a feature-gap message for a command PIET had
      // never heard of. Same reordering as the weight gate above: unknown
      // command wins over unknown qualifier, and the accepted-input set is
      // untouched (an unrouted word already failed cmdAllowsInRange).
      if (!isRoutableCommandToken(cmd)) {
        throw TokenizerError(unsupportedCommand(cmd));
      }
      throw TokenizerError(unsupportedOption(
          "in " + *q.inRange,
          "`" + cmd + "` does not support the `in` row-range qualifier in this engine. "
          "Use `if` to subset rows by a condition (maps to --keepif)."));
    }
  }

  ParsedCommand pc;
  pc.cmd = cmd;
  pc.varlist = varlist;
  pc.ifExpr = q.ifExpr;
  pc.inRange = q.inRange;
  pc.weightType = q.weightType;
  pc.weightVar = q.weightVar;
  pc.opts = opts;
  pc.raw = raw;
  return pc;
}

std::vector<std::string> preprocessDo(const std::string& source) {
  // Python str.splitlines() — split on \n, \r\n, \r (and a handful of other
  // unicode line boundaries we don't expect in Stata do-files).
  std::vector<std::string> rawLines;
  {
    std::string cur;
    for (size_t i = 0; i < source.size(); ++i) {
      char c = source[i];
      if (c == '\r') {
        rawLines.push_back(cur);
        cur.clear();
        if (i + 1 < source.size() && source[i + 1] == '\n') ++i;
      } else if (c == '\n') {
        rawLines.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    // splitlines() does NOT emit a trailing empty line for a final newline;
    // it only appends the last segment if non-empty OR if there was no
    // trailing newline at all. Since we already flushed on every \n/\r, `cur`
    // holds exactly that trailing segment (empty when the source ended in a
    // newline) — Python's splitlines() omits it in that case.
    if (!cur.empty()) rawLines.push_back(cur);
  }
  auto continued = joinContinuations(rawLines);
  auto stripped = stripComments(continued);
  return stripped;
}

}  // namespace piet::eval
