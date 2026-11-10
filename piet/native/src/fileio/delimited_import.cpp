// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// delimited import.
#include "datamgmt/data_io_internal.h"
#include "piet/handlers/session_commands.h"
#include "piet/user_paths.h"
namespace piet::handlers::data_io_detail {


// Stata's sentence under r(198): every option-grammar refusal below is one.
ojson importRefusal(const std::string& sentence) { return rcErrorBlock(sentence, 198); }


bool abbreviates(const std::string& typed, const char* word, size_t shortest) {
  return typed.size() >= shortest && std::string(word).compare(0, typed.size(), typed) == 0;
}


// A whole number spelled with digits and at most a leading sign. Far too many
// digits saturate rather than fail, so they still read as "a big number".
bool readWholeNumber(const std::string& text, long long* out) {
  const size_t digitsAt = (!text.empty() && (text[0] == '+' || text[0] == '-')) ? 1 : 0;
  if (digitsAt == text.size()) return false;
  if (text.find_first_not_of("0123456789", digitsAt) != std::string::npos) return false;
  *out = std::strtoll(text.c_str(), nullptr, 10);
  return true;
}


std::optional<ojson> readSpan(const std::string& typed, const SpanWording& words,
                              ImportSpan* span) {
  const std::string inOption = std::string(" in ") + words.option + "() option";
  const size_t colon = typed.find(':');
  const std::string head = trim(typed.substr(0, colon));
  const std::string tail =
      colon == std::string::npos ? std::string() : trim(typed.substr(colon + 1));
  if (colon != std::string::npos && head.empty() && tail.empty())
    return importRefusal(words.loneColon);
  if (!head.empty() && (!readWholeNumber(head, &span->first) || span->first < 1))
    return importRefusal(head + " is an invalid start " + words.item + inOption);
  if (tail.empty()) return std::nullopt;

  double end = 0.0;
  if (!delimited::parseNumber(tail, &end))
    return importRefusal(tail + ": invalid end " + words.endItem + inOption);
  if (end < static_cast<double>(std::max(span->first, 1LL)))
    return importRefusal(typed + " is an invalid " + words.item + " range" + inOption + "\nstart " +
                         words.item + " is greater than end " + words.item);
  // A fractional end leaves the stretch open (StataBE: rowrange(2:2.5) reads
  // to the last record).
  if (end == std::floor(end)) span->last = static_cast<long long>(end);
  return std::nullopt;
}


bool readColumnRun(const std::string& word, ColumnPicks::Run* run) {
  const size_t slash = word.find('/');
  const size_t open = word.find('(');
  const size_t close = word.find(')');
  std::string from = word, to = word;
  long long step = 1;
  if (slash != std::string::npos) {
    from = word.substr(0, slash);
    to = word.substr(slash + 1);
  } else if (open != std::string::npos && close != std::string::npos && open < close) {
    from = word.substr(0, open);
    to = word.substr(close + 1);
    if (!readWholeNumber(word.substr(open + 1, close - open - 1), &step) || step == 0) return false;
  }
  if (!readWholeNumber(from, &run->from) || !readWholeNumber(to, &run->to)) return false;
  if (run->from < 1 || run->to < 1) return false;
  // `5/1` counts down; a stated step has to point from its start towards its end.
  const long long reach = run->to - run->from;
  if (slash != std::string::npos && reach < 0) step = -1;
  if (reach != 0 && (reach < 0) != (step < 0)) return false;
  run->step = step;
  return true;
}


std::optional<ojson> readColumnPicks(const std::string& typed, const std::string& option,
                                     ColumnPicks* picks) {
  if (typed == "_all") {
    picks->everyColumn = true;
    return std::nullopt;
  }
  std::string spaced = typed;
  std::replace(spaced.begin(), spaced.end(), ',', ' ');
  std::istringstream words(spaced);
  std::string word;
  bool anyWord = false;
  while (words >> word) {
    ColumnPicks::Run run{};
    if (!readColumnRun(word, &run))
      return importRefusal(typed + ": invalid numlist in " + option + "() option");
    picks->runs.push_back(run);
    anyWord = true;
  }
  if (!anyWord) return importRefusal(typed + ": invalid numlist in " + option + "() option");
  return std::nullopt;
}


// delimiters(): a shortcut word or a quoted set of characters, then at most one
// sub-option after a comma. *named stays false for `""` and for nothing at all,
// which both leave the choice to the reader.
std::optional<ojson> readDelimiters(const std::string& raw, delimited::ReadSpec* reader,
                                    bool* named) {
  const std::string text = raw.substr(std::min(raw.find_first_not_of(" \t"), raw.size()));
  std::string characters;
  std::string afterPart;
  const bool compoundQuoted = text.compare(0, 2, "`\"") == 0;
  if (compoundQuoted || (!text.empty() && text[0] == '"')) {
    const size_t open = compoundQuoted ? 2 : 1;
    const size_t close = compoundQuoted ? text.find("\"'", open) : text.find('"', open);
    if (close == std::string::npos) return importRefusal("unmatched quote");
    characters = text.substr(open, close - open);
    if (characters.find('"') != std::string::npos) return importRefusal("unmatched quote");
    afterPart = text.substr(close + (compoundQuoted ? 2 : 1));
    // backslash-t stands for a tab; a quoted word is its characters, never a shortcut.
    for (size_t at = characters.find("\\t"); at != std::string::npos;
         at = characters.find("\\t", at + 1))
      characters.replace(at, 2, "\t");
  } else {
    const size_t comma = text.find(',');
    const std::string part = text.substr(0, comma);
    if (comma != std::string::npos) afterPart = text.substr(comma);
    static const std::map<std::string, std::string> kShortcuts = {
        {"tab", "\t"}, {"comma", ","}, {"space", " "}, {"whitespace", "\t "}};
    const std::string word = trim(part);
    if (word.empty() && comma != std::string::npos)
      return importRefusal("invalid delimiter() argument");
    if (!word.empty()) {
      const auto shortcut = kShortcuts.find(word);
      if (shortcut == kShortcuts.end())
        return importRefusal(part + ": invalid delimiter() shortcut");
      characters = shortcut->second;
    }
  }

  const std::string rest = trim(afterPart);
  if (!rest.empty() && rest[0] != ',') return importRefusal(rest + ": invalid delimiter() argument");
  const std::string subOption = rest.empty() ? rest : trim(rest.substr(1));
  if (abbreviates(subOption, "collapse", 5)) {
    reader->collapse = true;
  } else if (abbreviates(subOption, "asstring", 5)) {
    reader->asString = true;
  } else if (!subOption.empty()) {
    return importRefusal(subOption + ": invalid delimiter() argument");
  }

  if (!characters.empty()) {
    reader->delims = characters;
    *named = true;
  }
  return std::nullopt;
}


std::optional<ojson> readOneImportOption(const std::string& key, const std::string& raw,
                                         ImportRequest* request) {
  using Bind = delimited::ReadSpec::Bind;
  using Strip = delimited::ReadSpec::Strip;
  const std::string typed = optDequote(raw);
  if (key == "delimiters" || key == "delimiter" || key == "delim")
    return readDelimiters(raw, &request->reader, &request->delimiterNamed);
  if (key == "rowrange") return readSpan(typed, kRecordSpan, &request->records);
  if (key == "colrange") return readSpan(typed, kColumnSpan, &request->columns);
  if (key == "numericcols") return readColumnPicks(typed, key, &request->numericColumns);
  if (key == "stringcols") return readColumnPicks(typed, key, &request->stringColumns);

  if (key == "bindquotes") {
    if (abbreviates(typed, "loose", 1)) request->reader.bind = Bind::kLoose;
    else if (abbreviates(typed, "strict", 1)) request->reader.bind = Bind::kStrict;
    else if (abbreviates(typed, "nobind", 3)) request->reader.bind = Bind::kNoBind;
    else
      return importRefusal(typed + " is an invalid bindquotes() argument; use loose, strict, or nobind");
  } else if (key == "stripquotes") {
    if (abbreviates(typed, "yes", 1)) request->reader.strip = Strip::kYes;
    else if (abbreviates(typed, "no", 1)) request->reader.strip = Strip::kNo;
    else if (abbreviates(typed, "default", 3)) request->reader.strip = Strip::kDefault;
    else
      return importRefusal(typed + " is an invalid stripquotes() argument; use yes, no, or default");
  } else if (key == "maxquotedrows") {
    long long limit = 0;
    if (typed != "unlimited" && !readWholeNumber(typed, &limit))
      return importRefusal("option maxquotedrows() incorrectly specified.");
    request->reader.maxQuotedRows = std::max(limit, 0LL);
  } else if (key == "emptylines") {
    if (typed != "skip" && typed != "include")
      return importRefusal(typed + " is an invalid emptylines() argument; use skip, or include");
    request->reader.skipEmptyLines = typed == "skip";
  } else if (key == "case") {
    if (abbreviates(typed, "lower", 1)) request->nameCase = delimited::Case::kLower;
    else if (abbreviates(typed, "upper", 1)) request->nameCase = delimited::Case::kUpper;
    else if (abbreviates(typed, "preserve", 3)) request->nameCase = delimited::Case::kPreserve;
    else
      return importRefusal(typed + ": invalid argument in option case();\n   use upper, lower, or preserve");
  } else if (key == "varnames") {
    if (abbreviates(typed, "nonames", 3)) {
      request->noNamesRecord = true;
    } else if (readWholeNumber(typed, &request->namesRecord) && request->namesRecord >= 1) {
      request->namesRecordTyped = typed;
    } else {
      return importRefusal(typed + ": invalid row number in varnames() option");
    }
  } else if (key == "encoding" || key == "charset") {
    const std::string folded = strutil::lower(typed);
    if (folded == "utf-8" || folded == "utf8") {
      request->encoding.clear();
    } else {
      const iconv_t probe = iconv_open("UTF-8", typed.c_str());
      if (probe == reinterpret_cast<iconv_t>(-1)) return importRefusal("encoding not supported");
      iconv_close(probe);
      request->encoding = typed;
    }
  }
  return std::nullopt;
}


std::optional<ojson> readImportOptions(const ImportOptionList& opts, ImportRequest* request) {
  for (const auto& [key, value] : opts) {
    if (key == "clear" || key == "collapsedelimiters") continue;  // the latter: accepted, no effect
    if (key == "asdouble" || key == "asfloat") {
      if (!value.isBool) return importRefusal("option " + key + "() incorrectly specified");
      (key == "asdouble" ? request->asDouble : request->asFloat) = true;
      continue;
    }
    if (value.isBool) {
      const bool delimiterSpelling = key == "delimiter" || key == "delim";
      return importRefusal("option " + (delimiterSpelling ? std::string("delimiters") : key) +
                           " incorrectly specified");
    }
    if (auto refusal = readOneImportOption(key, value.strValue, request)) return refusal;
  }
  if (request->numericColumns.everyColumn && request->stringColumns.everyColumn)
    return importRefusal("numericcols(_all) cannot be combined with stringcols(_all)");
  if (request->asDouble && request->asFloat)
    return importRefusal("asfloat() and asdouble() are mutually exclusive options");
  return std::nullopt;
}


// Strict RFC 3629 shape: no overlong forms, no surrogates, nothing past U+10FFFF.
bool holdsOnlyUtf8(const std::string& bytes) {
  static const unsigned long kSmallest[] = {0, 0x80, 0x800, 0x10000};
  for (size_t at = 0; at < bytes.size();) {
    const auto lead = static_cast<unsigned char>(bytes[at]);
    int extra = -1;
    if (lead < 0x80) extra = 0;
    else if ((lead >> 5) == 0x06) extra = 1;
    else if ((lead >> 4) == 0x0E) extra = 2;
    else if ((lead >> 3) == 0x1E) extra = 3;
    if (extra < 0 || at + static_cast<size_t>(extra) >= bytes.size()) return false;
    unsigned long point = extra == 0 ? lead : lead & (0x3Fu >> extra);
    for (int k = 1; k <= extra; ++k) {
      const auto next = static_cast<unsigned char>(bytes[at + static_cast<size_t>(k)]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < kSmallest[extra] || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))
      return false;
    at += static_cast<size_t>(extra) + 1;
  }
  return true;
}


// nullopt when the bytes are not valid in `encoding`.
std::optional<std::string> decodeToUtf8(const std::string& bytes, const std::string& encoding) {
  const iconv_t converter = iconv_open("UTF-8", encoding.c_str());
  if (converter == reinterpret_cast<iconv_t>(-1)) return std::nullopt;
  std::string decoded;
  decoded.reserve(bytes.size() + bytes.size() / 8);
  char* input = const_cast<char*>(bytes.data());
  size_t inputLeft = bytes.size();
  char chunk[16384];
  bool clean = true;
  while (clean && inputLeft > 0) {
    char* output = chunk;
    size_t outputLeft = sizeof chunk;
    const size_t status = iconv(converter, &input, &inputLeft, &output, &outputLeft);
    decoded.append(chunk, sizeof chunk - outputLeft);
    if (status == static_cast<size_t>(-1) && errno != E2BIG) clean = false;
  }
  if (clean) {  // a stateful encoding may still owe its closing bytes
    char* output = chunk;
    size_t outputLeft = sizeof chunk;
    iconv(converter, nullptr, nullptr, &output, &outputLeft);
    decoded.append(chunk, sizeof chunk - outputLeft);
  }
  iconv_close(converter);
  if (!clean) return std::nullopt;
  return decoded;
}


// The file's text as UTF-8; an error block when it cannot be had.
std::optional<ojson> loadImportText(const std::string& path, const std::string& shownName,
                                    const std::string& encoding, std::string* text) {
  std::ifstream file(cecon::to_native_path(path), std::ios::binary);
  if (!file) return rcErrorBlock("file " + shownName + " could not be opened", 603);
  std::ostringstream slurped;
  slurped << file.rdbuf();
  *text = slurped.str();
  if (!encoding.empty() || !holdsOnlyUtf8(*text)) {
    const std::string source = encoding.empty() ? std::string("ISO-8859-1") : encoding;
    auto decoded = decodeToUtf8(*text, source);
    if (!decoded.has_value()) return importRefusal("file " + shownName + " is not valid " + source);
    *text = std::move(*decoded);
  }
  if (text->compare(0, 3, "\xEF\xBB\xBF") == 0) text->erase(0, 3);
  return std::nullopt;
}


// With no varnames(), the first record names the columns when, leaving its
// missing tokens aside, it holds text and no number.
bool recordReadsAsNames(const std::vector<std::string>& record) {
  bool sawText = false;
  for (const std::string& field : record) {
    if (delimited::isMissingToken(field)) continue;
    double unused = 0.0;
    if (delimited::parseNumber(field, &unused)) return false;
    sawText = true;
  }
  return sawText;
}


ojson answerEmptyImport(SessionState& session) {
  ojson cleared = runClear("", session);
  if (cleared.is_object() && cleared.value("type", std::string()) == "error") return cleared;
  ojson answer;
  answer["type"] = "display";
  answer["text"] = "(0 vars, 0 obs)";
  return answer;
}


// The namelist of `import delimited a b using f`: the leading names change and
// nothing else does.
std::optional<ojson> renameLeadingColumns(const std::vector<std::string>& namelist,
                                          std::vector<std::string>* names) {
  if (namelist.empty()) return std::nullopt;
  if (namelist.size() > names->size()) return rcErrorBlock("too many variables specified", 103);
  std::copy(namelist.begin(), namelist.end(), names->begin());
  const std::set<std::string> distinct(names->begin(), names->end());
  if (distinct.size() != names->size()) return importRefusal("could not name variables specified");
  return std::nullopt;
}
}
namespace piet::handlers {
using namespace data_io_detail;


std::string importSourcePath(SessionState& session, const std::string& pathArg, bool excel) {
  const std::string path = resolveUserPath(session, pathArg);
  if (!excel) return withDefaultSuffix(path, ".csv");
  if (pathHasSuffix(path)) return path;
  // A stem names NAME.xlsx, or NAME.xls when only that exists (StataBE 19.5:
  // `import excel using e2` reads e2.xls, `import excel e1` reads e1.xlsx).
  if (!cecon::native_exists(path + ".xlsx") && cecon::native_exists(path + ".xls")) return path + ".xls";
  return path + ".xlsx";
}


// `import delimited`, [D] import delimited. The option layer above settles what
// was asked; delimited_io.h's reader does the parsing, naming and typing.
ojson runImportDelimited(const std::vector<std::string>& varlist, const std::string& pathArg,
                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                         SessionState& session) {
  for (const std::string& word : varlist)
    if (!strutil::isValidStataIdentifier(word))
      return importRefusal(word + ": invalid variable name");

  ImportRequest request;
  if (auto refusal = readImportOptions(opts, &request)) return *refusal;

  const std::string path = importSourcePath(session, pathArg, /*excel=*/false);
  const std::string shownName = typedFileName(pathArg, ".csv");
  if (!cecon::native_exists(path)) return rcErrorBlock("file " + shownName + " not found", 601);

  std::string text;
  if (auto failure = loadImportText(path, shownName, request.encoding, &text)) return *failure;
  if (!request.delimiterNamed)
    request.reader.delims = std::string(1, delimited::detectDelimiter(text));

  const auto records = delimited::parse(text, request.reader);
  text.clear();
  if (records.empty()) return answerEmptyImport(session);

  // Which record names the columns, and which records hold data (both 0-based
  // here). A record at or before the names record is never data.
  const auto recordCount = static_cast<long long>(records.size());
  if (request.namesRecord > recordCount)
    return importRefusal(request.namesRecordTyped + ": invalid row number in varnames() option");
  long long namesAt = -1;
  if (request.namesRecord > 0) namesAt = request.namesRecord - 1;
  else if (!request.noNamesRecord && recordReadsAsNames(records.front())) namesAt = 0;
  const long long dataFrom = std::max(request.records.first - 1, namesAt + 1);
  const long long dataEnd =
      request.records.last > 0 ? std::min(request.records.last, recordCount) : recordCount;

  // The file's width over the records in play, then the colrange() window in it.
  size_t width = namesAt >= 0 ? records[static_cast<size_t>(namesAt)].size() : 0;
  for (long long r = dataFrom; r < dataEnd; ++r)
    width = std::max(width, records[static_cast<size_t>(r)].size());
  const long long firstColumn = std::max(request.columns.first, 1LL);
  const long long lastColumn = request.columns.last > 0
                                   ? std::min(request.columns.last, static_cast<long long>(width))
                                   : static_cast<long long>(width);
  if (firstColumn > lastColumn) return answerEmptyImport(session);
  const auto windowSize = static_cast<size_t>(lastColumn - firstColumn + 1);
  const auto fileColumnOf = [firstColumn](size_t slot) {
    return firstColumn + static_cast<long long>(slot);
  };
  static const std::string kAbsentCell;
  const auto cellOf = [&](const std::vector<std::string>& record,
                          size_t slot) -> const std::string& {
    const auto index = static_cast<size_t>(fileColumnOf(slot) - 1);
    return index < record.size() ? record[index] : kAbsentCell;
  };

  // Names and labels.
  std::vector<std::string> headerCells(windowSize);
  if (namesAt >= 0)
    for (size_t slot = 0; slot < windowSize; ++slot)
      headerCells[slot] = cellOf(records[static_cast<size_t>(namesAt)], slot);
  delimited::NameResult named = delimited::legalNames(headerCells, request.nameCase);
  if (named.clash) return importRefusal("Error creating variables");
  if (auto refusal = renameLeadingColumns(varlist, &named.names)) return *refusal;

  delimited::Table table;
  table.names = std::move(named.names);
  table.labels = namesAt >= 0 ? std::move(named.labels) : std::vector<std::string>(windowSize);

  // Storage types, each from its column's data cells.
  const bool typeAsDouble =
      request.asDouble || (!request.asFloat && session.defaultStorageType == "double");
  std::vector<const std::string*> columnCells;
  for (size_t slot = 0; slot < windowSize; ++slot) {
    columnCells.clear();
    for (long long r = dataFrom; r < dataEnd; ++r)
      columnCells.push_back(&cellOf(records[static_cast<size_t>(r)], slot));
    const bool numericForced = request.numericColumns.holds(fileColumnOf(slot));
    const bool stringForced = !numericForced && request.stringColumns.holds(fileColumnOf(slot));
    table.types.push_back(
        delimited::typeColumn(columnCells, stringForced, numericForced, typeAsDouble));
  }

  for (long long r = dataFrom; r < dataEnd; ++r) {
    std::vector<std::string> row;
    row.reserve(windowSize);
    for (size_t slot = 0; slot < windowSize; ++slot)
      row.push_back(cellOf(records[static_cast<size_t>(r)], slot));
    table.rows.push_back(std::move(row));
  }

  if (auto failure = delimited::stageAndAdopt(session, table, "import delimited", path))
    return *failure;
  return openBlockFor(session, path);
}
}
