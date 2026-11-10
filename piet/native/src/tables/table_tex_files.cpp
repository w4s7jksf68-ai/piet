// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table tex files.
#include "tables_internal.h"
#include "estout_internal.h"
#include "piet/user_paths.h"
namespace piet::handlers::table_detail {


// Portions derived from texsave 1.6.1 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// texsave -- the DATASET as a LaTeX table
// ===========================================================================


// A variable's display format, as `list`/outsheet render it (graph/graph_support.cpp
// displayFormatOf's pattern).
std::string displayFormatOfVar(SessionState& session, Datastore& ds, const std::string& var) {
  if (ds.has(var)) {
    const std::vector<VarInfo> card = ds.formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}
}
namespace piet::handlers {
using namespace table_detail;


ojson runTexsave(const eval::ParsedCommand& parsed, SessionState& session) {
  // W2AK TW-TEXSAVE: derived from texsave 1.6.1 (t/texsave).
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexsave.known,
                                            kVocabTexsave.unimplemented)) {
    return ojson(*err);
  }
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }
  const auto& opts = parsed.opts;
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto sarg = [&](const char* k) {  // `string` options: one pair of outer quotes off
    return str(k).has_value() ? eoStringArg(str(k)) : std::string();
  };
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("using required");
  }
  Datastore* ds = session.datastore();
  // [varlist]: empty is every variable; `*`, `a-b` expand.
  std::vector<std::string> cols;
  if (words.empty()) {
    for (const auto& n : ds->names()) {
      if (!isHiddenColumn(n)) cols.push_back(n);
    }
  } else {
    for (const auto& w : words) {
      auto vr = loops::expandVarlist(w, session);
      if (vr.isError) return ojson(vr.error);
      for (const auto& t : vr.tokens) cols.push_back(t);
    }
  }
  for (const auto& v : cols) {
    if (!ds->has(v)) return tblErrorRc("variable " + v + " not found", 111);
  }
  const bool names = !has("nonames");
  const bool fix = !has("nofix");
  const bool endash = !has("noendash");
  const bool frag = has("frag");
  if (has("varlabels") && !names) {
    return tblErrorRc("option varlabels not allowed with option nonames", 198);
  }
  const std::string delimiter = str("delimiter").has_value() ? sarg("delimiter") : "&";
  const std::string horiz = "\\midrule";
  const std::string rowsep = str("rowsep").has_value() ? " \\addlinespace[" + sarg("rowsep") + "]" : "";
  const std::string headersep = str("headersep").has_value()
                                    ? " \\addlinespace[" + sarg("headersep") + "]"
                                    : " \\addlinespace[\\belowrulesep]";
  std::string userPath = dequote(*usingPath);
  if (userPath.find('.') == std::string::npos) userPath += ".tex";
  std::string marker = sarg("marker");
  if (str("label").has_value()) {
    if (!marker.empty()) return tblErrorRc("Cannot specify both the label and marker options", 198);
    marker = sarg("label");
  }
  std::string location = "tbp";
  if (str("location").has_value()) {
    location.clear();
    for (char c : sarg("location")) {
      if (c == ' ') continue;
      if (std::string("htbpH").find(c) == std::string::npos) {
        return tblErrorRc("location() option contains invalid characters", 198);
      }
      location += c;
    }
  }
  const std::string width = str("width").has_value() ? sarg("width") : "\\linewidth";
  // footnote(text [, size() width() addlinespace()])
  std::string footnote, footnotesize, footnotewidth, footaddls;
  if (auto fo = str("footnote")) {
    std::string lhs, rhs;
    eoSplitComma(*fo, &lhs, &rhs);
    const std::vector<std::string> t = eoTokens(lhs);
    footnote = t.empty() ? std::string() : t[0];
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (nm == "size") footnotesize = eoStringArg(arg);
      else if (nm == "width") footnotewidth = eoStringArg(arg);
      else if (nm == "addlinespace") footaddls = eoStringArg(arg);
      else return tblErrorRc("Invalid syntax for footnote() option", 198);
    }
  }
  std::string footnotespace;
  if (!footnote.empty()) footnotespace = "\\addlinespace[\\belowrulesep]";
  if (!footnote.empty() && !footaddls.empty()) footnotespace = "\\addlinespace[" + footaddls + "]";
  if (footnotewidth.empty()) footnotewidth = width;
  if (footnotesize.empty()) footnotesize = "footnotesize";
  std::string size = sarg("size");
  for (std::string* o : {&size, &footnotesize}) {
    if (o->empty()) continue;
    static const char* kSizes[] = {"tiny",  "scriptsize", "footnotesize", "small", "normalsize",
                                   "large", "Large",      "LARGE",        "huge",  "Huge"};
    bool isInt = true;
    for (char c : *o) {
      if (!std::isdigit(static_cast<unsigned char>(c))) isInt = false;
    }
    if (isInt) {
      const int k = std::stoi(*o);
      if (k < 1 || k > 10) return tblErrorRc("size() value must be between 1 and 10", 198);
      *o = kSizes[k - 1];
    } else {
      bool ok = false;
      for (const char* z : kSizes) {
        if (*o == z) ok = true;
      }
      if (!ok) return tblErrorRc(*o + " is an invalid option for size()", 198);
    }
  }
  const size_t nvars = cols.size();
  std::string align = sarg("align");
  if (align.empty()) {
    align = "@{}l";
    for (size_t i = 2; i <= nvars; ++i) align += "C";
    align += "@{}";
  }
  // ---- header ----------------------------------------------------------------------
  std::string headerHeaderlines;
  if (auto hl = str("headerlines")) {
    for (const auto& t : eoTokens(*hl)) headerHeaderlines += t + " \\tabularnewline ";
  }
  if (auto hl = str("headerlines2")) {
    for (const auto& t : eoTokens(*hl)) headerHeaderlines += t + " ";
  }
  std::string headerAutonumber;
  if (has("autonumber")) {
    for (size_t i = 1; i < nvars; ++i) headerAutonumber += delimiter + "{(" + std::to_string(i) + ")}";
    headerAutonumber += " \\tabularnewline";
    if (names) headerAutonumber += " " + horiz;
  }
  std::string headerColnames;
  if (names) {
    if (has("varlabels")) session.syncEvalVarLabels();
    for (size_t i = 0; i < nvars; ++i) {
      if (i) headerColnames += delimiter;
      const std::string lbl = has("varlabels") ? session.eval.varLabels.get(cols[i]) : cols[i];
      headerColnames += "{" + lbl + "}";
    }
    headerColnames += " \\tabularnewline";
  }
  std::string title = sarg("title");
  auto fixText = [&](std::string s, bool header) {
    for (const char* sym : {"_", "%", "#", "$", "&", "~"}) {
      if (header && std::string(sym) == "&") continue;
      s = eoReplaceAll(s, sym, std::string("\\") + sym);
    }
    if (!header) {
      s = eoReplaceAll(s, "{", "\\{");
      s = eoReplaceAll(s, "}", "\\}");
    }
    return eoReplaceAll(s, "^", "\\^{}");
  };
  if (fix) {
    headerColnames = fixText(headerColnames, true);
    footnote = fixText(footnote, false);
    title = fixText(title, false);
  }
  // ---- the data, outsheet-style (delimiter, nonames, noquote, nolabel) ------------
  const int64_t n = ds->nRows();
  std::vector<std::string> lines;
  {
    std::vector<std::string> fmts;
    for (const auto& c : cols) fmts.push_back(displayFormatOfVar(session, *ds, c));
    static const std::regex reEndash(R"((^|[^A-Za-z\-])-[0-9])");
    for (int64_t r = 0; r < n; ++r) {
      std::string line;
      for (size_t c = 0; c < nvars; ++c) {
        if (c) line += delimiter;
        const cecon::Column& col = ds->col(cols[c]);
        std::string cell;
        if (col.dtype == cecon::DType::Factor) {
          const int32_t code = col.codes()[r];
          if (code >= 0) cell = col.levels[static_cast<size_t>(code)];
          if (fix) cell = fixText(cell, false);
          if (endash) {
            std::smatch m;
            int guard = 0;
            while (std::regex_search(cell, m, reEndash) && guard++ < 1000) {
              const std::string s1 = m[0].str();
              std::string s2 = s1;
              const size_t dash = s2.find('-');
              s2.replace(dash, 1, "--");
              const size_t at = cell.find(s1);
              cell.replace(at, s1.size(), s2);
            }
          }
        } else {
          const double v = col.f64()[r];
          cell = std::isnan(v) ? std::string(".") : eoTrim(eval::applyStataFmt(v, fmts[c]));
        }
        line += cell;
      }
      lines.push_back(line);
    }
  }
  // hlines(): `\midrule ` before the group that follows each listed row
  std::vector<std::string> body;
  {
    std::vector<long long> hl;
    if (auto h = str("hlines")) {
      for (const auto& t : eoTokens(*h)) {
        long long v = 0;
        try {
          v = std::stoll(t);
        } catch (...) {
          return tblErrorRc("invalid numlist", 121);
        }
        if (std::llabs(v) > n) {
          return tblErrorRc("hlines() cannot cannot include values larger than " + std::to_string(n) +
                                ", the size of the table",
                            198);
        }
        if (v < 0) v = n + v;
        hl.push_back(v);
      }
      std::sort(hl.begin(), hl.end());
    }
    std::string pending;
    long long rowAt = 0;
    size_t gi = 0;
    for (size_t g = 0; g <= hl.size(); ++g) {
      const long long hi = g < hl.size() ? hl[g] : n;
      if (g > 0) pending += horiz + " ";
      (void)gi;
      for (; rowAt < hi && rowAt < n; ++rowAt) {
        body.push_back(pending + lines[static_cast<size_t>(rowAt)] + " \\tabularnewline" + rowsep);
        pending.clear();
      }
      if (g < hl.size()) rowAt = std::max(rowAt, hi);
    }
    if (!pending.empty()) body.push_back(pending);  // a trailing empty group
  }
  // ---- assemble (texsave) -----------------------------------------------
  std::vector<std::string> out;
  auto put = [&](const std::string& s) { out.push_back(s); };
  auto putTokens = [&](const char* k) {
    if (auto v = str(k)) {
      for (const auto& t : eoTokens(*v)) put(t);
    }
  };
  if (!has("dataonly")) {
    if (!frag) {
      put("\\documentclass{article}");
      put("\\usepackage{booktabs}");
      put("\\usepackage{tabularx}");
      put("\\usepackage[margin=1in]{geometry}");
      if (has("landscape")) put("\\usepackage{pdflscape}");
    }
    putTokens("preamble");
    if (!frag) {
      put("\\begin{document}");
      put("");
    }
    if (has("landscape")) put("\\begin{landscape}");
    if (str("geometry").has_value()) put("\\newgeometry{" + sarg("geometry") + "}");
    putTokens("headlines");
    if (has("sw")) {
      put("%TCIMACRO{\\TeXButton{B}{\\begin{table}[" + location + "] \\centering}}");
      put("%BeginExpansion");
    }
    put("\\begin{table}[" + location + "] \\centering");
    putTokens("tablelines");
    if (str("rowstretch").has_value()) put("\\renewcommand{\\arraystretch}{" + sarg("rowstretch") + "}");
    if (str("rowheight").has_value()) put("\\setlength\\extrarowheight{" + sarg("rowheight") + "}");
    if (str("colwidth").has_value()) put("\\setlength{\\tabcolsep}{" + sarg("colwidth") + "}");
    put("\\newcolumntype{R}{>{\\raggedleft\\arraybackslash}X}");
    put("\\newcolumntype{L}{>{\\raggedright\\arraybackslash}X}");
    put("\\newcolumntype{C}{>{\\centering\\arraybackslash}X}");
    put("");
    if (has("sw")) put("%EndExpansion");
    if (!title.empty()) put("\\caption{" + title + "}");
    if (!marker.empty()) put("\\label{" + marker + "}");
    if (!size.empty()) put("{\\" + size);
    put("\\begin{tabularx}{" + width + "}{" + align + "}");
    put("");
    put("\\toprule");
    if (!headerAutonumber.empty()) put(headerAutonumber);
    if (!headerHeaderlines.empty()) put(headerHeaderlines);
    if (!headerColnames.empty()) put(headerColnames);
    if (!headerHeaderlines.empty() || !headerAutonumber.empty() || !headerColnames.empty()) {
      put(horiz + headersep);
    }
  }
  for (const auto& b : body) put(b);
  if (!has("dataonly")) {
    put("\\bottomrule " + footnotespace);
    put("");
    put("\\end{tabularx}");
    if (!footnote.empty()) put("\\\\ \\parbox{" + footnotewidth + "}{\\" + footnotesize + " " + footnote + "}");
    if (!size.empty()) put("}");
    if (has("sw")) {
      put("%TCIMACRO{\\TeXButton{E}{\\end{table}}}%");
      put("%BeginExpansion");
    }
    put("\\end{table}");
    if (has("sw")) put("%EndExpansion");
    putTokens("footlines");
    if (str("geometry").has_value()) put("\\restoregeometry");
    if (has("landscape")) put("\\end{landscape}");
    if (!frag) put("\\end{document}");
  }
  std::string bytes;
  for (const auto& l : out) bytes += l + "\n";
  return emitTable("texsave", session, userPath, bytes, has("replace"), false);
}


