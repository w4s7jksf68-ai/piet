// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session metadata. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

std::vector<std::string> SessionState::labelSetOrder() const {
    std::vector<std::string> names;
    for (const auto& kv : labelDefs) names.push_back(kv.first);
    std::stable_sort(names.begin(), names.end(), [this](const std::string& a, const std::string& b) {
      const auto ia = labelSeq.find(a), ib = labelSeq.find(b);
      const unsigned long long sa = ia == labelSeq.end() ? 0 : ia->second;
      const unsigned long long sb = ib == labelSeq.end() ? 0 : ib->second;
      return sa > sb;
    });
    return names;
  }

std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>>
  SessionState::valueLabelMapForBrowse() const {
    std::vector<std::pair<std::string, std::vector<std::pair<int, std::string>>>> out;
    out.reserve(valueLabels.size());
    for (const auto& [varname, codeMap] : valueLabels) {
      std::vector<std::pair<int, std::string>> pairs;
      pairs.reserve(codeMap.size());
      for (const auto& [code, text] : codeMap) pairs.emplace_back(code, text);
      out.emplace_back(varname, std::move(pairs));
    }
    return out;
  }

void SessionState::forgetVarPlanes(const std::string& name) {
    varMeta.erase(name);
    implicitFloatVars.erase(name);
    userFormattedVars.erase(name);
    varLblname.erase(name);
    valueLabels.erase(name);
  }

void SessionState::clearMetadata() {
    varMeta.clear();
    implicitFloatVars.clear();
    userFormattedVars.clear();
    labelDefs.clear();
    labelLang = "default";
    labelLangs = {"default"};
    labelLangStash.clear();
    varLblname.clear();
    valueLabels.clear();
    dataLabel.clear();
    characteristics.clear();
    // xtset-file-persistence: the tsset/xtset declaration IS a set of `_dta`
    // characteristics in Stata (_TSpanel/_TStvar/_TSdelta/iis/tis), so a verb
    // that drops the char table drops the declaration with it — `clear`,
    // `import`, `filelist`, and `use` (which re-adopts it from the file's own
    // chars; see adoptTsFromChars). Before this, panelvar/timevar survived every
    // dataset replacement and `xtreg, fe` after `use` of an un-xtset file ran on
    // the PREVIOUS dataset's panel id (w2_eststo_prefix.log:441, rc 459 there).
    panelvar.reset();
    timevar.reset();
    delta.reset();
  }

}
