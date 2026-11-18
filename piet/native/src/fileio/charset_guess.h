// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The text-encoding guess behind r(encoding) of `import delimited`.
#pragma once

#include <string>

namespace piet::fileio {

// The name Stata reports in r(encoding) for text it was given no encoding() for:
// "UTF-8" or the Latin-set name its charset detector picks for the bytes.
// Text that holds a byte above 0x7F is "UTF-8" when it is valid UTF-8 and
// "ISO-8859-1" otherwise; plain ASCII text goes to the trigram scorer.
std::string guessTextEncoding(const std::string& bytes, bool validUtf8);

}  // namespace piet::fileio
