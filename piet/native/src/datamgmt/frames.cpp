// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// frames — Stata's `frame` family (P6 wave): additional named datasets living
// beside the current one, plus the `frlink`/`frget` link-and-copy pair.
//
// DESIGN — frames are a PIET session concept, and the engine never learns the
// word. Each frame is one hbin inside the session's PID-unique workdir
// (SessionState::frames maps name -> path), and every operation below is
// expressed with engine verbs that already exist:
//
//   frame put VARLIST [if], into(F)     -> cobra eval --keepif ... --drop ...
//   frame F: CMD                        -> swap the session's dataset to F's
//                                          hbin, dispatch(CMD) normally, swap
//                                          back (and keep whatever CMD wrote,
//                                          so `frame F: gen x = 1` persists)
//   frlink m:1 KEY, frame(F) gen(G)     -> cobra merge m:1 against a helper
//                                          built from F carrying a row index
//   frget NEW=VAR, from(G)              -> the same merge, keyed on the link
//
// So there is no new join code here and no new engine surface: the link
// variable IS an m:1 merge result, which means unmatched rows get missing for
// free and key-uniqueness is enforced by the merge the engine already ships.
//
// CURRENT FRAME (vs-stata stage 4, 2026-11-23): the current frame is the
// session's own dataset -- the one every command, and the browse grid, reads.
// Its name is SessionState::currentFrame (c(frame)). `frame change` parks the
// current frame's data and metadata layer under its name and adopts the
// target's; `frame NAME:` is a change there and back. create, change, pwf,
// dir, rename, copy, post, putlabel and `frames describe` are built on those
// two primitives.
#include "piet/handlers/data_combine.h"
#include "piet/handlers/session_commands.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "io/win_compat.h"  // native_exists / native_process_id
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

// Routed through eval::engineError so frames errors carry the same normalized
// code and coarse-rc treatment as every other handler's (an unknown code would
// otherwise skip the rc derivation entirely — the rc comes from the message
// shape, see errors.cpp::coarseRc).
ojson frameError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Same scratch-naming discipline as every other handler file (BUG-062: the
// token carries the PID so two processes sharing a workdir cannot collide).
std::string nextPath(const std::string& workdir, const std::string& prefix) {
  static std::atomic<uint64_t> counter{0};
  static const long long pid = static_cast<long long>(cecon::native_process_id());
  std::string path = workdir + "/" + prefix + "_" + std::to_string(pid) + "_" +
                     std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".hbin";
  registerScratchMint(path);  // BUG-389
  return path;
}

// File-local, like every other handler file's copy — see datamgmt_support.h
// for why the raw-stderr path exists (merge/rename print status text to stderr
// and JSON only on error, so runCobraJson's contract does not fit).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
    // not JSON — fall through to the plain-text form
  }
  return CobraError("engine_error", stderrText.empty() ? "cobra failed" : stderrText);
}

std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// S07-ROUNDTRIP escape hatch (the C1/C2/C4/K1 convention: one env var that
// restores the pre-optimization path verbatim, so the equivalence claim gets a
// real A/B arm inside ONE binary and the merge gets its seeded-RED). Per-call
// getenv on purpose — a selftest leg setenv/unsetenvs around single dispatches,
// which a static cache would ignore.
bool mergeFoldDisabled() {
  const char* e = std::getenv("PIET_NO_MERGE_FOLD");
  return e && *e;
}

// S07-ROUNDTRIP. frlink and frget both expressed their LEFT join as two engine
// round trips: `merge` (all three _merge codes, _merge column generated) then
// `eval --keepif "_merge!=2" --drop _merge` to throw away the half that should
// never have been built. The second trip re-reads and re-writes the WHOLE
// merged frame — measured at 58 ms (frlink) and 63 ms (frget) per call on the
// 48k-row P2PERF fixture, 723 ms of S07's 1.89 s engine wait across its six
// reps — to delete rows and one column the FIRST trip could simply not have
// emitted.
//
// `merge --keep master,matched --nogenerate` is that same left join in one
// trip. Equivalence is structural, not empirical hope (src/data/frame.cpp
// merge_frames): `keep_codes` filters the SAME `plan` vector in the SAME order
// that Frame::keep_if would afterwards, `nogenerate` is literally the branch
// that appends `_merge`, column emission order (keys, master non-keys, using
// non-keys) is unchanged, and keep_if prunes no factor levels — so the two
// paths differ in the number of trips and in nothing else. Held to that claim
// by a byte-comparison leg in selftest.cpp, both arms in one binary.
void appendLeftJoinFold(std::vector<std::string>& args) {
  args.push_back("--keep");
  args.push_back("master,matched");
  args.push_back("--nogenerate");
}

// Stata prints counts with comma grouping ("4,800"). The renderer the rest of
// PIET uses for this lives with the display layer; frames only ever needs the
// integer form, so this is the one small local helper rather than a dependency.
std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

// A workdir path for a frame's own hbin. Frames outlive individual commands, so
// they get their own prefix rather than sharing the per-command scratch names.
std::string framePathFor(SessionState& session, const std::string& name) {
  return nextPath(session.workdir(), "frame_" + name);
}

// `frame drop F` on an unknown F is r(111) in Stata ("frame F not found") —
// the same class an unknown VARIABLE gets, so it carries the same stable code
// the rest of PIET uses for that class (isid's own r(111) path, and every
// other "not found" reject) rather than inventing a second spelling.
// The rc for a missing frame comes from the MESSAGE ("frame NAME not found" ->
// r(111), errors.cpp::coarseRc), which is how every other not-found reject in
// this codebase lands its rc; the code field stays the generic one.
constexpr const char* kNotFoundCode = "engine_error";

ojson rcError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message,
                                std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", "") == "error";
}

// The first error block of a dispatch result (an array of blocks), if any.
std::optional<ojson> firstError(const ojson& blocks) {
  if (isErrorBlock(blocks)) return blocks;
  if (blocks.is_array())
    for (const auto& b : blocks)
      if (isErrorBlock(b)) return b;
  return std::nullopt;
}

