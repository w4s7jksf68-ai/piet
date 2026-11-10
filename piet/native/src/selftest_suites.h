// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#ifndef PIET_NATIVE_SELFTEST_SUITES_H
#define PIET_NATIVE_SELFTEST_SUITES_H

#include <string>

// Default/full execution retains its original call order. Selected suites are
// developer feedback only; integration gates continue to invoke the full run.
int runSelftest();
int runSelftestSuite(const std::string& name);
void listSelftestSuites();

#endif  // PIET_NATIVE_SELFTEST_SUITES_H
