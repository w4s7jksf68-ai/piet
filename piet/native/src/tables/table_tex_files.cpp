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
    return tblErrorRc("using required", 100);
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
  const bool decimalalign = has("decimalalign");
  const bool valuelabels = has("valuelabels");
  // Style options, in the order the ado applies them.
  static const std::vector<std::pair<std::string, std::string>> styleCmds = {
      {"bold", "\\textbf{"},      {"italics", "\\textit{"},   {"underline", "\\underline{"},
      {"slanted", "\\textsl{"},   {"smallcaps", "\\textsc{"}, {"sansserif", "\\textsf{"},
      {"monospace", "\\texttt{"}, {"emphasis", "\\emph{"}};
  std::map<std::string, std::vector<std::string>> styleWords;
  for (const auto& [opt, cmd] : styleCmds) {
    (void)cmd;
    if (auto v = str(opt.c_str())) styleWords[opt] = eoTokens(*v);
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
    for (size_t i = 2; i <= nvars; ++i) align += decimalalign ? "S" : "C";
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
  // texsave's own `if` / `in`: outsheet writes only the selected rows. The
  // qualifier follows `using FILE`; the expression ends where a trailing
  // `using` would begin, as it does for outfile.
  std::unique_ptr<Datastore> filtered;
  {
    std::optional<std::string> ifExpr = parsed.ifExpr;
    if (ifExpr.has_value()) {
      static const std::regex reUsingTail(R"(\s+using\s.*$)");
      ifExpr = std::regex_replace(*ifExpr, reUsingTail, "");
    }
    if ((ifExpr.has_value() && !ifExpr->empty()) ||
        (parsed.inRange.has_value() && !parsed.inRange->empty())) {
      InRangeFilterResult fr;
      try {
        fr = applyIfAndInFilter(session, ds->path(), ifExpr, parsed.inRange);
      } catch (const CobraError& ce) {
        return tblErrorRc(cobraErrorMessage(ce), 198);
      }
      if (!fr.ok) return ojson(inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "texsave"));
      filtered = std::make_unique<Datastore>(fr.dataPath);
      ds = filtered.get();
    }
  }
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
          if (endash && !decimalalign) {
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
          // bold() ... emphasis(): a string cell holding any of the listed words is wrapped
          // in the style's command, one option after the other in the ado's order.
          for (const auto& [opt, cmd] : styleCmds) {
            for (const auto& word : styleWords[opt]) {
              if (!word.empty() && cell.find(word) != std::string::npos) cell = cmd + cell + "}";
            }
          }
          // decimalalign: text that is not a number is braced so siunitx leaves it alone.
          if (decimalalign) {
            char* endp = nullptr;
            const std::string t = eoTrim(cell);
            (void)std::strtod(t.c_str(), &endp);
            if (t.empty() || *endp != '\0') cell = "{" + cell + "}";
          }
        } else {
          const double v = col.f64()[r];
          cell = std::isnan(v) ? std::string(".") : eoTrim(eval::applyStataFmt(v, fmts[c]));
          // valuelabels: a labeled numeric is written as its label (outsheet's default).
          if (valuelabels && !std::isnan(v)) {
            const auto lv = session.varLblname.find(cols[c]);
            if (lv != session.varLblname.end()) {
              const auto ld = session.labelDefs.find(lv->second);
              if (ld != session.labelDefs.end()) {
                auto it = ld->second.find(eval::pythonFloatRepr(v));
                if (it == ld->second.end() && v == std::floor(v)) {
                  it = ld->second.find(std::to_string(static_cast<long long>(v)));
                }
                if (it != ld->second.end()) cell = it->second;
              }
            }
          }
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
      if (decimalalign) put("\\usepackage{siunitx}");
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
  ojson written = emitTable("texsave", session, userPath, bytes, has("replace"), false);
  if (written.is_object() && written.value("type", "") == "error") return written;
  // What the ado's last file command leaves in r(). dataonly copies the
  // filefilter output and stops, so r() is filefilter's own: one occurrence per
  // data row, the 1-byte end-of-line replaced by " \tabularnewline" + rowsep + eol.
  // Otherwise the last command is appendfile's `file read` loop, which ends at
  // end of file and leaves r(eof) = 1 and r(status) = "eof".
  eval::replaceRClass(session.eval);
  if (has("dataonly")) {
    session.eval.rValues.set("occurrences", static_cast<double>(n));
    session.eval.rValues.set("bytes_from", 1.0);
    session.eval.rValues.set("bytes_to", static_cast<double>(std::string(" \\tabularnewline").size() +
                                                              rowsep.size() + 1));
  } else {
    session.eval.rValues.set("eof", 1.0);
    session.eval.rStrings.set("status", "eof");
  }
  return written;
}


