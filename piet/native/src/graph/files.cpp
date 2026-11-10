// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "piet/handlers/graph.h"
#include "piet/user_paths.h"
#include "piet/eval/errors.h"
#include "io/win_compat.h"
#include "graph_support.h"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


// ===========================================================================
// M4b: graph save / combine / export — C++ port of graph_combine.py +
// graph_export.py. SVG geometry constants and tick/scale helpers below
// MUST match graph_export.py's `_W/_H/_MT/_MR/_MB/_ML`/`_nice_ticks`/
// `_fmt_tick`/`_padded_range`/`_sx`/`_sy` byte-for-byte (same source of
// truth graph.js's frontend renderer already uses) — this is the
// "SVG structural" parity the M4 milestone gate gauges debug_all against.



static bool fileExistsG(const std::string& path) { return cecon::native_exists(path); }


// BS-007 (D3): the on-disk read-back half of runGraphSave's (below)
// {"_magic":"PIETGPH1","block":<block>} envelope -- a graph SPEC-JSON
// serializer+loader pair, NEVER a raster/frozen-pixel format (D3 hard-stop:
// growing this beyond serialize+load is out of scope). Returns nullopt for
// any of: file missing/unreadable, not valid JSON, wrong/missing magic, or
// no "block" key -- callers turn that into an honest reject, never a crash.
static std::optional<nlohmann::ordered_json> loadGphFile(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) return std::nullopt;
  // Whole-file slurp: the same `ostringstream << rdbuf()` idiom used
  // throughout this codebase (doexec.cpp/main.cpp/routes.cpp/
  // settings.cpp) rather than istreambuf_iterator, for consistency.
  std::ostringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  nlohmann::ordered_json doc;
  try {
    doc = nlohmann::ordered_json::parse(text);
  } catch (...) {
    return std::nullopt;
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1" || !doc.contains("block")) {
    return std::nullopt;
  }
  return doc["block"];
}



// graph save's confirmation names the file as typed (.gph added when it has no
// suffix), and a new file is noted first; a name that does
// not end in .gph is "saved as .gph format" (StataBE 19.5, BUG-1528/1529:
// `graph save g n2sv2, replace` and `twoway ..., saving(n2sv1, replace)` print
// "(file n2sv2.gph not found)" then "file n2sv2.gph saved", never the absolute
// path; saving(v11.eps, replace) ends "file v11.eps saved as .gph format").
static ojson gphSavedNote(const std::string& typed, bool replace, bool existed) {
  ojson out;
  out["type"] = "display";
  out["class"] = "note";
  out["text"] = (replace && !existed ? "(file " + typed + " not found)\n" : std::string()) + "file " + typed +
                (typed.size() >= 4 && typed.compare(typed.size() - 4, 4, ".gph") == 0 ? " saved"
                                                                                       : " saved as .gph format");
  return out;
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


ojson runGraphSave(const std::optional<std::string>& name, const std::optional<std::string>& filepath,
                    const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  bool replace = optBool(opts, "replace").value_or(false);

  // The named graph, else the current one. graph save never
  // registers a memory graph of its own (w2ai gf_use_059_gd, log:1529-1532).
  std::string gname;
  const ojson* blk = nullptr;
  if (name.has_value() && !name->empty()) {
    auto it = session.namedGraphs.find(*name);
    if (it == session.namedGraphs.end()) return stataRefusal("graph " + *name + " not found", 111);
    blk = &it->second;
    gname = *name;
  } else if (!session.currentGraph.empty() && session.namedGraphs.count(session.currentGraph) != 0) {
    gname = session.currentGraph;
    blk = &session.namedGraphs[gname];
  } else if (session.lastGraph.has_value()) {
    blk = &*session.lastGraph;
    gname = session.sersets.lastOwner();
  }
  if (blk == nullptr) {
    return engineErrorBlockG("graph save: no graph in memory. Run a graph command first.");
  }
  if (!filepath.has_value() || filepath->empty()) return stataRefusal("filename name required", 198);
  // .gph is appended to a file name without a suffix (w2ai gf_save_gp3
  // `graph save gp3 "fC"` writes fC.gph, log:1485-1491) -- a dot ANYWHERE in the
  // name as typed counts as one: StataBE uni-oracle/gs.do saves
  // "../uni-oracle/gsub/fb" and "./gsub/fc" as fb and fc ("saved as .gph
  // format"), as 007's `graph save "$output/.../Figure_A3"` with $output "..".
  std::string fp = resolveUserPath(session, *filepath);
  std::string typed = *filepath;  // what the messages name (BUG-1528)
  if (filepath->find('.') == std::string::npos) {
    fp += ".gph";
    typed += ".gph";
  }
  const bool existed = fileExistsG(fp);
  if (existed && !replace) return stataRefusal("file " + typed + " already exists", 602);
  ojson doc;
  doc["_magic"] = "PIETGPH1";
  doc["block"] = *blk;
  {
    // serset-twoway GST: the graph's sersets travel with it.
    const std::vector<serset::Serset> sets = serset::ownedBy(session, gname);
    if (!sets.empty()) doc["sersets"] = serset::toJson(sets);
    // graph-family AI-4: and so does its describe record.
    auto mt = session.graphMeta.find(gname);
    if (mt != session.graphMeta.end()) doc["meta"] = mt->second;
  }
  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) return stataRefusal("file " + fp + " could not be opened", 603);
  std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return gphSavedNote(typed, replace, existed);
}


