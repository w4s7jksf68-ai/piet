// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: run progress ticks, cancellation, the user-input round trip, command history, do-worker thread marker.
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

namespace {
// Monotonic seconds, matching Python's time.perf_counter() role (used only
// for elapsed-time deltas, never as a wall-clock timestamp).
double monotonicSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// BUG-087/D9: see session_state.h's doc comment on isCurrentThreadADoWorker()
// for why this exists. thread_local storage duration — each OS thread gets
// its own copy, defaulting false; only main.cpp's runDoInBackground() worker
// lambda ever sets it true, once, at the top of its own thread body.
thread_local bool tl_isDoWorkerThread = false;
}  // namespace

void setCurrentThreadIsDoWorker(bool isWorker) { tl_isDoWorkerThread = isWorker; }
bool isCurrentThreadADoWorker() { return tl_isDoWorkerThread; }

nlohmann::ordered_json RunProgress::asDict() const {
  long long elapsed = elapsedMs;
  if (running && startedAt > 0.0) {
    elapsed = static_cast<long long>((monotonicSeconds() - startedAt) * 1000);
  }
  nlohmann::ordered_json j;
  j["running"] = running;
  j["current_line"] = currentLine;
  j["total_lines"] = totalLines;
  j["current_cmd"] = currentCmd;
  j["elapsed_ms"] = elapsed;
  j["blocks_ready"] = blocksReady;
  j["do_path"] = doPath.has_value() ? nlohmann::ordered_json(*doPath) : nlohmann::ordered_json(nullptr);
  j["cancel_requested"] = cancelRequested;
  j["stopped_reason"] = stoppedReason.has_value() ? nlohmann::ordered_json(*stoppedReason)
                                                   : nlohmann::ordered_json(nullptr);
  return j;
}

void SessionState::progressStart(int totalLines, std::optional<std::string> doPath) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  progress_ = RunProgress{};
  progress_.running = true;
  progress_.totalLines = totalLines;
  progress_.startedAt = monotonicSeconds();
  progress_.doPath = std::move(doPath);
}

void SessionState::progressUpdate(int currentLine, const std::string& currentCmd, int blocksReady) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  progress_.currentLine = currentLine;
  // Cut at 120 bytes, but never inside a UTF-8 character: a Korean statement
  // cut mid-codepoint is not valid UTF-8, and the snapshot is JSON (BUG-1131).
  size_t cut = currentCmd.size();
  if (cut > 120) {
    cut = 120;
    while (cut > 0 && (static_cast<unsigned char>(currentCmd[cut]) & 0xC0) == 0x80) --cut;
  }
  progress_.currentCmd = currentCmd.substr(0, cut);
  progress_.blocksReady = blocksReady;
}

void SessionState::progressFinish(const std::string& reason, std::optional<int> blocksReady) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  if (progress_.startedAt > 0.0) {
    progress_.elapsedMs = static_cast<long long>((monotonicSeconds() - progress_.startedAt) * 1000);
  }
  progress_.running = false;
  progress_.stoppedReason = reason;
  if (blocksReady.has_value()) progress_.blocksReady = *blocksReady;
}

bool SessionState::progressRequestCancel() {
  {
    std::lock_guard<std::mutex> lock(progressMutex_);
    if (!progress_.running) return false;
    progress_.cancelRequested = true;
  }
  // BUG-087/D9: also wake any _request() blocked in requestUserInput()'s
  // wait — REUSES this same cancelRequested flag (requestUserInput()'s wait
  // predicate calls progressCancelRequested(), same as the do-executor's own
  // between-top-level-items poll does) rather than inventing a second
  // cancellation concept; this notify_all() is the only change needed to
  // make that flag actually WAKE a thread parked in a condition_variable
  // (a pending _request() sits INSIDE one top-level `display` command's
  // evaluation, a granularity the executor's own poll never reaches) instead
  // of only being observed by that poll. Safe to call unconditionally (a
  // no-op notify when nothing is waiting); no lock needed to call it.
  requestCv_.notify_all();
  return true;
}

bool SessionState::progressCancelRequested() const {
  std::lock_guard<std::mutex> lock(progressMutex_);
  // BUG-1068: a cancel belongs to the run it was asked of. The flag outlives
  // that run (the final tick reports it), and read bare it stopped the next
  // block executed on this session without owning a run -- at its first line,
  // "run cancelled" -- until some later run reset it. That happened whenever a
  // cancel landed after the run's last poll, or was the one that stopped it.
  return progress_.running && progress_.cancelRequested;
}