ojson runTexdoc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexdoc.known,
                                            kVocabTexdoc.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("texdoc: a subcommand is required (init or close)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {"do", "stlog", "local", "graph",
                                                            "write", "substitute", "append"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }

  if (sub == "close") {
    // W2AK TW-TEXDOC: nothing open is a no-op note, rc 0 (texdoc.ado close).
    if (session.eval.globals.get(kTexdocPathKey).empty()) {
      return displayBlock("(texdoc not initialized; nothing to do)");
    }
    const std::string path = session.eval.globals.get(kTexdocPathKey);
    session.eval.globals.erase(kTexdocPathKey);
    return displayBlock("(texdoc closed " + path + ")");
  }
  if (sub != "init") {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  std::vector<std::string> words(parsed.varlist.begin() + 1, parsed.varlist.end());
  // `texdoc init using f.tex` and `texdoc init f.tex` are both written in the
  // wild; `using` is optional here, unlike everywhere else in this file.
  std::optional<std::string> path = takeUsing(words);
  if (!path.has_value() && !words.empty()) path = dequote(words[0]);
  if (!path.has_value() || path->empty()) {
    return tblSyntax("texdoc init: a filename is required");
  }
  // W2AK TW-TEXDOC (texdoc): outside `texdoc do`, init is a no-op
  // unless `force` is given -- no file is written (w2at wt_x03 reads it back
  // r(601)). `force` only bypasses that check; it is not `replace` (wt_x05:
  // an existing file without replace/append is r(602) and stays intact).
  if (!hasFlag(parsed.opts, "force")) {
    return displayBlock("(texdoc do not running; nothing to do)");
  }
  const std::string userPath = ensureExt(*path, ".tex");
  const std::string resolved = resolveUserPath(session, userPath);
  const bool replace = hasFlag(parsed.opts, "replace");
  const bool append = hasFlag(parsed.opts, "append");
  ojson guard = guardOverwrite("texdoc init", resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  if (!append) {
    // TRUNCATE at init, so a `replace` that is never followed by a `tex` line
    // still leaves an empty file rather than the previous run's contents.
    ojson werr = writeTextFileAtomic("texdoc init", resolved, userPath, std::string());
    if (!werr.is_null()) return werr;
  }
  session.eval.globals.set(kTexdocPathKey, resolved);
  return displayBlock("(texdoc opened " + userPath + ")");
}


ojson runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session) {
  // NO reject net here, and that is deliberate rather than an omission: `tex`
  // takes no options at all -- everything after the verb is literal TeX -- so
  // the tokenizer's comma split (`tex Hello, world`) produces a phantom option
  // key from ordinary prose. Reading the whole raw remainder is the only way
  // the text survives, and there is no option vocabulary to be silent about.
  std::string text;
  {
    const std::string& raw = parsed.raw;
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = b;
      while (e < raw.size() &&
             (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
        ++e;
      }
      // Every blank between the verb and the text goes, as in tex.ado's `0':
      // StataBE writes 007's `tex<TAB><TAB><TAB>& (1) ...` (Table_A3.tex) from
      // its `&`, the inner tab stops kept.
      if (e < raw.size()) {
        const size_t t = raw.find_first_not_of(" \t", e);
        if (t != std::string::npos) text = raw.substr(t);
      }
    }
  }
  // A bare `tex` writes a blank line, which is how texdoc users separate
  // paragraphs; it is not an error.

  const std::string resolved = session.eval.globals.get(kTexdocPathKey);
  if (resolved.empty()) {
    // tex: no open document is a note, rc 0 (w2at wt_x07).
    return displayBlock("(texdoc not initialized; nothing to do)");
  }
  bool ok = false;
  std::string existing;
  if (cecon::native_exists(resolved)) {
    existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError("tex: cannot read the open texdoc file", "data_io");
  }
  existing += text;
  existing += "\n";
  // Same temp+rename writer as every other file in this TU: the destination IS
  // the source on every `tex` line after the first.
  ojson werr = writeTextFileAtomic("tex", resolved, "the open texdoc file", existing);
  if (!werr.is_null()) return werr;
  return ojson::array();  // silent success -- texdoc lines produce no output
}
}
