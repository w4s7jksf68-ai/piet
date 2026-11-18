// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Raw DEFLATE (RFC 1951) compression of a byte string, bit-for-bit what zlib's
// deflate() writes for the same level (window 32 KiB, memLevel 8, default
// strategy), so the size of an archive entry is the size zlib-based tools give.
#pragma once

#include <string>

namespace piet::fileio {

// `level` 0 (stored) to 9; a value outside the range is the default, 6. Level 0
// is one stored block per 65535 bytes.
std::string rawDeflate(const std::string& input, int level);

}  // namespace piet::fileio
