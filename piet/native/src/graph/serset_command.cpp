// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset_command.cpp — the `serset` command family (`serset`, `serset [set] #`,
// `serset use`, `clear`, `drop`, `dir`). See include/piet/serset.h.
#include "piet/serset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"
#include "serset_internal.h"

namespace piet::serset {

using ojson = nlohmann::ordered_json;
using namespace serset_detail;

namespace {

bool parseInt(const std::string& s, int* out) {
  const std::string t = trimText(s);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') i = 1;
  if (i >= t.size()) return false;
  for (size_t j = i; j < t.size(); ++j)
    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
  try {
    *out = std::stoi(t);
  } catch (...) {
    return false;
  }
  return true;
}


std::string csvNumber(double v) {
  if (std::isnan(v)) return ".";
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string csvString(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// `serset use`: replaces the data in memory with the serset's variables, their
// names and storage types (the w2al helper reads `: type` of each).
ojson useSerset(SessionState& session, const Serset& s) {
  ojson out = ojson::array();
  if (!s.retained) {
    out.push_back(refusal("serset use: this serset holds " + std::to_string(s.n) + " observations on " +
                              std::to_string(s.cols.size()) +
                              " variables, more than PIET keeps for read-back; its data were not "
                              "retained",
                          910));
    return out;
  }
  const std::string hbin = mintScratchPath(session.workdir(), "serset");
  const std::string csv = hbin + ".csv";
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      out.push_back(refusal("serset use: could not stage the serset's data", 603));
      return out;
    }
    for (size_t c = 0; c < s.cols.size(); ++c) {
      if (c) f << ",";
      f << s.cols[c].name;
    }
    f << "\n";
    for (int64_t i = 0; i < s.n; ++i) {
      for (size_t c = 0; c < s.cols.size(); ++c) {
        if (c) f << ",";
        const Column& col = s.cols[c];
        const size_t r = static_cast<size_t>(i);
        if (col.isStr) {
          f << csvString(r < col.str.size() ? col.str[r] : std::string());
        } else {
          f << csvNumber(r < col.num.size() ? col.num[r] : kNaN);
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csv);
      out.push_back(refusal("serset use: could not stage the serset's data (disk full or I/O error)",
                            693));
      return out;
    }
  }
  std::string factorList;
  for (const Column& c : s.cols) {
    if (!c.isStr) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += c.name;
  }
  std::vector<std::string> args = {"convert", "--data", csv, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  const CobraRunResult cr = runCobra(args);
  cecon::native_unlink(csv);
  if (!cr.ok || !cecon::native_exists(hbin)) {
    cecon::native_unlink(hbin);
    out.push_back(refusal("serset use: the serset's data could not be loaded", 693));
    return out;
  }
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.invalidateDatastore();
  for (const Column& c : s.cols) session.varMeta[c.name].stataType = c.type;
  out.push_back(handlers::mutationResult(session, "use"));
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The `serset` command family

ojson runSerset(SessionState& session, const std::vector<std::string>& args, bool clearOpt) {
  (void)clearOpt;
  ojson out = ojson::array();
  Store& st = session.sersets;
  const std::string sub = args.empty() ? std::string() : strutil::lower(args[0]);

  // `serset`: r(id) of the current serset (-1 when none), r(k), r(N).
  if (sub.empty()) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("id", static_cast<double>(st.current()));
    std::vector<std::string> posted = {"id"};
    if (const Serset* s = st.currentSet()) {
      session.eval.rValues.set("k", static_cast<double>(s->cols.size()));
      session.eval.rValues.set("N", static_cast<double>(s->n));
      posted = {"id", "k", "N"};
    }
    eval::declareRPosted(session.eval, "serset", posted);
    return out;
  }

  // `serset [set] #`
  int id = 0;
  if (sub == "set" || parseInt(args[0], &id)) {
    const std::string num = sub == "set" ? (args.size() > 1 ? args[1] : std::string()) : args[0];
    if (!parseInt(num, &id)) {
      out.push_back(refusal("serset set: a serset number is required", 198));
      return out;
    }
    if (!st.select(id)) {
      out.push_back(refusal("series " + std::to_string(id) + " not found", 111));
      return out;
    }
    return out;
  }

  // `serset use [, clear]`: the current serset becomes the data (w2al log:183-188
  // `no serset in use`, r(198)).
  if (sub == "use") {
    const Serset* s = st.currentSet();
    if (s == nullptr) {
      out.push_back(refusal("no serset in use", 198));
      return out;
    }
    const Serset copy = *s;
    ojson blocks = useSerset(session, copy);
    for (auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        out.push_back(b);
        return out;
      }
    }
    eval::replaceRClass(session.eval);
    std::string names;
    for (size_t i = 0; i < copy.cols.size(); ++i) {
      if (i) names += " ";
      names += copy.cols[i].name;
    }
    session.eval.rStrings.set("varnames", names);
    eval::declareRPosted(session.eval, "serset use", {"varnames"});
    for (auto& b : blocks) out.push_back(b);
    return out;
  }

  if (sub == "clear") {
    st.clear();
    return out;
  }

  if (sub == "drop") {
    if (args.size() < 2) {
      st.clear();
      return out;
    }
    std::vector<int> ids;
    for (size_t i = 1; i < args.size(); ++i) {
      const std::string a = args[i];
      if (strutil::lower(a) == "_all") {
        st.clear();
        return out;
      }
      const size_t slash = a.find('/');
      int lo = 0, hi = 0;
      if (slash != std::string::npos && parseInt(a.substr(0, slash), &lo) &&
          parseInt(a.substr(slash + 1), &hi)) {
        for (int k = lo; k <= hi; ++k) ids.push_back(k);
      } else if (parseInt(a, &lo)) {
        ids.push_back(lo);
      } else {
        out.push_back(refusal("serset drop: `" + a + "' is not a serset number", 198));
        return out;
      }
    }
    for (int k : ids) st.drop(k);
    return out;
  }

  // `serset dir` (w2al log:5418-5429): the names as stored, a renamed duplicate
  // under its own (StataBE 19.5, BUG-1543: `y x y x` where `serset use` loads
  // `y x y1 x1`).
  if (sub == "dir") {
    std::string text;
    for (const auto& kv : st.all()) {
      const Serset& s = kv.second;
      char head[96];
      std::snprintf(head, sizeof(head), "%3d.  %lld observation%s on %zu variable%s", kv.first,
                    static_cast<long long>(s.n), s.n == 1 ? "" : "s", s.cols.size(),
                    s.cols.size() == 1 ? "" : "s");
      std::string names;
      for (size_t i = 0; i < s.cols.size(); ++i) {
        if (i) names += " ";
        names += storedName(s.cols[i]);
      }
      text += "\n" + std::string(head) + "\n      " + names + "\n";
    }
    if (!text.empty()) out.push_back(displayBlock(text));
    return out;
  }

  out.push_back(refusal("serset " + sub + " is not supported in this version of PIET", 199));
  return out;
}

}  // namespace piet::serset