// A frame name is a Stata name: a letter or underscore, then letters, digits
// and underscores, at most 32 characters.
bool validFrameName(const std::string& n) {
  if (n.empty() || n.size() > 32) return false;
  if (!(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return true;
}

bool frameExists(const SessionState& session, const std::string& name) {
  return name == session.currentFrame || session.frames.count(name) != 0;
}

// The data-describing layer of the current frame: the same fields `preserve`
// keeps beside its snapshot.
PreserveFrame captureFrameMeta(const SessionState& s) {
  PreserveFrame f;
  f.varMeta = s.varMeta;
  f.implicitFloatVars = s.implicitFloatVars;
  f.labelDefs = s.labelDefs;
  f.varLblname = s.varLblname;
  f.valueLabels = s.valueLabels;
  f.dataLabel = s.dataLabel;
  f.characteristics = s.characteristics;
  f.changed = s.datasetChanged;
  return f;
}

void applyFrameMeta(SessionState& s, const PreserveFrame& f) {
  s.varMeta = f.varMeta;
  s.implicitFloatVars = f.implicitFloatVars;
  s.labelDefs = f.labelDefs;
  s.varLblname = f.varLblname;
  s.valueLabels = f.valueLabels;
  s.dataLabel = f.dataLabel;
  s.characteristics = f.characteristics;
  s.datasetChanged = f.changed;
  s.adoptTsFromChars();
}

// Park the current frame: its data path (materialized first -- a dirty
// virtual head names no file) goes into session.frames under its name, which
// also pins the file against scratch reclaim, and its metadata into frameMeta.
std::optional<ojson> parkCurrentFrame(SessionState& session) {
  std::string path;
  if (session.currentHbin().has_value()) {
    try {
      session.ensureMaterializedHead("frame-change");
      (void)session.datastore();
    } catch (const std::exception& e) {
      return frameError(std::string("frame ") + session.currentFrame +
                        ": could not save the frame's dataset: " + e.what());
    }
    path = *session.currentHbin();
  }
  session.frames[session.currentFrame] = path;
  session.frameMeta[session.currentFrame] = captureFrameMeta(session);
  session.frameFiles[session.currentFrame] = {session.currentFile(),
                                              !session.currentSaveTarget().empty()};
  return std::nullopt;
}

// Make the parked frame `name` current (the current frame must already be
// parked).
void adoptFrame(SessionState& session, const std::string& name) {
  const std::string path = session.frames[name];
  std::optional<PreserveFrame> meta;
  auto mit = session.frameMeta.find(name);
  if (mit != session.frameMeta.end()) {
    meta = std::move(mit->second);
    session.frameMeta.erase(mit);
  }
  session.frames.erase(name);
  std::pair<std::string, bool> file{std::string(), false};
  auto fit = session.frameFiles.find(name);
  if (fit != session.frameFiles.end()) {
    file = fit->second;
    session.frameFiles.erase(fit);
  }
  if (path.empty()) {
    session.clearData();
    session.clearMetadata();
  } else {
    session.load(path);
  }
  session.setCurrentFile(file.first, file.second);
  if (meta.has_value()) {
    applyFrameMeta(session, *meta);
  } else if (!path.empty()) {
    session.clearMetadata();
    if (Datastore* ds = session.datastore()) session.seedVarMetaFromLoadedFile(ds);
    session.datasetChanged = false;
  }
  session.currentFrame = name;
}

// A private copy of a frame's data file (frames never share a path, so a
// later reclaim of one can never pull the other's data away).
std::optional<std::string> copyFrameFile(SessionState& session, const std::string& src,
                                         const std::string& name) {
  if (src.empty()) return std::string();
  const std::string dst = framePathFor(session, name);
  std::error_code ec;
  std::filesystem::copy_file(cecon::to_native_path(src), cecon::to_native_path(dst),
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return std::nullopt;
  return dst;
}

// The (data path, metadata) of any frame, current or parked. The current
// frame's head is materialized first.
std::optional<std::pair<std::string, PreserveFrame>> frameSnapshot(SessionState& session,
                                                                   const std::string& name) {
  if (name == session.currentFrame) {
    std::string path;
    if (session.currentHbin().has_value()) {
      session.ensureMaterializedHead("frame-copy");
      (void)session.datastore();
      path = *session.currentHbin();
    }
    return std::make_pair(path, captureFrameMeta(session));
  }
  auto it = session.frames.find(name);
  if (it == session.frames.end()) return std::nullopt;
  auto mit = session.frameMeta.find(name);
  return std::make_pair(it->second, mit != session.frameMeta.end() ? mit->second : PreserveFrame{});
}

// All frame names, alphabetically (Stata's `frames dir` order).
std::vector<std::string> allFrameNames(const SessionState& session) {
  std::set<std::string> names;
  names.insert(session.currentFrame);
  for (const auto& [n, p] : session.frames) {
    (void)p;
    names.insert(n);
  }
  return std::vector<std::string>(names.begin(), names.end());
}

ojson displayText(const std::string& text) {
  return ojson{{"type", "display"}, {"text", text}};
}

}  // namespace

// ---------------------------------------------------------------------------
// frame put VARLIST [if] [in], into(NAME)
//
// Copies the current frame into the new frame NAME (data and its whole
// metadata layer -- labels, formats, notes, chars), then keeps the wanted rows
// and columns THERE with `keep`, so the varlist (wildcards, ranges), `if` and
// `in` mean exactly what they mean to `keep`. The current dataset is
// untouched. StataBE 19.5: an existing NAME is r(110) "frame NAME already
// defined"; the new frame counts as changed (frames describe r(changed) 1).
ojson runFramePut(const std::vector<std::string>& varlist, const std::optional<std::string>& ifExpr,
                  const std::string& into, SessionState& session,
                  const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (session.datastore() == nullptr) return eval::noDataLoaded();
  if (into.empty()) return rcError("option into() required", 198);
  if (!validFrameName(into)) return rcError(into + " invalid name", 198);
  const bool haveRows = (ifExpr.has_value() && !ifExpr->empty()) ||
                        (inRange.has_value() && !inRange->empty());
  if (varlist.empty() && !haveRows) return rcError("varlist required", 100);
  if (frameExists(session, into)) return rcError("frame " + into + " already defined", 110);

  auto snap = frameSnapshot(session, session.currentFrame);
  if (!snap.has_value()) return eval::noDataLoaded();
  std::optional<std::string> copy = copyFrameFile(session, snap->first, into);
  if (!copy.has_value()) {
    return frameError("frame put: failed writing frame '" + into + "' (disk full or I/O error)");
  }
  session.frames[into] = *copy;
  session.frameMeta[into] = snap->second;

  // Rows first: the `if` may read a column the varlist does not keep.
  std::vector<std::string> cmds;
  if (haveRows) {
    std::string c = "keep";
    if (ifExpr.has_value() && !ifExpr->empty()) c += " if " + *ifExpr;
    if (inRange.has_value() && !inRange->empty()) c += " in " + *inRange;
    cmds.push_back(c);
  }
  if (!varlist.empty()) {
    std::string c = "keep";
    for (const auto& v : varlist) c += " " + v;
    cmds.push_back(c);
  }
  for (const auto& c : cmds) {
    ojson r = runFramePrefix(into, c, session);
    if (std::optional<ojson> err = firstError(r)) {
      (void)runFrameDrop(into, session);
      return *err;
    }
  }
  session.frameMeta[into].changed = true;
  return ojson::array();  // silent success, per the isid/preserve convention
}

// ---------------------------------------------------------------------------
// frame drop NAME
ojson runFrameDrop(const std::string& name, SessionState& session) {
  // StataBE 19.5: the current frame cannot be dropped, r(119).
  if (name == session.currentFrame) return rcError("may not drop current frame", 119);
  auto it = session.frames.find(name);
  if (it == session.frames.end()) {
    return rcError("frame " + name + " not found", 111);
  }
  // Forget the link variables that pointed into it — a link into a dropped
  // frame is dangling, and `frget` through it must not silently answer from a
  // stale path.
  for (auto lit = session.frameLinks.begin(); lit != session.frameLinks.end();) {
    if (lit->second == name) {
      session.frameLinkSpecs.erase(lit->first);
      lit = session.frameLinks.erase(lit);
    } else {
      ++lit;
    }
  }
  const std::string droppedPath = it->second;
  session.frames.erase(it);
  session.frameMeta.erase(name);
  session.frameFiles.erase(name);
  // BUG-389 (2026-09-02): `frame drop` used to forget the map entry and leave the
  // file — no ledger knew it any more, so no reap could ever reach it (the LEAK
  // AUDIT's ORPHAN class). Route it through the ONE reclaim primitive: every pin
  // (live head, preserve twins, use memos, other frames sharing the path, the
  // running command's ScratchPin, the last model's data paths) is honoured there,
  // and the budget decides eager-vs-deferred exactly as for a superseded head.
  if (!droppedPath.empty() && droppedPath != session.currentHbin().value_or(std::string())) {
    session.reclaimScratchFile(droppedPath);
  }
  return ojson::array();  // silent success
}

// ---------------------------------------------------------------------------
// frame NAME: CMD
//
// Runs CMD with NAME as the current frame, then makes the original frame
// current again. Any mutation CMD performed stays with the frame (Stata
// semantics: `frame F: gen x = 1` really does add x to F). Both switches are
// the `frame change` primitives (park the current frame, adopt the target),
// so each frame keeps its own data AND its own labels, formats and notes.
// BUG-396 (2026-09-02) stays closed by construction: parking materializes a
// dirty virtual head first, and a parked frame's file is pinned by
// session.frames against scratch reclaim for as long as it is parked.
ojson runFramePrefix(const std::string& name, const std::string& innerCmd, SessionState& session) {
  ojson inner;
  if (name == session.currentFrame) {
    inner = dispatch(innerCmd, session);
  } else {
    if (session.frames.find(name) == session.frames.end()) {
      return rcError("frame " + name + " not found", 111);
    }
    const std::string home = session.currentFrame;
    if (std::optional<ojson> err = parkCurrentFrame(session)) return *err;
    adoptFrame(session, name);
    try {
      inner = dispatch(innerCmd, session);
    } catch (...) {
      if (!parkCurrentFrame(session).has_value()) adoptFrame(session, home);
      throw;
    }
    if (std::optional<ojson> err = parkCurrentFrame(session)) {
      // The frame's own result could not be kept; go home with the error.
      session.frames[name] = session.frames.count(name) ? session.frames[name] : std::string();
      adoptFrame(session, home);
      return *err;
    }
    adoptFrame(session, home);
  }

  ojson blocks = ojson::array();
  if (!inner.is_array()) {
    blocks.push_back(inner);
    return blocks;
  }
  for (const auto& b : inner) {
    if (b.is_object() && b.value("type", "") == "echo") continue;  // the prefix owns the echo
    blocks.push_back(b);
  }
  return blocks;
}

ojson runWithFrameCurrent(const std::string& name, SessionState& session,
                          const std::function<ojson()>& fn) {
  if (name == session.currentFrame) return fn();
  if (session.frames.find(name) == session.frames.end()) {
    return rcError("frame " + name + " not found", 111);
  }
  const std::string home = session.currentFrame;
  if (std::optional<ojson> err = parkCurrentFrame(session)) return *err;
  adoptFrame(session, name);
  ojson out;
  try {
    out = fn();
  } catch (...) {
    if (!parkCurrentFrame(session).has_value()) adoptFrame(session, home);
    throw;
  }
  if (std::optional<ojson> err = parkCurrentFrame(session)) {
    adoptFrame(session, home);
    return *err;
  }
  adoptFrame(session, home);
  return out;
}

// ---------------------------------------------------------------------------
// frame create NAME [newvarlist]
//
// A new, empty frame; its variables (default type float, or as typed:
// `frame create r str20 name double b`) exist with 0 observations.
ojson runFrameCreate(const std::string& name, const std::vector<std::string>& newvars,
                     SessionState& session) {
  if (!validFrameName(name)) return rcError(name + " invalid name", 198);
  if (frameExists(session, name)) return rcError("frame " + name + " already defined", 110);
  // (type, name) pairs.
  std::vector<std::pair<std::string, std::string>> specs;
  {
    static const std::regex reType(R"(^(byte|int|long|float|double|str[0-9]+|strL)$)");
    std::string pending;
    for (const auto& w : newvars) {
      if (std::regex_match(w, reType)) {
        pending = w;
        continue;
      }
      specs.emplace_back(pending, w);
      pending.clear();
    }
    if (!pending.empty()) return rcError("varlist required", 100);
  }
  const std::string home = session.currentFrame;
  if (std::optional<ojson> err = parkCurrentFrame(session)) return *err;
  session.frames[name] = std::string();
  session.frameMeta[name] = PreserveFrame{};
  adoptFrame(session, name);
  std::optional<ojson> failure;
  if (!specs.empty()) {
    ojson so = runSetObs("0", session);
    failure = firstError(so);
    for (const auto& [type, var] : specs) {
      if (failure.has_value()) break;
      const bool isStr = type.rfind("str", 0) == 0;
      std::string c = "generate " + (type.empty() ? std::string() : type + " ") + var +
                      (isStr ? " = \"\"" : " = .");
      failure = firstError(dispatch(c, session));
    }
  }
  session.datasetChanged = false;
  if (std::optional<ojson> err = parkCurrentFrame(session)) failure = *err;
  adoptFrame(session, home);
  if (failure.has_value()) {
    (void)runFrameDrop(name, session);
    return *failure;
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frame change NAME  (cwf NAME)
ojson runFrameChange(const std::string& name, SessionState& session) {
  if (name == session.currentFrame) return ojson::array();
  if (session.frames.find(name) == session.frames.end()) {
    return rcError("frame " + name + " not found", 111);
  }
  if (std::optional<ojson> err = parkCurrentFrame(session)) return *err;
  adoptFrame(session, name);
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frame pwf: "  (current frame is NAME)", r(currentframe).
ojson runFramePwf(SessionState& session) {
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("currentframe", session.currentFrame);
  eval::declareRPosted(session.eval, "frame pwf", {"currentframe"});
  return displayText("  (current frame is " + session.currentFrame + ")");
}

// ---------------------------------------------------------------------------
// frame dir: one line per frame, alphabetically -- "* " marks a frame with
// unsaved changes -- and r(frames), r(changed) (StataBE 19.5 bank:
// `  default  74 x 12; 1978 automobile data` / `  f2       0 x 0`).
ojson runFrameDir(SessionState& session) {
  const std::vector<std::string> names = allFrameNames(session);
  size_t w = 0;
  for (const auto& n : names) w = std::max(w, n.size());
  std::string text, framesList, changedList;
  bool anyChanged = false;
  for (const auto& n : names) {
    int64_t nObs = 0, nVars = 0;
    std::string label;
    bool changed = false;
    if (n == session.currentFrame) {
      if (session.currentHbin().has_value()) {
        if (Datastore* ds = session.datastore()) {
          nObs = ds->nRows();
          nVars = static_cast<int64_t>(ds->names().size());
        }
      }
      label = session.dataLabel;
      changed = session.datasetChanged;
    } else {
      const std::string& path = session.frames.at(n);
      if (!path.empty()) {
        Datastore ds(path);
        nObs = ds.nRows();
        nVars = static_cast<int64_t>(ds.names().size());
      }
      auto mit = session.frameMeta.find(n);
      if (mit != session.frameMeta.end()) {
        label = mit->second.dataLabel;
        changed = mit->second.changed;
      }
    }
    anyChanged = anyChanged || changed;
    std::string line = std::string(changed ? "* " : "  ") + n + std::string(w - n.size() + 2, ' ') +
                       commaGroup(nObs) + " x " + commaGroup(nVars);
    if (!label.empty()) line += "; " + label;
    if (!text.empty()) text += "\n";
    text += line;
    framesList += (framesList.empty() ? "" : " ") + n;
    changedList += std::string(changedList.empty() ? "" : " ") + (changed ? "1" : "0");
  }
  if (anyChanged) text += "\n\nNote: Frames marked with * contain unsaved data.";
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("frames", framesList);
  session.eval.rStrings.set("changed", changedList);
  eval::declareRPosted(session.eval, "frame dir", {"frames", "changed"});
  return displayText(text);
}

// ---------------------------------------------------------------------------
// frame rename OLD NEW
ojson runFrameRename(const std::string& oldName, const std::string& newName,
                     SessionState& session) {
  if (!frameExists(session, oldName)) return rcError("frame " + oldName + " not found", 111);
  if (!validFrameName(newName)) return rcError(newName + " invalid name", 198);
  if (oldName == newName) return ojson::array();
  if (frameExists(session, newName)) return rcError("frame " + newName + " already defined", 110);
  if (oldName == session.currentFrame) {
    session.currentFrame = newName;
  } else {
    session.frames[newName] = session.frames[oldName];
    session.frames.erase(oldName);
    auto mit = session.frameMeta.find(oldName);
    if (mit != session.frameMeta.end()) {
      session.frameMeta[newName] = std::move(mit->second);
      session.frameMeta.erase(oldName);
    }
    auto fit = session.frameFiles.find(oldName);
    if (fit != session.frameFiles.end()) {
      session.frameFiles[newName] = fit->second;
      session.frameFiles.erase(oldName);
    }
  }
  for (auto& [link, target] : session.frameLinks)
    if (target == oldName) target = newName;
  // A preserved copy belongs to the frame, whatever it is called now.
  for (PreserveFrame& p : session.preserveStack)
    if (p.frameName == oldName) p.frameName = newName;
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frame copy FROM TO [, replace]
ojson runFrameCopy(const std::string& from, const std::string& to, bool replace,
                   SessionState& session) {
  if (!frameExists(session, from)) return rcError("frame " + from + " not found", 111);
  if (!validFrameName(to)) return rcError(to + " invalid name", 198);
  if (from == to) return ojson::array();
  if (frameExists(session, to)) {
    if (!replace) return rcError("frame " + to + " already defined", 110);
    if (to == session.currentFrame) return rcError("may not replace current frame", 119);
  }
  auto snap = frameSnapshot(session, from);
  if (!snap.has_value()) return rcError("frame " + from + " not found", 111);
  std::optional<std::string> copy = copyFrameFile(session, snap->first, to);
  if (!copy.has_value()) {
    return frameError("frame copy: failed writing frame '" + to + "' (disk full or I/O error)");
  }
  if (session.frames.count(to)) (void)runFrameDrop(to, session);
  session.frames[to] = *copy;
  session.frameMeta[to] = snap->second;
  session.frameMeta[to].changed = true;
  // StataBE 19.5 leaves r(currentframe) behind after `frame copy` (bank
  // 07a5fd5a8445ddb9, 3c1cb981bb06942b), the pwf result.
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("currentframe", session.currentFrame);
  eval::declareRPosted(session.eval, "frame copy", {"currentframe"});
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frame post NAME (exp) (exp) ...
//
// One new observation at the end of frame NAME. Each expression is evaluated
// in the CURRENT frame first (as Stata does), then stored in NAME's
// variables in order.
ojson runFramePost(const std::string& name, const std::vector<std::string>& exprs,
                   SessionState& session) {
  if (!frameExists(session, name)) return rcError("frame " + name + " not found", 111);
  std::vector<std::pair<bool, std::string>> values;  // (isString, text)
  for (const auto& e : exprs) {
    bool isStr = false;
    std::string err;
    std::optional<std::string> v = eval::evalArith(e, &session.eval, &err, &isStr);
    if (!v.has_value()) {
      return rcError(err.empty() ? "invalid expression: " + e : err, 198);
    }
    values.emplace_back(isStr, *v);
  }
  auto quoted = [](const std::string& t) { return "`\"" + t + "\"'"; };
  std::vector<std::string> varNames;
  int64_t nObs = 0;
  {
    auto snap = frameSnapshot(session, name);
    if (snap.has_value() && !snap->first.empty()) {
      Datastore ds(snap->first);
      varNames = ds.names();
      nObs = ds.nRows();
    }
  }
  if (varNames.size() != values.size()) {
    return rcError("post: wrong number of expressions; frame " + name + " has " +
                       std::to_string(varNames.size()) + " variables",
                   198);
  }
  ojson r = runFramePrefix(name, "set obs " + std::to_string(nObs + 1), session);
  if (std::optional<ojson> err = firstError(r)) return *err;
  for (size_t i = 0; i < values.size(); ++i) {
    const auto& [isStr, text] = values[i];
    std::string rhs = isStr ? quoted(text) : (text.empty() ? std::string(".") : text);
    ojson rr = runFramePrefix(name, "replace " + varNames[i] + " = " + rhs + " in l", session);
    if (std::optional<ojson> err = firstError(rr)) return *err;
  }
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frame putlabel LBLNAMES, into(NAME)  -- copies value-label definitions.
ojson runFramePutlabel(const std::vector<std::string>& lblnames, const std::string& into,
                       SessionState& session) {
  if (into.empty()) return rcError("option into() required", 198);
  if (!frameExists(session, into)) return rcError("frame " + into + " not found", 111);
  std::vector<std::string> names = lblnames;
  if (names.empty() || (names.size() == 1 && (names[0] == "_all" || names[0] == "*"))) {
    names.clear();
    for (const auto& [n, d] : session.labelDefs) {
      (void)d;
      names.push_back(n);
    }
  }
  for (const auto& n : names)
    if (!session.labelDefs.count(n)) return rcError("value label " + n + " not found", 111);
  if (into == session.currentFrame) return ojson::array();
  PreserveFrame& meta = session.frameMeta[into];
  for (const auto& n : names) meta.labelDefs[n] = session.labelDefs.at(n);
  return ojson::array();
}

// ---------------------------------------------------------------------------
// frames describe [varlist]: `describe` in each frame under a "Frame: NAME"
// rule, then r(frames) r(N) r(k) r(width) r(changed) as lists over the frames
// described. A frame lacking a variable of the list is skipped with a note.
ojson runFramesDescribe(const std::string& args, SessionState& session) {
  const std::string rule(80, '-');
  ojson blocks = ojson::array();
  std::string framesL, nL, kL, widthL, changedL;
  auto num = [](double v) {
    std::ostringstream o;
    o << static_cast<long long>(v);
    return o.str();
  };
  for (const auto& n : allFrameNames(session)) {
    blocks.push_back(displayText(rule + "\nFrame: " + n));
    ojson r = runFramePrefix(n, "describe" + (args.empty() ? std::string() : " " + args), session);
    if (firstError(r).has_value()) {
      blocks.push_back(displayText("    one or more variables not found; skipping frame " + n +
                                   "\n" + rule));
      continue;
    }
    for (const auto& b : r) blocks.push_back(b);
    blocks.push_back(displayText(rule));
    const double* vN = session.eval.rValues.find("N");
    const double* vK = session.eval.rValues.find("k");
    const double* vW = session.eval.rValues.find("width");
    const double* vC = session.eval.rValues.find("changed");
    auto add = [](std::string& l, const std::string& v) { l += (l.empty() ? "" : " ") + v; };
    add(framesL, n);
    add(nL, vN ? num(*vN) : ".");
    add(kL, vK ? num(*vK) : ".");
    add(widthL, vW ? num(*vW) : ".");
    add(changedL, vC ? num(*vC) : ".");
  }
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (!framesL.empty()) {
    session.eval.rStrings.set("frames", framesL);
    session.eval.rStrings.set("N", nL);
    session.eval.rStrings.set("k", kL);
    session.eval.rStrings.set("width", widthL);
    session.eval.rStrings.set("changed", changedL);
    posted = {"frames", "N", "k", "width", "changed"};
  }
  eval::declareRPosted(session.eval, "frames describe", posted);
  return blocks;
}


// ---------------------------------------------------------------------------
// frlink m:1 KEY [KEY2 ...], frame(F) generate(G)
//
// Creates G in the current dataset holding, for each row, the 1-based
// observation number of the matching row in frame F (missing when unmatched).
// Implemented as an m:1 merge against a helper built from F that carries the
// row index and the keys and nothing else — so key uniqueness, missing handling
// and factor-key matching are the engine's existing merge semantics rather than
// a second join implementation with its own bugs.
ojson runFrlink(const std::string& linkType, const std::vector<std::string>& keylist,
                const std::string& frameName, const std::string& genName, SessionState& session,
                const std::vector<std::string>& keys2In, const std::string& cmdText) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();

  if (linkType != "m:1" && linkType != "1:1") {
    return frameError("frlink: only `m:1` and `1:1` links are supported in this build (got `" +
                      linkType + "`)");
  }
  if (keylist.empty()) return frameError("frlink: key variable list required");
  if (frameName.empty()) return frameError("frlink: frame(FRAMENAME) is required");
  if (genName.empty()) return frameError("frlink: generate(NEWVAR) is required");

  auto it = session.frames.find(frameName);
  if (it == session.frames.end()) {
    return frameError("frame " + frameName + " not found", kNotFoundCode);
  }
  if (master->has(genName)) {
    return frameError("variable " + genName + " already defined");
  }
  for (const auto& k : keylist) {
    if (!master->has(k)) return frameError("variable " + k + " not found");
  }

  if (it->second.empty()) return rcError("variable " + keylist[0] + " not found", 111);
  // frame(F VARLIST2): the key variables' names in F, matched by position.
  const std::vector<std::string> keys2 = keys2In.empty() ? keylist : keys2In;
  if (keys2.size() != keylist.size()) {
    return rcError("number of variables in frame() must match the number of key variables", 198);
  }
  Datastore linked(it->second);
  for (const auto& k : keys2) {
    if (!linked.has(k)) {
      return frameError("frlink: key variable '" + k + "' not found in frame " + frameName);
    }
  }

  // An m:1 link requires the key to identify the LINKED frame's observations.
  // The merge underneath would catch a violation anyway, but it would say
  // "merge m:1: using dataset is not unique on the key(s)" — describing a
  // mechanism the user never invoked. Check it here so the message names the
  // command that was actually typed and the frame it was pointed at
  // ([[piet-diagnostic-honesty-classes]]).
  if (linkType == "m:1" || linkType == "1:1") {
    std::set<std::string> seen;
    const int64_t nLinked = linked.nRows();
    std::vector<const cecon::Column*> kc;
    kc.reserve(keylist.size());
    for (const auto& k : keys2) kc.push_back(&linked.col(k));
    for (int64_t r = 0; r < nLinked; ++r) {
      std::string key;
      for (const auto* c : kc) {
        if (c->dtype == cecon::DType::Factor) {
          key += std::to_string(c->codes()[r]);
        } else {
          const double v = c->f64()[r];
          key.append(reinterpret_cast<const char*>(&v), sizeof v);
        }
        key.push_back('\x1f');
      }
      if (!seen.insert(key).second) {
        std::string vars;
        for (size_t i = 0; i < keylist.size(); ++i) {
          if (i) vars += " ";
          vars += keylist[i];
        }
        // Wording deliberately carries "does not uniquely identify the
        // observations" — the exact discriminator errors.cpp::coarseRc uses to
        // land Stata's own r(459) for this class (isid shares it).
        return frameError("variable " + vars +
                          " does not uniquely identify the observations in frame " + frameName);
      }
    }
  }

  // Helper = keys + a 1..N row index named as the link variable. `--egen seq()`
  // is the engine's running-1..N generator (a bare `_n` needs a bysort context).
  const std::string helper = nextPath(session.workdir(), "frlink_helper");
  std::vector<std::string> hargs = {"eval", "--data", it->second, "--egen", genName + " = seq()",
                                    "--out", helper};
  std::set<std::string> keySet(keys2.begin(), keys2.end());
  for (const auto& nm : linked.names()) {
    if (!keySet.count(nm)) {
      hargs.push_back("--drop");
      hargs.push_back(nm);
    }
  }
  CobraRunResult hr = runCobra(hargs);
  if (!hr.ok) {
    CobraError ce = errorFromRaw(hr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  // The helper's keys take the current frame's names, so the merge matches
  // them by name.
  std::string helperPath = helper;
  for (size_t i = 0; i < keys2.size(); ++i) {
    if (keys2[i] == keylist[i]) continue;
    const std::string renamed = nextPath(session.workdir(), "frlink_rename");
    CobraRunResult rr = runCobra({"rename", "--data", helperPath, "--pairs",
                                  keys2[i] + "=" + keylist[i], "--out", renamed});
    if (!rr.ok) {
      CobraError ce = errorFromRaw(rr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
    helperPath = renamed;
  }

  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  // A link is a LEFT join: every master row survives, an unmatched one gets a
  // missing link, and a frame row with no master row must NOT be added. A bare
  // `--keep master` is the WRONG half (Stata's _merge==1 only) and the default
  // keeps using-only rows, so the left join is spelled `--keep master,matched`
  // — with `--nogenerate` because _merge existed only to be filtered on and
  // dropped again. See appendLeftJoinFold: that pair replaces a second
  // whole-frame round trip with nothing at all.
  const bool fold = !mergeFoldDisabled();
  const std::string mergedPath = nextPath(session.workdir(), "frlink_merged");
  const std::string outPath = fold ? mergedPath : nextPath(session.workdir(), "frlink");
  std::vector<std::string> margs = {"merge",  "--data",  *session.currentHbin(),
                                    "--using", helperPath, "--type", linkType,
                                    "--key",   joinedKeys, "--out", mergedPath};
  if (fold) appendLeftJoinFold(margs);
  CobraRunResult cr = runCobra(margs);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  if (!fold) {
    CobraRunResult lr = runCobra({"eval", "--data", mergedPath, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", outPath});
    if (!lr.ok) {
      CobraError ce = errorFromRaw(lr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
  }
  if (!cecon::native_exists(outPath)) {
    return frameError("frlink: failed writing linked dataset");
  }

  session.load(outPath);
  session.frameLinks[genName] = frameName;
  session.frameLinkSpecs[genName] = SessionState::FrameLinkSpec{cmdText, keys2};

  // Stata reports how the link came out. All-matched has its own wording; a
  // partial match names the count that did NOT match, because that is the
  // number the user has to act on.
  Datastore* after = session.datastore();
  int64_t unmatched = 0;
  int64_t total = 0;
  if (after != nullptr && after->has(genName)) {
    total = after->nRows();
    unmatched = after->nMissing(genName);
  }
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("unmatched", static_cast<double>(unmatched));
  eval::declareRPosted(session.eval, "frlink", {"unmatched"});
  const std::string msg =
      unmatched == 0
          ? "(all observations in frame " + session.currentFrame + " matched)"
          : "(" + commaGroup(unmatched) + " of " + commaGroup(total) +
                " observations in frame " + session.currentFrame + " not matched)";
  return ojson{{"type", "display"}, {"text", msg}};
}

// ---------------------------------------------------------------------------
// frget NEWNAME=VAR [NEWNAME2=VAR2 ...], from(LINKVAR)
//
// Copies variables out of the linked frame into the current dataset, matched
// through the link variable frlink created. Same mechanism: an m:1 merge keyed
// on the link, against a helper carrying the row index and the wanted columns.
namespace {
// Stata varlist pattern match over `names` in their order: `*`, `?` and `~`
// wildcards and `_all`; a literal must name a variable.
bool globMatch(const char* p, const char* t) {
  if (*p == '\0') return *t == '\0';
  if (*p == '*' || *p == '~') return globMatch(p + 1, t) || (*t != '\0' && globMatch(p, t + 1));
  if (*t == '\0') return false;
  if (*p == '?' || *p == *t) return globMatch(p + 1, t + 1);
  return false;
}
std::optional<std::string> expandOver(const std::vector<std::string>& tokens,
                                      const std::vector<std::string>& names,
                                      std::vector<std::string>* out) {
  for (const auto& tok : tokens) {
    if (tok == "_all" || tok == "*") {
      for (const auto& n : names)
        if (std::find(out->begin(), out->end(), n) == out->end()) out->push_back(n);
      continue;
    }
    if (tok.find_first_of("*?~") != std::string::npos) {
      bool any = false;
      for (const auto& n : names) {
        if (!globMatch(tok.c_str(), n.c_str())) continue;
        any = true;
        if (std::find(out->begin(), out->end(), n) == out->end()) out->push_back(n);
      }
      if (!any) return tok;
      continue;
    }
    if (std::find(names.begin(), names.end(), tok) == names.end()) return tok;
    if (std::find(out->begin(), out->end(), tok) == out->end()) out->push_back(tok);
  }
  return std::nullopt;
}
}  // namespace

// frget VARLIST, from(LINK) [prefix() suffix() exclude(VARLIST)]
// frget NEWVAR = VAR, from(LINK)
//
// StataBE 19.5 (vs-stata bank): the linked frame's key variables (the ones
// frlink matched on) are never copied -- `frget _all` and `frget make` name
// them in "(variable not copied from linked frame: make)" and r(excluded),
// as does exclude(); r(k) is the number copied, r(newlist) / r(srclist) the
// names in the current / linked frame. Copying nothing is rc 0.
ojson runFrget(const std::vector<std::string>& srcTokens, const std::string& renameTo,
               const std::string& fromLink, const std::string& prefix,
               const std::string& suffix, const std::vector<std::string>& excludeTokens,
               SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();
  if (srcTokens.empty()) return rcError("varlist required", 100);
  if (fromLink.empty()) return rcError("option from() required", 198);

  // from() names a variable first: `frget price2, from(nolink)` is r(111)
  // "variable nolink not found" (vs-stata bank 511f5bc6bc34ee49).
  if (!master->has(fromLink))
    return ojson(eval::errorBlock("engine_error", "variable " + fromLink + " not found",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  auto lit = session.frameLinks.find(fromLink);
  if (lit == session.frameLinks.end()) {
    return frameError("frget: " + fromLink +
                      " is not a linkage variable — create one with `frlink` first");
  }
  auto fit = session.frames.find(lit->second);
  if (fit == session.frames.end()) {
    return frameError("frame " + lit->second + " not found", kNotFoundCode);
  }
  if (fit->second.empty()) return rcError("frame " + lit->second + " has no variables", 111);

  Datastore linked(fit->second);
  const std::vector<std::string> linkedNames = linked.names();
  std::vector<std::string> wanted;
  if (auto bad = expandOver(srcTokens, linkedNames, &wanted))
    return rcError("variable " + *bad + " not found", 111);
  std::vector<std::string> excludeSet;
  {
    auto sit = session.frameLinkSpecs.find(fromLink);
    if (sit != session.frameLinkSpecs.end()) excludeSet = sit->second.keys2;
    std::vector<std::string> ex;
    if (auto bad = expandOver(excludeTokens, linkedNames, &ex))
      return rcError("variable " + *bad + " not found", 111);
    for (const auto& e : ex)
      if (std::find(excludeSet.begin(), excludeSet.end(), e) == excludeSet.end())
        excludeSet.push_back(e);
  }
  std::vector<std::string> excluded;
  std::vector<std::pair<std::string, std::string>> renames;  // (new, src)
  for (const auto& v : wanted) {
    if (std::find(excludeSet.begin(), excludeSet.end(), v) != excludeSet.end()) {
      excluded.push_back(v);
      continue;
    }
    renames.emplace_back(renameTo.empty() ? prefix + v + suffix : renameTo, v);
  }
  for (const auto& [newName, srcName] : renames) {
    (void)srcName;
    if (master->has(newName)) return rcError("variable " + newName + " already defined", 110);
  }

  auto joinNames = [](const auto& v, auto pick) {
    std::string out;
    for (const auto& x : v) out += (out.empty() ? "" : " ") + pick(x);
    return out;
  };
  ojson blocks = ojson::array();
  if (!excluded.empty()) {
    blocks.push_back(ojson{
        {"type", "display"},
        {"text", std::string(excluded.size() == 1 ? "(variable" : "(variables") +
                     " not copied from linked frame: " +
                     joinNames(excluded, [](const std::string& x) { return x; }) + ")"}});
  }
  auto postR = [&]() {
    eval::replaceRClass(session.eval);
    std::vector<std::string> posted = {"k"};
    session.eval.rValues.set("k", static_cast<double>(renames.size()));
    if (!renames.empty()) {
      session.eval.rStrings.set(
          "newlist", joinNames(renames, [](const auto& p) { return p.first; }));
      session.eval.rStrings.set(
          "srclist", joinNames(renames, [](const auto& p) { return p.second; }));
      posted.push_back("newlist");
      posted.push_back("srclist");
    }
    if (!excluded.empty()) {
      session.eval.rStrings.set(
          "excluded", joinNames(excluded, [](const std::string& x) { return x; }));
      posted.push_back("excluded");
    }
    eval::declareRPosted(session.eval, "frget", posted);
  };
  if (renames.empty()) {
    postR();
    blocks.push_back(ojson{{"type", "display"}, {"text", "(0 variables copied from linked frame)"}});
    return blocks;
  }

  // Helper carries the SAME 1..N index the link holds, plus the wanted columns
  // under their new names.
  const std::string helper = nextPath(session.workdir(), "frget_helper");
  std::vector<std::string> hargs = {"eval", "--data", fit->second, "--egen", fromLink + " = seq()",
                                    "--out", helper};
  std::set<std::string> wantedSet;
  for (const auto& [newName, srcName] : renames) wantedSet.insert(srcName);
  for (const auto& nm : linked.names()) {
    if (!wantedSet.count(nm)) {
      hargs.push_back("--drop");
      hargs.push_back(nm);
    }
  }
  CobraRunResult hr = runCobra(hargs);
  if (!hr.ok) {
    CobraError ce = errorFromRaw(hr);
    return frameError(cobraErrorStr(ce), ce.code);
  }

  // Rename each source column to its target name before the join, so the merge
  // never has to reconcile two columns of the same name.
  std::string current = helper;
  for (const auto& [newName, srcName] : renames) {
    if (newName == srcName) continue;
    const std::string renamed = nextPath(session.workdir(), "frget_rename");
    CobraRunResult rr = runCobra(
        {"rename", "--data", current, "--pairs", srcName + "=" + newName, "--out", renamed});
    if (!rr.ok) {
      CobraError ce = errorFromRaw(rr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
    current = renamed;
  }

  // Same left-join shape as frlink (see there, and appendLeftJoinFold): the
  // rows that exist only on the using side are never built rather than built
  // and then filtered away. A master row whose link is missing simply gets
  // missing values for the copied variables, which is Stata's behaviour.
  const bool fold = !mergeFoldDisabled();
  const std::string mergedPath = nextPath(session.workdir(), "frget_merged");
  const std::string outPath = fold ? mergedPath : nextPath(session.workdir(), "frget");
  std::vector<std::string> margs = {"merge",   "--data", *session.currentHbin(),
                                    "--using", current,  "--type", "m:1",
                                    "--key",   fromLink, "--out",  mergedPath};
  if (fold) appendLeftJoinFold(margs);
  CobraRunResult cr = runCobra(margs);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  if (!fold) {
    CobraRunResult lr = runCobra({"eval", "--data", mergedPath, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", outPath});
    if (!lr.ok) {
      CobraError ce = errorFromRaw(lr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
  }
  if (!cecon::native_exists(outPath)) return frameError("frget: failed writing output dataset");

  session.load(outPath);
  postR();
  const std::string noun = renames.size() == 1 ? " variable copied" : " variables copied";
  blocks.push_back(ojson{{"type", "display"},
                         {"text", "(" + std::to_string(renames.size()) + noun +
                                      " from linked frame)"}});
  return blocks;
}

// ---------------------------------------------------------------------------
// frlink dir / frlink describe LINKVAR / frlink rebuild LINKVAR
//
// The link variables of the current frame are the frlink results still
// present in its data. dir: r(vars), r(n_vars), r(n_toonew) (0 -- PIET
// writes no link format a later Stata could not read). describe: the
// command that made the link; r() cleared. rebuild: drop the variable and
// run its frlink again (r(unmatched), as frlink).
ojson runFrlinkSub(const std::string& sub, const std::string& var, SessionState& session) {
  Datastore* ds = session.currentHbin().has_value() ? session.datastore() : nullptr;
  auto isLink = [&](const std::string& v) {
    return ds != nullptr && ds->has(v) && session.frameLinks.count(v) != 0;
  };
  const std::string rule(78, '-');
  if (sub == "dir") {
    std::vector<std::string> vars;
    for (const auto& [v, f] : session.frameLinks) {
      (void)f;
      if (isLink(v)) vars.push_back(v);
    }
    std::string text = "  (" + std::to_string(vars.size()) +
                       (vars.size() == 1 ? " frlink variable found)" : " frlink variables found)");
    std::string list;
    if (!vars.empty()) {
      text += "\n  " + rule;
      for (const auto& v : vars) {
        auto sit = session.frameLinkSpecs.find(v);
        text += "\n  " + v + " created by " +
                (sit != session.frameLinkSpecs.end() ? sit->second.cmdText : std::string("frlink"));
        list += (list.empty() ? "" : " ") + v;
      }
      text += "\n  " + rule +
              "\n  Note: Type \"frlink describe varname\" to find out more, including whether the"
              "\n  variable is still valid.";
    }
    eval::replaceRClass(session.eval);
    session.eval.rStrings.set("vars", list);
    session.eval.rValues.set("n_vars", static_cast<double>(vars.size()));
    session.eval.rValues.set("n_toonew", 0.0);
    eval::declareRPosted(session.eval, "frlink dir", {"vars", "n_vars", "n_toonew"});
    return displayText(text);
  }
  if (var.empty()) return rcError("varname required", 100);
  if (ds == nullptr || !ds->has(var)) return rcError("variable " + var + " not found", 111);
  if (!isLink(var)) return rcError("variable " + var + " is not an frlink variable", 459);
  const SessionState::FrameLinkSpec spec = session.frameLinkSpecs[var];
  if (sub == "describe") {
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "frlink describe", {});
    return displayText("  History:\n  " + rule + "\n    Link variable " + var +
                       " created by\n    . " + spec.cmdText + "\n    Frame " +
                       session.frameLinks[var] + "\n  " + rule);
  }
  // rebuild
  ojson blocks = ojson::array();
  blocks.push_back(displayText("rebuilding variable " + var + "; executing\n" +
                               std::string(80, '-') + "\n-> " + spec.cmdText));
  ojson dropped = dispatch("drop " + var, session);
  if (auto err = firstError(dropped)) return *err;
  session.frameLinks.erase(var);
  ojson r = dispatch(spec.cmdText, session);
  if (auto err = firstError(r)) return *err;
  if (r.is_array()) {
    for (const auto& b : r)
      if (!(b.is_object() && b.value("type", "") == "echo")) blocks.push_back(b);
  }
  blocks.push_back(displayText(std::string(80, '-') + "\nvariable " + var +
                               " successfully rebuilt"));
  return blocks;
}

// ---------------------------------------------------------------------------
// frames reset
//
// Routed through runFrameDrop rather than clearing session.frames directly:
// that handler owns the dangling-link cleanup AND the scratch reclaim (BUG-389
// -- a forgotten map entry leaves a file no ledger knows, which is the LEAK
// AUDIT's ORPHAN class). Duplicating the teardown here would regrow it.
nlohmann::ordered_json runFramesReset(SessionState& session) {
  // Names collected FIRST: runFrameDrop erases from session.frames, so
  // iterating the map while dropping would invalidate the iterator mid-loop.
  // Every parked frame goes, `default` included when another frame is
  // current; the current frame is the session's own dataset, emptied below
  // and renamed `default`.
  std::vector<std::string> names;
  for (const auto& [name, path] : session.frames) {
    (void)path;
    if (name == session.currentFrame) continue;  // the session itself
    names.push_back(name);
  }
  ojson lastError = ojson::array();
  for (const auto& name : names) {
    // Keep going after a failure: stopping on the first one would leave the
    // rest of the frames alive AND their scratch files unreclaimed, which is
    // the opposite of what `frames reset` was asked to do.
    ojson r = runFrameDrop(name, session);
    if (r.is_object() && r.value("type", "") == "error") lastError = r;
  }
  if (!lastError.is_object()) {
    // [D] frames reset: "a single, empty frame named default" -- the default
    // frame's data go too (vs-stata bank: `frames reset` and `clear frames`
    // after `use auto` leave N 0, k 0).
    ojson cleared = runClear("", session);
    if (cleared.is_object() && cleared.value("type", "") == "error") return cleared;
    session.currentFrame = "default";
  }
  return lastError;
}

}  // namespace handlers
}  // namespace piet
