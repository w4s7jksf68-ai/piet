// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Helpers shared by destring and tostring: the skipped-variable note block and
// the generate() column placement right after its source.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/strutil.h"

#include <array>
#include <limits>

#include "datamgmt_support.h"
#include "piet/stata_format.h"  // padMissingLeftJustify (tostring format())
#include "piet/storage_type.h"  // the shared type -> default display format rule
#include "string_conversion_internal.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

namespace string_conversion_detail {
// BUG-664/665: the informational line Stata's destring and tostring print
// when a variable is skipped ("... no replace"). A display block, rc 0 -- the
// command did not fail, it declined one variable and moved on.
ojson destringNoteBlock(const std::string& text) {
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

// BUG-928 (FU-750): generate() places the new variable right after its own
// source, as Stata does (w2ab_strconv_order oracle) -- so `tostring a b, gen(a_s
// b_s)` on a b c leaves a a_s b b_s c (w2ab_strconv_order so_t3_gen_two).
// setcol --new appends the column last; one engine `order --after` (the
// append generate() pattern, BUG-551) moves it, skipped when the source was
// already the last column. `mutated` is the setcol's own mutationResult; its
// varlist_after says where the new column landed, and on a move it is
// replaced by the post-order card (same action and new_vars, final order).
// The order step rides the plane the setcol used: resident when the caller
// held ResidentEligible for setcol (the card fold takes order's S1b permutation
// plan), else the stateless one-shot over the real file setcol wrote. Returns
// an error block on failure, nullopt on success.
std::optional<ojson> placeGeneratedAfterSource(SessionState& session, const std::string& verb,
                                               const std::string& source,
                                               const std::string& target, bool resident,
                                               ojson& mutated) {
  if (mutated.is_object() && mutated.contains("varlist_after") &&
      mutated["varlist_after"].is_array()) {
    const ojson& vl = mutated["varlist_after"];
    for (size_t i = 0; i + 1 < vl.size(); ++i) {
      if (vl[i].value("name", std::string()) == source &&
          vl[i + 1].value("name", std::string()) == target)
        return std::nullopt;  // already right after its source
    }
  }
  std::string ordOut = nextPath(session.workdir(), verb + "_gen_order");
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars",
                                   target,  "--after", source, "--out", ordOut};
  {
    std::optional<SessionState::ResidentEligible> orderResident;
    if (resident) orderResident.emplace(session);
    CobraRunResult ocr = runCobra(args);
    if (!ocr.ok) {
      CobraError ce = errorFromRaw(ocr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  if (!resident && !cecon::native_exists(ordOut))
    return engineErrorBlock(verb + ": failed writing output dataset (disk full or I/O error)");
  session.load(ordOut);
  mutated = mutationResult(session, verb, {target});
  return std::nullopt;
}
}  // namespace string_conversion_detail

}  // namespace piet::handlers
