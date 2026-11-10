// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "graphics_support.h"
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {


// BS-011 (BETA B-graph-B): coefplot/marginsplot are graph-family results
// but their dispatch branches (below) return immediately after computing
// the block, never reaching the big scatter/histogram/twoway/... block's
// own tail that caches `session.lastGraph` and captures a bare `name()`
// into `session.namedGraphs`. Without that, `graph export`/`graph save`/
// `graph combine` unconditionally failed with "no graph in memory"
// immediately after a coefplot/marginsplot (BS-011 evidence). This is a
// literal mirror of that existing tail (same name-capture regex/logic,
// copied — not a refactor of the scatter/twoway block's own copy, and not
// a new shared abstraction either; unifying all call sites onto one
// consumer is BS-099's Phase E job, out of this stint's scope).
// graph-family AI-4: the describe record of the graph a statement draws:
// r(command) is the statement after a leading `graph` word as typed (w2ai
// gf_bar_mean_dc `bar (mean) y, over(g) nodraw name(gf_a1, replace)`, log:236;
// gf_tw_plain_dc `twoway scatter y x, ...`, :1313); a bare twoway plot command
// is `twoway <command>`. r(scheme) is scheme(), else stcolor.
ojson graphMetaFor(const std::string& raw, const std::string& family,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const std::string r = strutil::trim(raw);
  size_t e = 0;
  while (e < r.size() && !std::isspace(static_cast<unsigned char>(r[e])) && r[e] != ',') ++e;
  const std::string w0 = strutil::lower(r.substr(0, e));
  std::string command;
  if (w0 == "gr" || w0 == "gra" || w0 == "grap" || w0 == "graph") {
    command = strutil::trim(r.substr(e));
  } else if (w0 == "scatter" || w0 == "sc" || w0 == "line" || w0 == "connected" || w0 == "tsline") {
    command = "twoway " + r;
  } else {
    command = r;
  }
  std::string scheme;
  for (const auto& [k, v] : opts) {
    if ((k == "scheme" || k == "sch" || k == "sche" || k == "schem") && !v.isBool) {
      std::string t = strutil::trim(v.strValue);
      const size_t sp = t.find_first_of(" ,");
      scheme = sp == std::string::npos ? t : t.substr(0, sp);
    }
  }
  return serset::makeGraphMeta(family, command, scheme);
}


void registerGraphOutput(ojson& result, SessionState& session,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          const std::string& raw ) {
  if (!(result.is_object() && result.value("type", std::string()) == "graph")) return;
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  // serset-twoway SS: the one registration (piet/serset.h). These graphs make no
  // sersets of their own yet; an unnamed one is "Graph", as in Stata.
  const ojson meta = graphMetaFor(raw, "twoway", opts);
  serset::registerGraph(session, result, nameOpt, {}, &meta);
}


// serset-twoway SS: the last name() a graph command was given ("" = none).
std::string graphNameOption(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  return nameOpt;
}


// BS-053: `twoway scatter y x || lfit y x` -- Stata's `||` double-pipe
// layer separator, an alternate spelling of the parenthesised
// `twoway (scatter y x) (lfit y x)` form (same layer semantics, just
// without the parens). twoway_parse.cpp's parser only ever recognises
// TOP-LEVEL `(...)` groups as layer boundaries; a bare `||`-separated
// string has none, so it fell into the "no top-level parens" bare
// single-plot branch, which only ever looks at the FIRST plot-type token
// and silently drops everything from the first `||` onward (SCOPE's
// "silently truncates to first plot").
//
// Fix strategy: don't touch twoway_parse.cpp's parenthesised-layer grammar
// at all -- instead, when the raw twoway argument string has NO top-level
// parens but DOES have a top-level `||`, wrap each `||`-delimited segment
// in its own parens (`"scatter y x || lfit y x"` -> `"(scatter y x) (lfit
// y x)"`) and hand the synthesized string to the EXISTING, already-correct
// parenthesised-layer parser unchanged. `||` occurrences inside a quoted
// string (e.g. a `title("a||b")`) or inside a nested `(...)` option value
// are never split, mirroring twoway_parse.cpp's own paren/quote-depth
// scanning convention (scanToplevelParens) so the two stay consistent in
// spirit without sharing code.
std::string expandDoublePipeLayers(const std::string& rawArgs) {
  // serset-twoway HIST-5: the one expander lives in twoway_parse.cpp so
  // histogram's addplot() reads its `||` chain the same way.
  return twoway::expandDoublePipes(rawArgs);
}


// serset-twoway PARA: the twoway-level options (twoway_options) a plot's own
// option list may carry -- they apply to the whole graph (`plot1, opts || plot2,
// opts name(g)` names the graph: w2al tf_conn_082_gd `tf_b15`, log:1374-1414).
const std::unordered_set<std::string> kTwowayLevelOptions = {
    "name",      "saving",     "nodraw",    "draw",      "title",     "subtitle",  "note",
    "caption",   "xtitle",     "ytitle",    "ttitle",    "xlabel",    "ylabel",    "tlabel",
    "xmlabel",   "ymlabel",    "tmlabel",   "xtick",     "ytick",     "ttick",     "xmtick",
    "ymtick",    "tmtick",     "xscale",    "yscale",    "tscale",    "xline",     "yline",
    "tline",     "legend",     "scheme",    "graphregion", "plotregion", "xsize", "ysize",
    "fxsize",    "fysize",     "aspectratio", "scale",   "text",      "ttext",     "by",
    "iscale",    "xcommon",    "ycommon",   "b1title",   "b2title",   "l1title",   "l2title",
    "r1title",   "r2title",    "t1title",   "t2title",   "xoverhangs", "yoverhangs", "play"};


// Moves the twoway-level options out of each layer's own list into the graph's
// (layer order; the graph's own block after them).
void hoistTwowayLevelOptions(twoway::ParseResult& pr) {
  std::vector<std::pair<std::string, eval::OptValue>> hoisted;
  for (auto& l : pr.layers) {
    std::vector<std::pair<std::string, eval::OptValue>> keep;
    for (auto& kv : l.opts) {
      if (kTwowayLevelOptions.count(kv.first) != 0) {
        hoisted.push_back(kv);
      } else {
        keep.push_back(kv);
      }
    }
    l.opts = std::move(keep);
  }
  if (hoisted.empty()) return;
  for (auto& kv : pr.globalOpts) hoisted.push_back(kv);
  pr.globalOpts = std::move(hoisted);
}


// W4, "honesty over silence". A graph arm pushes exactly ONE block, so a note
// cannot be a sibling display block; it is appended to the block's own `note`
// string, which every graph renderer already draws (graph/commands.cpp gives every
// bare block the key, and graph/graph_extension_support.cpp does the same thing for
// grc1leg/event_plot -- this is that helper, at the dispatcher).
//
// `applied` is the set of keys the path that BUILT this block actually read, so
// the note is a statement about the code that ran, not about a hand-maintained
// list: re-routing a `scatter ... , xline(0)` into the layer renderer moves
// `xline` from the noted set to the applied one automatically, because the two
// call sites pass different `applied` sets. Keys the user did not type never
// appear, and a block with nothing to report is left byte-identical -- which is
// what keeps the freeze fixtures (all of them bare commands, no options) pinned.
void graphAcceptedNotApplied(nlohmann::ordered_json& block, const std::string& cmdName,
                             const std::vector<std::pair<std::string, piet::eval::OptValue>>& opts,
                             const std::unordered_set<std::string>& applied) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;
  std::vector<std::string> noted;
  for (const auto& kv : opts) {
    if (applied.count(kv.first) != 0) continue;
    if (std::find(noted.begin(), noted.end(), kv.first) != noted.end()) continue;
    noted.push_back(kv.first);
  }
  if (noted.empty()) return;
  std::string joined;
  for (size_t i = 0; i < noted.size(); ++i) {
    if (i != 0) joined += ", ";
    joined += noted[i];
  }
  const std::string text = "(" + cmdName + ": " + joined + " accepted and not applied — this "
                           "build draws the plot without " +
                           (noted.size() == 1 ? std::string("it") : std::string("them")) + ".)";
  std::string cur;
  if (block.contains("note") && block["note"].is_string()) cur = block["note"].get<std::string>();
  block["note"] = cur.empty() ? text : (cur + "  " + text);
}
}  // namespace piet::dispatch_detail
