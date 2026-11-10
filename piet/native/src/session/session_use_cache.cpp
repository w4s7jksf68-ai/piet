// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: the `use` conversion/stash memos and their byte cap.
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)

namespace piet {

// PP-L2A / K1a. Definition lives here (not the header) because releasing a
// superseded memo needs reclaimSupersededScratch, which is defined above.
void SessionState::useConvCachePut(const std::string& src, const UseConvCacheEntry& entry) {
  if (entry.statKey.empty() || entry.hbinPath.empty()) return;  // unkeyable => not cacheable
  auto it = useConvCache_.find(src);
  if (it != useConvCache_.end()) {
    const std::string stale = it->second.hbinPath;
    useConvCache_.erase(it);  // drop the exclusion-(3) pin FIRST ...
    if (!stale.empty() && stale != entry.hbinPath) {
      reclaimSupersededScratch(stale);  // ... so the ordinary GC can now take it
    }
  }
  useConvCache_[src] = entry;
  useCacheTouch_[src] = ++useCacheClock_;  // BUG-390
  enforceUseCacheCap();
}

// BUG-390: byte-bounded `use` memos ────────────────────────────────────────────
std::uintmax_t SessionState::useCacheCapBytes() {
  auto envMb = [](const char* name) -> long long {
    const char* v = std::getenv(name);
    if (!v || !*v) return -1;
    char* end = nullptr;
    long long mb = std::strtoll(v, &end, 10);
    return (end && *end == '\0' && mb >= 0) ? mb : -1;
  };
  long long mb = envMb("PIET_USE_CACHE_MB");
  if (mb < 0) {
    long long budget = envMb("CECON_MEMORY_BUDGET_MB");
    mb = budget > 0 ? budget / 2 : 1024;
  }
  return static_cast<std::uintmax_t>(mb) * 1024ull * 1024ull;
}

std::uintmax_t SessionState::useCacheBytes() const {
  namespace fs = std::filesystem;
  std::set<std::string> seen;
  std::uintmax_t total = 0;
  auto add = [&](const std::string& path) {
    if (path.empty() || !seen.insert(path).second) return;
    std::error_code e;
    auto sz = fs::file_size(cecon::to_native_path(path), e);
    if (!e) total += sz;
  };
  for (const auto& [k, e] : useConvCache_) { (void)k; add(e.hbinPath); }
  for (const auto& [k, e] : useStashCache_) { (void)k; add(e.hbinPath); }
  return total;
}

void SessionState::dropUseCacheEntry(const std::string& src) {
  std::vector<std::string> paths;
  if (auto it = useConvCache_.find(src); it != useConvCache_.end()) {
    paths.push_back(it->second.hbinPath);
    paths.push_back(it->second.metaPath);
    useConvCache_.erase(it);
  }
  if (auto it = useStashCache_.find(src); it != useStashCache_.end()) {
    if (cobraSession_ && cobraSession_->hasWorker() && !it->second.stashName.empty())
      (void)cobraSession_->frameStashDrop(it->second.stashName);
    paths.push_back(it->second.hbinPath);
    paths.push_back(it->second.metaPath);
    useStashCache_.erase(it);
  }
  useCacheTouch_.erase(src);
  // Entries erased FIRST so exclusions (3)/(4) no longer pin; every other pin
  // (live head, preserve twin, frame, ...) still applies inside the primitive.
  for (const auto& p : paths)
    if (!p.empty()) reclaimSupersededScratch(p);
}

void SessionState::dropUseCaches() {
  std::set<std::string> srcs;
  for (const auto& [k, e] : useConvCache_) { (void)e; srcs.insert(k); }
  for (const auto& [k, e] : useStashCache_) { (void)e; srcs.insert(k); }
  for (const auto& src : srcs) dropUseCacheEntry(src);
}

void SessionState::enforceUseCacheCap() {
  const std::uintmax_t cap = useCacheCapBytes();
  if (cap == 0) return;  // 0 = unbounded (explicit PIET_USE_CACHE_MB=0)
  auto sourceCount = [&]() {
    std::set<std::string> srcs;
    for (const auto& [k, e] : useConvCache_) { (void)e; srcs.insert(k); }
    for (const auto& [k, e] : useStashCache_) { (void)e; srcs.insert(k); }
    return srcs.size();
  };
  // Stop at ONE source, not one entry: a source's conv and stash memos share a
  // recency key and are evicted together, so counting entries evicted the last
  // remaining source too (the first cut emptied the cache on every put).
  while (sourceCount() > 1 && useCacheBytes() > cap) {
    // Evict the least recently used source. An entry never touched gets clock 0.
    std::string victim;
    std::uint64_t best = ~0ull;
    auto consider = [&](const std::string& src) {
      auto t = useCacheTouch_.find(src);
      std::uint64_t stamp = t == useCacheTouch_.end() ? 0 : t->second;
      if (stamp < best) { best = stamp; victim = src; }
    };
    for (const auto& [k, e] : useConvCache_) { (void)e; consider(k); }
    for (const auto& [k, e] : useStashCache_) { (void)e; consider(k); }
    if (victim.empty()) break;
    if (scratchTraceOn()) scratchTrace("EVICT", "use-cache-lru", victim);
    dropUseCacheEntry(victim);
  }
}

void SessionState::useStashCachePut(const std::string& src, const UseStashCacheEntry& entry) {
  if (entry.statKey.empty() || entry.stashName.empty() || entry.hbinPath.empty()) return;
  useStashCache_[src] = entry;
  useCacheTouch_[src] = ++useCacheClock_;
  enforceUseCacheCap();
}

}  // namespace piet