// gfx-histogram-kdensity-family: a graph command's own saving(filename[,
// replace]) -- the same {"_magic":"PIETGPH1","block":...} file `graph save`
// writes, under the name as given (Stata appends .gph only to a name without an
// extension: hk_h_031disc saving("hs2.eps") -> hs2.eps, hk_h_069by -> hs1.gph).
// Unlike `graph save` it registers no extra in-memory name. Null on success,
// when `savedNote` receives gr_save's confirmation (BUG-1529).
ojson saveGraphOption(const ojson& block, const std::string& savingText, SessionState& session,
                      const std::vector<serset::Serset>* sersets, ojson* savedNote) {
  std::string body = savingText;
  std::string sub;
  // The file name is the first token, quoted or bare; suboptions follow a comma.
  std::string fname;
  size_t i = body.find_first_not_of(" \t");
  if (i == std::string::npos) return ojson(eval::errorBlock("syntax_error", "saving(): file name required",
                                                            std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  if (body[i] == '"') {
    const size_t close = body.find('"', i + 1);
    fname = body.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
    sub = close == std::string::npos ? std::string() : body.substr(close + 1);
  } else {
    const size_t stop = body.find_first_of(", \t", i);
    fname = body.substr(i, stop == std::string::npos ? std::string::npos : stop - i);
    sub = stop == std::string::npos ? std::string() : body.substr(stop);
  }
  const size_t comma = sub.find(',');
  bool replace = false;
  if (comma != std::string::npos) {
    std::istringstream ss(sub.substr(comma + 1));
    std::string tok;
    while (ss >> tok) {
      if (tok == "replace") {
        replace = true;
      } else if (tok != "asis") {
        return ojson(eval::errorBlock("syntax_error", "saving(): option " + tok + " not allowed", std::nullopt,
                                      std::nullopt, std::nullopt, "r(198)"));
      }
    }
  }
  std::string fp = resolveUserPath(session, fname);
  std::string typed = fname;  // what the messages name (BUG-1529)
  if (fname.find('.') == std::string::npos) {  // as graph save
    fp += ".gph";
    typed += ".gph";
  }
  const bool existed = fileExistsG(fp);
  if (existed && !replace) {
    return ojson(eval::errorBlock("engine_error", "file " + typed + " already exists", std::nullopt, std::nullopt,
                                  std::nullopt, "r(602)"));
  }
  ojson doc;
  doc["_magic"] = "PIETGPH1";
  doc["block"] = block;
  if (sersets != nullptr && !sersets->empty()) doc["sersets"] = serset::toJson(*sersets);
  // graph-family AI-4: the describe record of the graph just registered.
  {
    auto mt = session.graphMeta.find(session.currentGraph);
    if (mt != session.graphMeta.end()) doc["meta"] = mt->second;
  }
  std::ofstream f(cecon::to_native_path(fp), std::ios::binary);
  if (!f) {
    return ojson(eval::errorBlock("engine_error", "file " + fp + " could not be opened", std::nullopt,
                                  std::nullopt, std::nullopt, "r(603)"));
  }
  const std::string text = doc.dump();
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (savedNote != nullptr) *savedNote = gphSavedNote(typed, replace, existed);
  return ojson();
}


ojson runGraphUse(const std::string& fileArg,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  ojson out = ojson::array();
  std::string fname = trimG(fileArg);
  if (fname.size() >= 2 && fname.front() == '"' && fname.back() == '"') fname = fname.substr(1, fname.size() - 2);
  if (fname.empty()) {
    out.push_back(stataRefusal("invalid file specification", 198));
    return out;
  }
  std::string fp = resolveUserPath(session, fname);
  {
    const size_t slash = fp.find_last_of('/');
    const std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
    if (base.find('.') == std::string::npos) fp += ".gph";
  }
  if (!fileExistsG(fp)) {
    out.push_back(stataRefusal("file " + fp + " not found", 601));
    return out;
  }
  std::ifstream f(cecon::to_native_path(fp), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  nlohmann::ordered_json doc;
  try {
    doc = nlohmann::ordered_json::parse(ss.str());
  } catch (...) {
    doc = nlohmann::ordered_json();
  }
  if (!doc.is_object() || doc.value("_magic", std::string()) != "PIETGPH1" || !doc.contains("block")) {
    out.push_back(stataRefusal("file " + fp + " is not a Stata .gph file", 610));
    return out;
  }
  // name(): as given, else the file's base name.
  std::string nameOpt;
  for (const auto& [k, v] : opts) {
    if (k == "name" && !v.isBool) nameOpt = v.strValue;
  }
  if (nameOpt.empty()) {
    const size_t slash = fp.find_last_of('/');
    std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    for (char& c : base)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    nameOpt = base + ", replace";
  }
  if (auto clash = serset::nameCollision(session, nameOpt)) {
    out.push_back(*clash);
    return out;
  }
  ojson block = doc["block"];
  std::vector<serset::Serset> sets = serset::fromJson(doc.value("sersets", ojson::array()));
  // graph-family AI-8: the graph keeps the command it was drawn with (w2ai
  // gf_use_059_d, log:1506-1527).
  const ojson meta = doc.contains("meta") && doc["meta"].is_object() ? doc["meta"]
                                                                       : serset::makeGraphMeta("twoway", "", "");
  serset::registerGraph(session, block, nameOpt, std::move(sets), &meta);
  decimateGraphBlockForView(block);
  out.push_back(block);
  return out;
}


ojson runGraphCombine(const std::vector<std::string>& names,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  ojson panels = ojson::array();
  for (const auto& raw : names) {
    // BS-007 (D3): each combine token resolves as EITHER a bare in-memory
    // graph NAME (session.namedGraphs -- the pre-existing behavior) OR a
    // quoted on-disk `.gph` spec-JSON path. Real-world need: analysis.do's
    // loop-then-combine idiom `graph save` (above)s each iteration's graph
    // to a DISTINCT tempfile while reusing the SAME in-memory name every
    // iteration (a Stata `tempfile` local re-bound per loop pass), so by
    // the time `graph combine` runs, the in-memory name holds only the
    // LAST iteration's graph -- only the on-disk .gph files carry every
    // iteration's distinct graph (analysis.do:1533's `graph combine
    // `graph_list'` where `graph_list` accumulates quoted tempfile paths,
    // not names -- BS-007 evidence). dispatcher.cpp's varlist tokenizer is
    // NOT quote-aware (eval/tokenizer.cpp's splitWs is a bare `>>` split),
    // so a token can arrive as e.g. `"a.gph"` with the quote characters
    // still embedded -- one-layer dequote first (same single-pair-strip
    // rule as fileio/user_paths.cpp's dequoteUserPath, replicated here per this
    // TU's established per-file small-helper convention -- see
    // firstNameToken above) before either lookup.
    std::string tok = raw;
    bool quotedTok = false;
    {
      size_t a = tok.find_first_not_of(" \t");
      size_t b = tok.find_last_not_of(" \t");
      tok = (a == std::string::npos) ? std::string() : tok.substr(a, b - a + 1);
      if (tok.size() >= 2 && ((tok.front() == '"' && tok.back() == '"') ||
                              (tok.front() == '\'' && tok.back() == '\''))) {
        tok = tok.substr(1, tok.size() - 2);
        quotedTok = true;
      }
    }

    auto it = session.namedGraphs.find(tok);
    if (it != session.namedGraphs.end()) {
      panels.push_back(it->second);
      continue;
    }
    // graph-family AI-11: gr_combine reads an unquoted word without a suffix or
    // a path as a memory graph name: `gf_nosuch is not a memory graph`, r(198)
    // (w2ai gf_cmb_absent, log:1817-1823).
    if (!quotedTok && tok.find('.') == std::string::npos && tok.find('/') == std::string::npos &&
        tok.find('\\') == std::string::npos) {
      return stataRefusal(tok + " is not a memory graph", 198);
    }

    std::string fp = resolveUserPath(session, tok);
    {
      const size_t slash = fp.find_last_of('/');
      const std::string base = slash == std::string::npos ? fp : fp.substr(slash + 1);
      if (base.find('.') == std::string::npos) fp += ".gph";  // gs_fileinfo, suffix
    }
    auto loaded = loadGphFile(fp);
    if (!loaded.has_value()) {
      return engineErrorBlockG("graph combine: no graph named '" + tok +
                                "' and no readable .gph file at '" + fp +
                                "' (use `graph save` first).");
    }
    panels.push_back(*loaded);
  }
  if (panels.empty()) {
    return engineErrorBlockG("graph combine: no graph names given.");
  }

  int n = static_cast<int>(panels.size());
  int cols = 0, rows = 0;
  auto colsOpt = optStr(opts, "cols");
  auto rowsOpt = optStr(opts, "rows");
  if (colsOpt.has_value()) {
    try { cols = std::stoi(*colsOpt); } catch (...) { cols = 0; }
  }
  if (rowsOpt.has_value()) {
    try { rows = std::stoi(*rowsOpt); } catch (...) { rows = 0; }
  }
  if (cols <= 0 && rows <= 0) {
    cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  } else if (cols <= 0) {
    cols = static_cast<int>(std::ceil(static_cast<double>(n) / rows));
  } else if (rows <= 0) {
    rows = static_cast<int>(std::ceil(static_cast<double>(n) / cols));
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "graph_combine";
  out["title"] = optTitleStr(opts, "title").value_or(std::string());
  out["subtitle"] = optTitleStr(opts, "subtitle").value_or(std::string());
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["panels"] = panels;
  ojson grid;
  grid["rows"] = rows;
  grid["cols"] = cols;
  out["grid"] = grid;
  out["imargin"] = optStr(opts, "imargin").has_value() ? ojson(*optStr(opts, "imargin")) : ojson(nullptr);
  out["graphregion_color"] = optStr(opts, "graphregion").has_value() ? ojson(*optStr(opts, "graphregion"))
                                                                       : ojson(nullptr);
  out["n_panels"] = n;
  // BS-047 (BETA A4): see runTwowayLayers' identical comment above.
  out["suppressed"] = session.graphicsOff;

  session.lastGraph = out;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value() && !nameOpt->empty()) {
    // BS-057: `name(X, replace)` -- strip the glued `, replace` (or any
    // other suboption) the same way the bare/twoway name() capture does, so
    // the stored key is the bare name "X", not the literal "X, replace"
    // (previously baked verbatim into session.namedGraphs' key).
    std::string nm = firstNameToken(*nameOpt);
    if (!nm.empty()) {
      session.namedGraphs[nm] = out;
      out["name"] = nm;
    } else {
      out["name"] = nullptr;
    }
  } else {
    out["name"] = nullptr;
  }
  return out;
}


ojson runGraphExport(const std::string& filepathArg,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts, SessionState& session) {
  std::string filepath = resolveUserPath(session, filepathArg);

  // W:P8 (BUG-169): `name()` was parsed and then never read, so
  // `graph export FILE, name(X)` ALWAYS wrote session.lastGraph — the current
  // graph, not X. Exporting a name that does not exist returned rc 0 and a
  // file byte-identical to exporting a name that does, which is the cardinal
  // silent-wrong-answer shape: the P8 target's own
  // `graph export "_runs/p8_graph.svg", name(p8all)` would have shipped the
  // last histogram in place of the three-panel combine with every pin green.
  // Stata answers an unknown name with "could not find Graph window", r(693).
  const ojson* blk = nullptr;
  auto nameOpt = optStr(opts, "name");
  if (nameOpt.has_value()) {
    std::string want = *nameOpt;
    size_t a = want.find_first_not_of(" \t");
    size_t b = want.find_last_not_of(" \t");
    want = (a == std::string::npos) ? std::string() : want.substr(a, b - a + 1);
    if (!want.empty()) {
      auto it = session.namedGraphs.find(want);
      if (it == session.namedGraphs.end()) {
        return engineErrorBlockG("graph export: could not find Graph window " + want);
      }
      blk = &it->second;
    }
  }
  if (blk == nullptr && !session.lastGraph.has_value()) {
    return engineErrorBlockG(
        "graph export: no graph in memory. Run a graph command first (e.g. `scatter price mpg`).");
  }
  const ojson& exportBlock = blk != nullptr ? *blk : *session.lastGraph;

  // BUG-103: `dot` used to be `filepath.find_last_of('.')` over the WHOLE
  // path, not just the basename. Under a TMPDIR/workdir containing a dot in
  // some directory component (e.g. a scratch dir literally named
  // `.tmp-scratch`), that scan finds the directory's dot instead of a real
  // filename extension whenever the filename itself is extensionless --
  // `/…/.tmp-scratch/noext_plot` has its ONLY '.' inside `.tmp-scratch`.
  // Two consequences, both silent: (1) extFromName above would read back a
  // directory-path fragment instead of "" ; (2) far worse, the as(png)/pdf
  // fallback path below (`filepath.substr(0, dot) + ".svg"`) TRUNCATES THE
  // PATH at that same wrong dot, producing a garbage sibling path outside
  // the intended directory -- the write silently lands nowhere the caller
  // (or the BS-012 selftest leg) ever looks, i.e. "writes nothing" under a
  // dotted TMPDIR while working fine under `/tmp` (which has no dot in a
  // typical path). Fix: only recognise a '.' that appears AFTER the last
  // path separator (i.e. within the basename) as a real extension.
#ifdef _WIN32
  size_t baseStart = filepath.find_last_of("/\\");
#else
  size_t baseStart = filepath.find_last_of('/');
#endif
  baseStart = (baseStart == std::string::npos) ? 0 : baseStart + 1;
  size_t dot = filepath.find_last_of('.');
  if (dot != std::string::npos && dot < baseStart) dot = std::string::npos;
  std::string extFromName = dot == std::string::npos ? std::string() : filepath.substr(dot + 1);
  std::transform(extFromName.begin(), extFromName.end(), extFromName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // BS-012 (D8): `as(fmt)` names the export format explicitly and wins over
  // the filename's own extension when given (Stata's `graph export FILE,
  // as(fmt)` semantics) -- previously parsed into `opts` by the shared
  // option grammar and then silently never read again.
  std::string fmt;
  auto asOpt = optStr(opts, "as");
  if (asOpt.has_value()) {
    fmt = *asOpt;
    size_t a = fmt.find_first_not_of(" \t");
    size_t b = fmt.find_last_not_of(" \t");
    fmt = (a == std::string::npos) ? std::string() : fmt.substr(a, b - a + 1);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });
  } else {
    fmt = extFromName;
  }

  if (fmt != "png" && fmt != "pdf" && fmt != "svg" && fmt != "eps" && fmt != "ps" && fmt != "jpg" &&
      fmt != "jpeg") {
    return eval::engineError(
        "graph export: unsupported format '" + fmt + "'. Use a .png/.jpg/.pdf/.eps/.svg filename, or `, as(png|jpg|pdf|eps|svg)`.",
        "syntax_error");
  }

  bool replace = optBool(opts, "replace").value_or(false);
  const bool existed = fileExistsG(filepath);
  if (existed && !replace) {
    // gr_export: r(602) `file ... already exists` (w2ai gf_exp_noreplace, log:2005-2011),
    // the file named as typed (StataBE 19.5, BUG-1528: "file ex1.png already exists").
    return stataRefusal("file " + filepathArg + " already exists", 602);
  }
  // BUG-1528: Stata's PNG, JPG and PDF writers name the file by its absolute path
  // and never note a new one; its EPS, PostScript and SVG writers open the file
  // as typed, so a new one under replace is noted first (StataBE 19.5,
  // scratch-laneA2/agents/gfx/g1.do, g2.do: `graph export ex1.png, replace`
  // prints only "file <abs>/ex1.png saved as PNG format"; ex4.eps prints
  // "(file ex4.eps not found)" then "file ex4.eps saved as EPS format", ex5.ps
  // "... saved as PostScript format", ex6.svg "... saved as SVG format").
  auto savedText = [&](const std::string& fmtName) {
    if (fmt != "eps" && fmt != "ps" && fmt != "svg") return "file " + filepath + " saved as " + fmtName + " format";
    return (replace && !existed ? "(file " + filepathArg + " not found)\n" : std::string()) + "file " +
           filepathArg + " saved as " + fmtName + " format";
  };

#ifdef _WIN32
  // Absolute Windows paths use '\\'; find_last_of('/') would miss it and treat
  // the whole path as the parent, so `graph export C:\dir\plot.svg` wrongly
  // reports "directory does not exist". Match either separator. POSIX unchanged.
  auto psep = filepath.find_last_of("/\\");
  std::string parent = (psep == std::string::npos) ? std::string() : filepath.substr(0, psep);
#else
  std::string parent = filepath.substr(0, filepath.find_last_of('/'));
#endif
  if (!parent.empty() && !cecon::native_exists(parent)) {
    return engineErrorBlockG("graph export: directory does not exist: " + parent);
  }

  if (fmt == "svg") {
    std::string svg = buildGraphSvg(exportBlock);
    std::ofstream f(cecon::to_native_path(filepath), std::ios::binary);
    if (!f) return engineErrorBlockG("graph export: could not write file: " + filepath);
    f.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    f.close();
    ojson out;
    out["type"] = "display";
    out["text"] = savedText("SVG");
    return out;
  }

  // graph-family AI-13: png/pdf/eps are written in that format at the path AS
  // GIVEN -- `graph export "x2", as(png)` writes x2, never x2.png (w2ai
  // gf_exp_asonly_f 0 / _png_f 601, log:1987-2001); gf_exp_069 (eps),
  // gf_exp_004 (png, width/height), gf_exp_019 (pdf), log:1922-1983. The
  // drawing is the graph's structural SVG replayed by graph_export_formats.cpp
  // (a real PNG/PDF/EPS, never SVG bytes under another name).
  int widthPx = 0, heightPx = 0;
  if (auto w = optStr(opts, "width")) widthPx = std::atoi(w->c_str());
  if (auto h = optStr(opts, "height")) heightPx = std::atoi(h->c_str());
  std::string bytes;
  const std::string svg = buildGraphSvg(exportBlock);
  if (!graphSvgToFormat(svg, fmt, widthPx, heightPx, &bytes)) {
    return eval::engineError("graph export: unsupported format '" + fmt + "'", "syntax_error");
  }
  std::ofstream f(cecon::to_native_path(filepath), std::ios::binary);
  if (!f) return stataRefusal("file " + filepath + " could not be opened", 603);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();
  // (StataBE 19.5, g1.do/g2.do: `.jpg` and as(jpg) are "JPG", `.ps` "PostScript".)
  const std::string fmtName = fmt == "png"   ? "PNG"
                              : fmt == "pdf" ? "PDF"
                              : fmt == "eps" ? "EPS"
                              : fmt == "ps"  ? "PostScript"
                                             : "JPG";
  ojson out;
  out["type"] = "display";
  out["text"] = savedText(fmtName);
  return out;
}

}  // namespace piet::handlers
