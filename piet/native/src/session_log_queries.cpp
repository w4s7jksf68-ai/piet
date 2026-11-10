// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session log queries. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

bool SessionState::logIsOpen(const std::string& name) const {
    for (const auto& l : logs_) {
      if (l->name == name) return true;
    }
    return false;
  }

std::vector<std::string> SessionState::logNames() const {
    std::vector<std::string> out;
    out.reserve(logs_.size());
    for (const auto& l : logs_) out.push_back(l->name);
    return out;
  }

}
