#include "piet/eval/gettoken.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"

namespace piet::eval {

namespace {

std::string toLowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// `^gettoken\s+(?P<tok>[A-Za-z_][A-Za-z0-9_]*)\s+(?P<rest>[A-Za-z_][A-Za-z0-9_]*)\s*:\s*
//   (?P<src>[A-Za-z_][A-Za-z0-9_]*)(?P<opts>.*)$`  re.IGNORECASE
const std::regex& reGettoken() {
  static const std::regex r(
      R"(^gettoken\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$)",
      std::regex::icase);
  return r;
}

// BS-033 (BETA campaign C3): the 2-argument destructure form — `gettoken
// TOKNAME : SRCNAME [, opts]` (no RESTNAME). This is a real, documented
// Stata idiom ("peek" the first token without consuming it) that the
// 3-arg-only regex above simply couldn't match at all (syntax_error on
// every use). Real Stata semantics (live-verified this stint, StataBE):
// unlike the 3-arg form, SRCNAME's own macro is left COMPLETELY UNCHANGED
// — this is a peek, not a pop (oracle: `local line "alpha beta gamma"` +
// `gettoken tok : line` -> tok=[alpha], line=[alpha beta gamma], still the
// full original string).
const std::regex& reGettoken2Arg() {
  static const std::regex r(
      R"(^gettoken\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$)",
      std::regex::icase);
  return r;
}

std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  auto isSp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  while (a < b && isSp(s[a])) ++a;
  while (b > a && isSp(s[b - 1])) --b;
  return s.substr(a, b - a);
}

}  // namespace

std::vector<nlohmann::json> runGettoken(const std::string& rawLine, LocalsStore& locals) {
  std::string stripped = strip(rawLine);
  std::smatch m;
  if (std::regex_match(stripped, m, reGettoken())) {
    std::string tokName = m[1].str();
    std::string restName = m[2].str();
    std::string srcName = m[3].str();
    std::string optsRaw = m[4].str();
    bool bind = toLowerCopy(optsRaw).find("bind") != std::string::npos;

    // Read SRC fully before writing any binding (handles aliasing: TOK == SRC).
    std::string srcVal = locals.get(srcName);

    auto [tokVal, remainder] = scanOneToken(srcVal, bind);

    locals.set(tokName, tokVal);
    locals.set(restName, remainder);

    return {};
  }

  // BS-033: 2-arg destructure form — see reGettoken2Arg()'s own comment.
  std::smatch m2;
  if (std::regex_match(stripped, m2, reGettoken2Arg())) {
    std::string tokName = m2[1].str();
    std::string srcName = m2[2].str();
    std::string optsRaw = m2[3].str();
    bool bind = toLowerCopy(optsRaw).find("bind") != std::string::npos;

    std::string srcVal = locals.get(srcName);
    auto [tokVal, remainder] = scanOneToken(srcVal, bind);
    (void)remainder;  // 2-arg form: SRC is a peek, never written back.

    locals.set(tokName, tokVal);
    return {};
  }

  return {engineError(
      "gettoken: syntax error — expected "
      "``gettoken TOKNAME RESTNAME : SRCNAME [, bind]``",
      "syntax_error")};
}

}  // namespace piet::eval
