// SPDX-License-Identifier: GPL-3.0-or-later
#include "selftest_support.h"
namespace piet::selftest {
int g_failures=0;
int g_skipped=0;
std::string selftestValidateDir() {
  if (const char* vd = std::getenv("COBRA_VALIDATE_DIR"); vd && *vd) return vd;
  std::string f = std::string(selftestSourceFile());
  const auto pos = f.rfind("/piet/native/src/selftest.cpp");
  return (pos == std::string::npos) ? std::string("tests/validate")
                                    : f.substr(0, pos) + "/tests/validate";
}

void check(bool cond, const std::string& label) {
  if (cond) {
    std::printf("[selftest]  PASS  %s\n", label.c_str());
  } else {
    std::printf("[selftest]  FAIL  %s\n", label.c_str());
    ++g_failures;
  }
}

void skip(const std::string& label, const std::string& why) {
  std::printf("[selftest]  SKIP  %s (%s)\n", label.c_str(), why.c_str());
  ++g_skipped;
}

std::pair<int, std::string> runCommandCapture(const std::string& cmd) {
#ifdef _WIN32
  FILE* pipe = _popen(cmd.c_str(), "r");
#else
  FILE* pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) return {-1, "popen failed"};
  std::ostringstream out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.write(buf, n);
#ifdef _WIN32
  // _pclose returns the child's exit status directly (no wait(2) encoding).
  int exitCode = _pclose(pipe);
#else
  int status = pclose(pipe);
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return {exitCode, out.str()};
}

std::string commandPathQuote(const std::string& s) {
#ifdef _WIN32
  // A Windows filename cannot contain a literal double quote, so surrounding
  // the UTF-8 command argument is sufficient for cmd.exe/CreateProcess.
  return "\"" + s + "\"";
#else
  // POSIX single-quote escape.
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
#endif
}

std::pair<int, std::string> runNode(const std::string& scriptPath) {
  return runCommandCapture("node " + commandPathQuote(scriptPath) + " 2>&1");
}

bool nodeAvailable() {
#ifdef _WIN32
  return std::system("where node > NUL 2>&1") == 0;
#else
  return std::system("command -v node > /dev/null 2>&1") == 0;
#endif
}

bool parseSemver(const std::string& s, int out[3]) {
  return std::sscanf(s.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

int cmpSemver(const int a[3], const int b[3]) {
  for (int i = 0; i < 3; ++i) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  return 0;
}

bool parsePietVersion(const std::string& s, int out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0;
  if (s.size() > 1 && (s[0] == 'B' || s[0] == 'A') && s[1] == '.') {
    out[0] = (s[0] == 'B') ? 1 : 2;
    return std::sscanf(s.c_str() + 2, "%d.%d", &out[1], &out[2]) == 2;
  }
  return std::sscanf(s.c_str(), "%d.%d.%d", &out[1], &out[2], &out[3]) == 3;
}

int cmpPietVersion(const int a[4], const int b[4]) {
  for (int i = 0; i < 4; ++i) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  return 0;
}

std::string plistStringForKey(const std::string& plist, const std::string& key) {
  std::string needle = "<key>" + key + "</key>";
  auto kpos = plist.find(needle);
  if (kpos == std::string::npos) return "";
  auto spos = plist.find("<string>", kpos);
  if (spos == std::string::npos) return "";
  spos += std::string("<string>").size();
  auto epos = plist.find("</string>", spos);
  if (epos == std::string::npos) return "";
  return plist.substr(spos, epos - spos);
}

std::string currentFileValue(const std::string& text, const std::string& name) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos) continue;
    if (line.compare(p, name.size(), name) != 0) continue;
    size_t eq = line.find('=', p + name.size());
    if (eq == std::string::npos) continue;
    // NAME must be the whole token: only whitespace between NAME and '='.
    if (line.find_first_not_of(" \t", p + name.size()) != eq) continue;
    std::string rest = line.substr(eq + 1);
    size_t hash = rest.find('#');
    if (hash != std::string::npos) rest = rest.substr(0, hash);
    size_t b = rest.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = rest.find_last_not_of(" \t\r\n");
    return rest.substr(b, e - b + 1);
  }
  return "";
}

std::string joinForMsg(const std::vector<std::string>& xs) {
  std::string out;
  for (size_t i = 0; i < xs.size(); ++i) {
    if (i != 0) out += ", ";
    if (i == 8) { out += "... (" + std::to_string(xs.size() - 8) + " more)"; break; }
    out += xs[i];
  }
  return out;
}

const nlohmann::ordered_json* firstBlockOfTypeD1(const nlohmann::ordered_json& blocks,
                                                  const std::string& type) {
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == type) return &b;
  }
  return nullptr;
}

std::optional<double> coefFieldD1(const nlohmann::ordered_json& est, const std::string& name,
                                   const std::string& field) {
  if (!est.contains("coeftable") || !est["coeftable"].is_array()) return std::nullopt;
  for (const auto& row : est["coeftable"]) {
    if (row.value("name", std::string()) == name && row.contains(field) && row[field].is_number()) {
      return row[field].get<double>();
    }
  }
  return std::nullopt;
}

}
