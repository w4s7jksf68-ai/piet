// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session time metadata. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

std::string SessionState::stataHex21(double v) {
    char buf[48];
    if (v == 0.0 || !std::isfinite(v)) {
      std::snprintf(buf, sizeof(buf), "+0.0000000000000X-3ff");
      return buf;
    }
    int e = 0;
    const double m = std::frexp(std::fabs(v), &e) * 2.0;  // [1, 2)
    --e;
    const auto frac = static_cast<unsigned long long>(std::ldexp(m - 1.0, 52));
    std::snprintf(buf, sizeof(buf), "%c1.%013llxX%c%03x", v < 0 ? '-' : '+', frac,
                  e < 0 ? '-' : '+', static_cast<unsigned>(e < 0 ? -e : e));
    return buf;
  }

std::optional<double> SessionState::parseStataHex21(const std::string& s) {
    const size_t x = s.find_first_of("Xx");
    if (s.size() < 6 || x == std::string::npos || x + 2 > s.size()) return std::nullopt;
    size_t i = 0;
    double sign = 1.0;
    if (s[i] == '+' || s[i] == '-') sign = (s[i++] == '-') ? -1.0 : 1.0;
    auto hexv = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (i >= x || hexv(s[i]) < 0) return std::nullopt;
    double mant = hexv(s[i++]);
    if (i < x && s[i] == '.') ++i;
    double scale = 1.0 / 16.0;
    for (; i < x; ++i, scale /= 16.0) {
      const int d = hexv(s[i]);
      if (d < 0) return std::nullopt;
      mant += d * scale;
    }
    size_t j = x + 1;
    int esign = 1;
    if (s[j] == '+' || s[j] == '-') esign = (s[j++] == '-') ? -1 : 1;
    if (j >= s.size()) return std::nullopt;
    long ex = 0;
    for (; j < s.size(); ++j) {
      const int d = hexv(s[j]);
      if (d < 0) return std::nullopt;
      ex = ex * 16 + d;
    }
    return sign * std::ldexp(mant, static_cast<int>(esign * ex));
  }

void SessionState::writeTsChars() {
    characteristics.erase(
        std::remove_if(characteristics.begin(), characteristics.end(),
                       [](const DtaCharacteristic& c) {
                         return c.varname == "_dta" && isTsCharName(c.charname);
                       }),
        characteristics.end());
    const bool hasP = panelvar.has_value() && !panelvar->empty();
    const bool hasT = timevar.has_value() && !timevar->empty();
    if (hasP) {
      characteristics.push_back(DtaCharacteristic{"_dta", "iis", *panelvar});
      characteristics.push_back(DtaCharacteristic{"_dta", "_TSpanel", *panelvar});
    }
    if (hasT) {
      characteristics.push_back(DtaCharacteristic{"_dta", "tis", *timevar});
      characteristics.push_back(
          DtaCharacteristic{"_dta", "_TSdelta", stataHex21(delta.value_or(1.0))});
      characteristics.push_back(DtaCharacteristic{"_dta", "_TStvar", *timevar});
    }
  }

void SessionState::adoptTsFromChars() {
    panelvar.reset();
    timevar.reset();
    delta.reset();
    auto pick = [&](const char* primary, const char* legacy) -> std::optional<std::string> {
      const std::string* v = dtaChar(primary);
      if (v == nullptr || v->empty()) v = dtaChar(legacy);
      if (v == nullptr || v->empty()) return std::nullopt;
      return *v;
    };
    panelvar = pick("_TSpanel", "iis");
    timevar = pick("_TStvar", "tis");
    if (timevar.has_value()) {
      if (const std::string* d = dtaChar("_TSdelta")) {
        std::optional<double> dv = parseStataHex21(*d);
        if (dv.has_value() && *dv > 0.0 && *dv != 1.0) delta = dv;
      }
    }
  }

void SessionState::noteCheckpointTsDecl(const std::string& path) {
    CheckpointTsDecl ck{panelvar, timevar, delta};
    for (const auto& [name, vm] : varMeta)
      if (vm.stataType.has_value() && !vm.stataType->empty()) ck.stataTypes[name] = *vm.stataType;
    ck.implicitFloat = implicitFloatVars;
    ck.varLblname = varLblname;
    ck.labelDefs = labelDefs;
    ck.valueLabels = valueLabels;
    ck.dataLabel = dataLabel;
    ck.characteristics = characteristics;
    ck.labelLang = labelLang;
    ck.labelLangs = labelLangs;
    ck.labelLangStash = labelLangStash;
    checkpointTsDecl_[path] = std::move(ck);
  }

bool SessionState::adoptCheckpointTsDecl(const std::string& path) {
    auto it = checkpointTsDecl_.find(path);
    if (it == checkpointTsDecl_.end()) return false;
    panelvar = it->second.panelvar;
    timevar = it->second.timevar;
    delta = it->second.delta;
    characteristics = it->second.characteristics;
    writeTsChars();
    for (const auto& [name, t] : it->second.stataTypes) varMeta[name].stataType = t;
    implicitFloatVars = it->second.implicitFloat;
    varLblname = it->second.varLblname;
    labelDefs = it->second.labelDefs;
    valueLabels = it->second.valueLabels;
    dataLabel = it->second.dataLabel;
    labelLang = it->second.labelLang;
    labelLangs = it->second.labelLangs;
    labelLangStash = it->second.labelLangStash;
    return true;
  }

}