nlohmann::ordered_json SessionState::progressSnapshot() const {
  std::lock_guard<std::mutex> lock(progressMutex_);
  return progress_.asDict();
}

// BUG-087/D9 (BETA Phase F, D9 locked decision — contract_B additive MINOR):
// see session_state.h's doc comment for the full design. Assumes the caller
// (language/display_command.cpp) already checked hasRequestPostEvent() AND
// isCurrentThreadADoWorker() — this method only re-checks the former
// defensively (returns nullopt immediately, same shape as "cancelled", if
// somehow called with no postEvent hook wired).
std::optional<std::string> SessionState::requestUserInput(
    const std::string& macroName, const std::string& prompt,
    const std::vector<std::pair<std::string, std::string>>& choices,
    const nlohmann::ordered_json& extra) {
  if (!requestPostEvent_) return std::nullopt;

  std::string requestId;
  {
    std::lock_guard<std::mutex> lock(requestMutex_);
    requestId = std::to_string(++requestSeq_);
    pendingRequests_[requestId] = std::nullopt;
  }

  // WIRE SHAPE (LOCKED, contract_B additive — CONTRACT.md v2.3.0):
  // {"kind":"request_dialog","requestId":<string>,"prompt":<string>,
  //  "macroName":<string>}. "kind" is the discriminator existing RunProgress
  // ticks never carry (RunProgress::asDict() above has no such field), so
  // the frontend's piet-event listener can tell the two apart unambiguously.
  nlohmann::ordered_json evt;
  evt["kind"] = "request_dialog";
  evt["requestId"] = requestId;
  evt["prompt"] = prompt;
  evt["macroName"] = macroName;
  // SPEC II-4 (2026-09-02, additive): fixed choices + caller extras, only
  // when present, so the `_request()` push is byte-identical to before.
  if (!choices.empty()) {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const auto& c : choices) {
      nlohmann::ordered_json one;
      one["value"] = c.first;
      one["label"] = c.second;
      arr.push_back(one);
    }
    evt["choices"] = arr;
  }
  for (auto it = extra.begin(); it != extra.end(); ++it) evt[it.key()] = it.value();
  // The prompt is the do-file's own text, which can hold a byte that is not
  // UTF-8: it goes out as U+FFFD, as every other block does (BUG-1131).
  requestPostEvent_(evt.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));

  std::unique_lock<std::mutex> lock(requestMutex_);
  bool cancelled = false;
  requestCv_.wait(lock, [&] {
    if (progressCancelRequested()) {
      cancelled = true;
      return true;
    }
    auto it = pendingRequests_.find(requestId);
    // Slot missing (already reaped by a prior wake) counts as "done" too —
    // defensive, should not normally happen (only this method erases its
    // own slot, once, right below).
    return it == pendingRequests_.end() || it->second.has_value();
  });

  std::optional<std::string> result;
  auto it = pendingRequests_.find(requestId);
  if (!cancelled && it != pendingRequests_.end()) result = it->second;
  if (it != pendingRequests_.end()) pendingRequests_.erase(it);
  return result;
}

void SessionState::resolveUserInput(const std::string& requestId, const std::string& value) {
  {
    std::lock_guard<std::mutex> lock(requestMutex_);
    auto it = pendingRequests_.find(requestId);
    if (it != pendingRequests_.end()) it->second = value;
    // Unknown/stale requestId: silent no-op (see this method's header doc
    // comment) — the wire contract's response is {"ok":true} regardless;
    // the caller (main.cpp's request.answer route) doesn't consult our
    // return value at all.
  }
  requestCv_.notify_all();
}

void SessionState::addHistory(const std::string& cmd, bool success) {
  history_.push_back(HistoryEntry{cmd, static_cast<double>(std::time(nullptr)), success});
  // cap at 500 entries (session.py add_history)
  if (history_.size() > 500) {
    history_.erase(history_.begin(), history_.begin() + (history_.size() - 500));
  }
}

nlohmann::ordered_json SessionState::historyJson() const {
  auto out = nlohmann::ordered_json::array();
  for (const auto& h : history_) {
    out.push_back({{"cmd", h.cmd}, {"ts", h.ts}, {"success", h.success}});
  }
  return out;
}

}  // namespace piet
