// Case-compatibility shim (WP-W1, ours — NOT part of the Microsoft SDK).
//
// Microsoft's WebView2.h does `#include "EventToken.h"` (CamelCase, line
// ~1819), which resolves fine on Windows's case-insensitive filesystem but
// not in a MinGW-w64 cross-compile on a case-sensitive Linux FS, where the
// toolchain ships the header as lowercase <eventtoken.h> (declares
// EventRegistrationToken). A quoted include searches the including file's
// own directory FIRST, so this one-liner placed beside the vendored
// WebView2.h bridges the name without patching the SDK header itself
// (which stays verbatim for provenance — see vendor/VENDOR.md).
// _WIN32 native (MinGW on a case-INSENSITIVE FS): a plain #include
// <eventtoken.h> would re-match THIS shim (it sits on the -I vendor/webview2
// search path, and EventToken.h == eventtoken.h to the OS), recursing until the
// preprocessor's max include depth. #include_next skips the directory this
// header was found in and resumes the search after it, reaching MinGW's real
// lowercase <eventtoken.h>. On case-sensitive Linux it behaves identically.
// Windows-only vendor file: the macOS(Cocoa)/Linux(GTK) builds never include it.
#include_next <eventtoken.h>