namespace {

// The document's settings kept between `texdoc init` and `texdoc close`: one
// global per setting (same cost as the path's `__piet_texdoc_path`).
const std::string kTexdocSettingPrefix = "__piet_texdoc_s_";

// A bare flag option (`logdir`, as against `logdir(x)`).
bool texdocBareFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const char* key) {
  const eval::OptValue* v = findOpt(opts, key);
  return v != nullptr && v->isBool && v->boolValue;
}

// `sreturn clear`: s() is kept in rStrings under the reserved "\x01s:" lead.
void texdocClearSClass(SessionState& session) {
  std::vector<std::string> keys;
  for (const auto& kv : session.eval.rStrings.items())
    if (kv.first.compare(0, 3, "\x01s:") == 0) keys.push_back(kv.first);
  for (const auto& k : keys) session.eval.rStrings.erase(k);
}

}  // namespace


// texdoc's subcommand word as it reads it: `gettoken subcmd 0 : 0, parse(",: ")`,
// each name abbreviable to its own minimum (write 1, _write 2, append 1, local 3,
// substitute 3, init 1, close 1, stlog 1, graph 2; strip and do exactly), and
// the text after it (`0`, the delimiter that ended the word included).
static std::string texdocSubcommand(const std::string& raw, std::string* rest) {
  size_t b = raw.find_first_not_of(" \t");
  size_t e = b == std::string::npos ? raw.size() : raw.find_first_of(" \t", b);
  if (e == std::string::npos) e = raw.size();
  size_t s = raw.find_first_not_of(" \t", e);
  if (s == std::string::npos) {
    rest->clear();
    return std::string();
  }
  size_t t = s;
  if (raw[t] == ',' || raw[t] == ':') {
    ++t;
  } else {
    while (t < raw.size() && raw[t] != ',' && raw[t] != ':' && raw[t] != ' ' && raw[t] != '\t')
      ++t;
  }
  *rest = raw.substr(t);
  const std::string w = raw.substr(s, t - s);
  struct Sub {
    const char* full;
    size_t min;
  };
  static const Sub kSubs[] = {{"write", 1},      {"_write", 2}, {"append", 1}, {"local", 3},
                              {"substitute", 3}, {"init", 1},   {"close", 1},  {"stlog", 1},
                              {"graph", 2}};
  for (const auto& sb : kSubs) {
    const std::string f(sb.full);
    if (w.size() >= sb.min && w.size() <= f.size() && f.compare(0, w.size(), w) == 0) return f;
  }
  return w;
}

// texdoc write / _write / append / local / substitute: what they do outside
// `texdoc do` (texdoc 2.4.0, the version the bank's Stata ran). write puts
// the text after the subcommand word (less the one character that ended the
// word) as a line of the document, _write without the line end; append copies
// a file's lines in, after substitute()'s from/to pairs; local defines the
// local macro (no stlog section to back it up to: "(no stlog available;
// skipping backup)"); substitute keeps the pairs `texdoc do` applies to its
// tex blocks. Each is a note when no document is open (vs-stata bank
// ee2677d0f65929cd, 5739d498655cea3d, bfc4a64a11987e3b, 58f8f00d321b3275).
static ojson texdocAppendText(SessionState& session, const std::string& text, const char* verb) {
  const std::string resolved = session.eval.globals.get(kTexdocPathKey);
  bool ok = false;
  std::string existing;
  if (cecon::native_exists(resolved)) {
    existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError(std::string(verb) + ": cannot read the open texdoc file", "data_io");
  }
  existing += text;
  return writeTextFileAtomic(verb, resolved, "the open texdoc file", existing);
}

