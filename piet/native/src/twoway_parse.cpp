#include "piet/twoway_parse.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "piet/strutil.h"

namespace piet::twoway {

namespace {

using eval::OptValue;
using eval::findUnquotedComma;
using eval::parseOpts;

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

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// above is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }

// Trailing `in range` extraction: `\bin\s+(\S+)\s*$` (re.IGNORECASE).
const std::regex& reIn() {
  static const std::regex r(R"(\bin\s+(\S+)\s*$)", std::regex::icase);
  return r;
}

// `_extract_if_in(s)`: strip a trailing `if expr` (and optional `in range`)
// from *s*. Returns (cleaned, if_expr, in_range).
struct IfInResult {
  std::string cleaned;
  std::optional<std::string> ifExpr;
  std::optional<std::string> inRange;
};

IfInResult extractIfIn(const std::string& sIn) {
  std::string s = strip(sIn);
  std::optional<std::string> inRange;

  // BUG-115 class sweep: use the quote-aware search (eval::searchUnquoted)
  // so a qualifier-shaped substring inside a quoted literal (e.g. `scatter y
  // x, title("in progress")`) is never mistaken for a real `in`/`if`
  // qualifier — same fix as the main tokenizer's extractQualifiers.
  using eval::searchUnquoted;
  std::smatch mIn;
  if (searchUnquoted(s, reIn(), mIn)) {
    inRange = mIn[1].str();
    s = rstrip(s.substr(0, static_cast<size_t>(mIn.position(0))));
  }

  std::optional<std::string> ifExpr;
  // `re.split(r'(?:^|\s+)if\s+', s, maxsplit=1, IGNORECASE)` — match a
  // whitespace-bounded `if` keyword OR an `if` at the very start of the
  // (stripped) string.
  static const std::regex reIfSplit(R"((?:^|\s+)if\s+)", std::regex::icase);
  std::smatch mIf;
  if (searchUnquoted(s, reIfSplit, mIf)) {
    std::string before = s.substr(0, static_cast<size_t>(mIf.position(0)));
    std::string after = s.substr(static_cast<size_t>(mIf.position(0) + mIf.length(0)));
    s = rstrip(before);
    ifExpr = rstrip(after);
  }

  return {strip(s), ifExpr, inRange};
}

// `_scan_toplevel_parens(s)`: [(start,end), ...] for every top-level (...)
// group. Depth tracking mirrors findUnquotedComma exactly (quote/compound-
// quote aware) so the two stay consistent — ported directly from the
// Python reference algorithm (not reusing findUnquotedComma itself, since
// that scans for commas not parens; the state machine shape is shared).
std::vector<std::pair<size_t, size_t>> scanToplevelParens(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> groups;
  int depth = 0;
  bool inQuote = false;
  int compound = 0;  // depth of `"..."' compound quotes
  long long groupStart = -1;
  size_t i = 0;
  size_t n = s.size();
  while (i < n) {
    if (compound > 0) {
      if (i + 1 < n && s[i] == '"' && s[i + 1] == '\'') {
        --compound;
        i += 2;
        continue;
      }
      if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
        ++compound;
        i += 2;
        continue;
      }
      ++i;
      continue;
    }
    if (inQuote) {
      if (s[i] == '"') inQuote = false;
      ++i;
      continue;
    }
    if (i + 1 < n && s[i] == '`' && s[i + 1] == '"') {
      ++compound;
      i += 2;
      continue;
    }
    if (s[i] == '"') {
      inQuote = true;
      ++i;
      continue;
    }
    if (s[i] == '(') {
      if (depth == 0) groupStart = static_cast<long long>(i);
      ++depth;
      ++i;
      continue;
    }
    if (s[i] == ')') {
      if (depth > 0) {
        --depth;
        if (depth == 0 && groupStart >= 0) {
          groups.emplace_back(static_cast<size_t>(groupStart), i + 1);
          groupStart = -1;
        }
      }
      ++i;
      continue;
    }
    ++i;
  }
  return groups;
}

// `_parse_layer_interior(interior)`: parse the inside of one (...) layer
// group — a mini-command `plottype varlist [if expr] [, layer_opts]`.
LayerSpec parseLayerInterior(const std::string& interiorIn) {
  std::string interior = strip(interiorIn);
  long long commaIdx = findUnquotedComma(interior);
  std::string pre, post;
  if (commaIdx >= 0) {
    pre = strip(interior.substr(0, static_cast<size_t>(commaIdx)));
    post = strip(interior.substr(static_cast<size_t>(commaIdx) + 1));
  } else {
    pre = interior;
    post = "";
  }

  IfInResult ifin = extractIfIn(pre);
  pre = ifin.cleaned;

  LayerSpec spec;
  // tokens = pre.split() — whitespace-delimited, Python semantics (any run
  // of whitespace is a separator, leading/trailing ignored).
  std::vector<std::string> tokens;
  {
    size_t i = 0, n = pre.size();
    while (i < n) {
      while (i < n && isPySpace(pre[i])) ++i;
      if (i >= n) break;
      size_t start = i;
      while (i < n && !isPySpace(pre[i])) ++i;
      tokens.push_back(pre.substr(start, i - start));
    }
  }
  if (tokens.empty()) {
    return spec;  // plottype == "" -> caller skips degenerate empty groups
  }
  spec.plottype = toLower(tokens[0]);
  // AER-SCOPE1 bundle A: `sc` is Stata's accepted truncation of `scatter`, and
  // the corpus writes `tw (sc x y) (sc x z)` -- a layer plottype is read here,
  // never through the command registry, so the registry abbreviation that makes
  // `tw` work does nothing for the layer heads inside it.
  if (spec.plottype == "sc") spec.plottype = "scatter";
  spec.varlist.assign(tokens.begin() + 1, tokens.end());
  spec.ifExpr = ifin.ifExpr;
  spec.opts = post.empty() ? std::vector<std::pair<std::string, OptValue>>() : parseOpts(post);
  return spec;
}

}  // namespace