ojson runTexdoc(const eval::ParsedCommand& parsed, SessionState& session) {
  {
    std::string rest;
    const std::string sub = texdocSubcommand(parsed.raw, &rest);
    const bool open = !session.eval.globals.get(kTexdocPathKey).empty();
    const ojson notInit = displayBlock("(texdoc not initialized; nothing to do)");
    if (sub == "write" || sub == "_write") {
      if (!open) return notInit;
      const std::string text = rest.empty() ? std::string() : rest.substr(1);
      ojson werr = texdocAppendText(session, sub == "write" ? text + "\n" : text, "texdoc write");
      if (!werr.is_null()) return werr;
      return ojson::array();
    }
    if (sub == "substitute") {
      if (!open) return ojson::array();
      bool add = false;
      std::string body = rest;
      const size_t comma = body.find(',');
      if (comma != std::string::npos) {
        const std::string opt = strutil::lower(strutil::trim(body.substr(comma + 1)));
        if (!opt.empty() && std::string("add").compare(0, opt.size(), opt) != 0)
          return tblErrorRc("option " + opt + " not allowed", 198);
        add = !opt.empty();
        body = body.substr(0, comma);
      }
      body = strutil::trim(body);
      std::istringstream iss(body);
      size_t n = 0;
      for (std::string w; iss >> w;) ++n;
      if (n % 2 == 1) body += " \"\"";
      const std::string key = kTexdocSettingPrefix + std::string("substitute");
      if (add) {
        if (!body.empty()) {
          const std::string s0 = session.eval.globals.get(key);
          session.eval.globals.set(key, (s0.empty() ? "" : s0 + " ") + body);
        }
      } else {
        session.eval.globals.set(key, body);
      }
      return ojson::array();
    }
    if (sub == "append") {
      if (!open) return notInit;
      std::string body = rest;
      std::vector<std::string> subs;
      const size_t comma = body.find(',');
      if (comma != std::string::npos) {
        const std::string opt = strutil::trim(body.substr(comma + 1));
        body = body.substr(0, comma);
        static const std::regex reSub(R"(^sub(?:s(?:t(?:i(?:t(?:u(?:t(?:e)?)?)?)?)?)?)?\((.*)\)$)",
                                      std::regex::icase);
        std::smatch m;
        if (!std::regex_match(opt, m, reSub))
          return tblErrorRc("option " + opt + " not allowed", 198);
        std::istringstream iss(m.str(1));
        for (std::string w; iss >> w;) subs.push_back(dequote(w));
        if (subs.size() % 2 == 1) subs.push_back(std::string());
      }
      const std::string file = dequote(strutil::trim(body));
      if (file.empty()) return tblErrorRc("using required", 100);
      const std::string resolved = resolveUserPath(session, file);
      if (!cecon::native_exists(resolved)) return tblErrorRc("file " + file + " not found", 601);
      bool ok = false;
      std::string content = readWholeFile(resolved, &ok);
      if (!ok) return tblErrorRc("file " + file + " could not be opened", 603);
      std::string out;
      size_t i = 0;
      while (i < content.size()) {
        size_t e = content.find('\n', i);
        std::string line = content.substr(i, (e == std::string::npos ? content.size() : e) - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (size_t k = 0; k + 1 < subs.size(); k += 2) {
          if (subs[k].empty()) continue;
          size_t p = 0;
          while ((p = line.find(subs[k], p)) != std::string::npos) {
            line.replace(p, subs[k].size(), subs[k + 1]);
            p += subs[k + 1].size();
          }
        }
        out += line + "\n";
        if (e == std::string::npos) break;
        i = e + 1;
      }
      ojson werr = texdocAppendText(session, out, "texdoc append");
      if (!werr.is_null()) return werr;
      return displayBlock("(" + file + " appended)");
    }
    if (sub == "local") {
      ojson res = dispatch("local" + rest, session);
      ojson out = ojson::array();
      if (open) out.push_back(displayBlock("(no stlog available; skipping backup)"));
      for (const auto& b : res)
        if (b.value("type", std::string()) != "echo") out.push_back(b);
      return out;
    }
    static const std::set<std::string> kKnown = {"init", "close", "stlog", "graph", "strip", "do"};
    if (!sub.empty() && kKnown.count(sub) == 0)
      return tblErrorRc(sub + " invalid subcommand", 198);
  }
  // The subcommands this build does not run carry options of their own (`texdoc
  // do d.do, cd nostop init()`); they are refused by name before the options
  // are judged against init's.
  if (!parsed.varlist.empty()) {
    static const std::set<std::string> kNotRunSubs = {"do", "stlog", "graph", "strip"};
    const std::string first = strutil::lower(parsed.varlist[0]);
    if (kNotRunSubs.count(first) != 0)
      return ojson(eval::unsupportedCommand("texdoc " + first, std::nullopt, std::nullopt,
                                            "refused:not_implemented_sub",
                                            eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexdoc.known,
                                            kVocabTexdoc.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("texdoc: a subcommand is required (init or close)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {"do", "stlog", "graph", "strip"};
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
    // texdoc close returns the document's settings in s() (sreturn clear first)
    // and shows where the output went.
    const std::string shown =
        session.eval.globals.get(kTexdocSettingPrefix + std::string("docname0"), path);
    texdocClearSClass(session);
    static const char* const kOrder[] = {
        "certify", "custom", "nokeep", "hardcode", "alert", "tag", "verbatim", "cmdlog",
        "beamer", "nooutput", "noltrim", "gtstrip", "lbstrip", "cmdstrip", "matastrip",
        "nolog", "nodo", "linesize", "gropts", "grdir", "stpath", "prefix", "logdir", "path",
        "logall", "basename", "docname"};
    for (const char* k : kOrder) {
      const std::string v = session.eval.globals.get(kTexdocSettingPrefix + std::string(k));
      if (!v.empty()) session.eval.rStrings.set(std::string("\x01s:") + k, v);
    }
    std::vector<std::string> gone;
    for (const auto& kv : session.eval.globals.items())
      if (kv.first.compare(0, kTexdocSettingPrefix.size(), kTexdocSettingPrefix) == 0)
        gone.push_back(kv.first);
    for (const auto& k : gone) session.eval.globals.erase(k);
    return displayBlock("(texdoc output written to " + shown + ")");
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
  // The option rules of texdoc init's syntax line, in the order it checks them.
  {
    auto given = [&](const std::string& k) { return findOpt(parsed.opts, k) != nullptr; };
    auto bad = [&](const std::string& msg) { return tblErrorRc(msg, 198); };
    if (given("replace") && given("append")) return bad("replace and append not both allowed");
    if (given("nolog") && given("cmdlog")) return bad("nolog and cmdlog not both allowed");
    if (given("custom") && given("hardcode")) return bad("custom and hardcode not both allowed");
    for (const char* opt : {"logdir", "prefix", "stpath"})
      if (given(opt) && given(std::string("no") + opt))
        return bad(std::string(opt) + "() and no" + opt + " not both allowed");
    for (const char* opt : {"logall", "do", "log", "cmdlog", "beamer", "verbatim", "hardcode",
                            "keep", "custom", "cmdstrip", "lbstrip", "gtstrip", "matastrip",
                            "output", "ltrim", "certify"})
      if (given(opt) && given(std::string("no") + opt))
        return bad(std::string(opt) + " and no" + opt + " not both allowed");
    if (auto ls = optStr(parsed.opts, "linesize")) {
      // numlist int max=1 >=40 <=255
      const std::string t = dequote(*ls);
      char* endp = nullptr;
      const double v = std::strtod(t.c_str(), &endp);
      if (t.empty() || *endp != '\0' || v != std::floor(v) ||
          t.find_first_of(" \t") != std::string::npos)
        return tblErrorRc("invalid numlist", 121);
      if (v < 40 || v > 255) return tblErrorRc("invalid number, outside of allowed range", 125);
    }
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
  // The document's settings, as texdoc keeps them until `texdoc close` returns
  // them in s(): the file's names and the options that were given, derived the
  // way texdoc's init does (log/graph directories, the prefix of the log files).
  {
    namespace fs = std::filesystem;
    const std::string name = fs::path(userPath).filename().string();
    const std::string dir0 = fs::path(userPath).parent_path().string();
    const std::string stem = fs::path(name).stem().string();
    std::string absDir = fs::path(resolved).parent_path().string();
    if (absDir.empty() || absDir.back() != '/') absDir += "/";
    auto word = [&](const char* k) {
      return findOpt(parsed.opts, k) != nullptr ? std::string(k) : std::string();
    };
    auto arg = [&](const char* k) { return optStr(parsed.opts, k).value_or(std::string()); };
    auto flag = [&](const char* k) { return texdocBareFlag(parsed.opts, k); };
    std::string logdir;
    if (!arg("logdir").empty()) logdir = arg("logdir");
    else if (flag("logdir")) logdir = stem;
    std::string prefix;
    if (flag("noprefix")) prefix = "";
    else if (!arg("prefix").empty()) prefix = arg("prefix");
    else if (flag("prefix")) prefix = stem + "_";
    else if (!logdir.empty()) prefix = "";
    else prefix = stem + "_";
    std::string stpath;
    if (!arg("stpath").empty()) stpath = arg("stpath");
    else if (flag("stpath")) stpath = dir0;
    // texdoc makes the log and graph directories it is told about.
    for (const std::string& sub : {logdir, arg("grdir")}) {
      if (sub.empty()) continue;
      std::error_code ec;
      fs::create_directories(
          cecon::to_native_path(resolveUserPath(session, (dir0.empty() ? "" : dir0 + "/") + sub)),
          ec);
    }
    const std::vector<std::pair<std::string, std::string>> kept = {
        {"certify", word("certify")},   {"custom", word("custom")},     {"nokeep", word("nokeep")},
        {"hardcode", word("hardcode")}, {"alert", arg("alert")},        {"tag", arg("tag")},
        {"verbatim", word("verbatim")}, {"cmdlog", word("cmdlog")},     {"beamer", word("beamer")},
        {"nooutput", word("nooutput")}, {"noltrim", word("noltrim")},   {"gtstrip", word("gtstrip")},
        {"lbstrip", word("lbstrip")},   {"cmdstrip", word("cmdstrip")}, {"matastrip", word("matastrip")},
        {"nolog", word("nolog")},       {"nodo", word("nodo")},         {"linesize", arg("linesize")},
        {"gropts", arg("gropts")},      {"grdir", arg("grdir")},        {"stpath", stpath},
        {"prefix", prefix},             {"logdir", logdir},             {"path", absDir},
        {"logall", word("logall")},     {"basename", name},             {"docname", absDir + name},
        {"docname0", dir0.empty() ? name : dir0 + "/" + name}};
    for (const auto& [k, v] : kept) session.eval.globals.set(kTexdocSettingPrefix + k, v);
  }
  // texdoc init ends with `sreturn clear`.
  texdocClearSClass(session);
  return displayBlock("(texdoc output file is " +
                      session.eval.globals.get(kTexdocSettingPrefix + std::string("docname0")) +
                      ")");
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