ParseResult parseTwoway(const std::string& rawAfterCmd) {
  ParseResult result;
  std::string raw = strip(rawAfterCmd);
  if (raw.empty()) return result;

  // Step 1: split off the global option block at the first top-level comma.
  long long globalComma = findUnquotedComma(raw);
  std::string layerRegion, globalOptsStr;
  if (globalComma >= 0) {
    layerRegion = strip(raw.substr(0, static_cast<size_t>(globalComma)));
    globalOptsStr = strip(raw.substr(static_cast<size_t>(globalComma) + 1));
  } else {
    layerRegion = raw;
    globalOptsStr = "";
  }
  result.globalOpts = globalOptsStr.empty() ? std::vector<std::pair<std::string, OptValue>>()
                                             : parseOpts(globalOptsStr);

  // Step 2: scan for top-level parenthesised groups FIRST (a global if/in is
  // only valid OUTSIDE all groups).
  std::vector<std::pair<size_t, size_t>> groups = scanToplevelParens(layerRegion);

  if (groups.empty()) {
    // Bare single-layer form (no parens): "line y x [if expr], opts".
    IfInResult ifin = extractIfIn(layerRegion);
    std::string bare = strip(ifin.cleaned);
    result.ifExpr = ifin.ifExpr;
    result.inRange = ifin.inRange;
    if (bare.empty()) return result;
    std::vector<std::string> tokens;
    {
      size_t i = 0, n = bare.size();
      while (i < n) {
        while (i < n && isPySpace(bare[i])) ++i;
        if (i >= n) break;
        size_t start = i;
        while (i < n && !isPySpace(bare[i])) ++i;
        tokens.push_back(bare.substr(start, i - start));
      }
    }
    LayerSpec spec;
    spec.plottype = toLower(tokens[0]);
  // AER-SCOPE1 bundle A: `sc` is Stata's accepted truncation of `scatter`, and
  // the corpus writes `tw (sc x y) (sc x z)` -- a layer plottype is read here,
  // never through the command registry, so the registry abbreviation that makes
  // `tw` work does nothing for the layer heads inside it.
  if (spec.plottype == "sc") spec.plottype = "scatter";
    spec.varlist.assign(tokens.begin() + 1, tokens.end());
    spec.ifExpr = std::nullopt;
    spec.opts = result.globalOpts;  // the global option block becomes the layer's own options
    result.layers.push_back(std::move(spec));
    return result;
  }

  // Step 3: a global if/in may trail the LAST group at depth 0.
  size_t lastEnd = groups.back().second;
  std::string tail = layerRegion.substr(lastEnd);
  IfInResult tailIfin = extractIfIn(tail);
  result.ifExpr = tailIfin.ifExpr;
  result.inRange = tailIfin.inRange;

  // Step 4: parse each parenthesised group as an independent layer.
  for (const auto& [start, end] : groups) {
    std::string interior = layerRegion.substr(start + 1, (end - 1) - (start + 1));
    LayerSpec spec = parseLayerInterior(interior);
    if (!spec.plottype.empty()) result.layers.push_back(std::move(spec));
  }
  return result;
}

bool hasToplevelParens(const std::string& rawAfterCmd) {
  std::string raw = strip(rawAfterCmd);
  if (raw.empty()) return false;
  long long globalComma = findUnquotedComma(raw);
  std::string region = globalComma >= 0 ? raw.substr(0, static_cast<size_t>(globalComma)) : raw;
  return !scanToplevelParens(region).empty();
}

}  // namespace piet::twoway
