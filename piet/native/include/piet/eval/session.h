// EvalSession — the M1 subset of the Python SessionState (session.py),
// carrying exactly the state the macro layer reads/writes: local/global
// macros, scalars, r()/e() stores, matrices, variable labels, cwd, and the
// last-model result JSON.
//
// TODO(M2): this struct is absorbed into the full C++ SessionState when the
// session/datastore module is ported (dispatcher glue milestone). The macro
// engine only depends on the accessors here, so M2 can wrap or replace the
// storage without touching macros.cpp logic.
//
// Ordering: Python dicts preserve insertion order, and the macro engine's
// behavior depends on it (the scalar-substitution loop in _eval_arith walks
// session._scalars in insertion order). OrderedDict mirrors that.
#ifndef PIET_NATIVE_EVAL_SESSION_H
#define PIET_NATIVE_EVAL_SESSION_H

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// Insertion-ordered string map with Python-dict update semantics
// (assignment to an existing key keeps its position; new keys append).
class OrderedDict {
 public:
  bool contains(const std::string& k) const { return find(k) != nullptr; }

  const std::string* find(const std::string& k) const {
    for (const auto& [key, val] : items_) {
      if (key == k) return &val;
    }
    return nullptr;
  }

  // Python dict.get(k, def)
  std::string get(const std::string& k, const std::string& def = "") const {
    const std::string* v = find(k);
    return v ? *v : def;
  }

  void set(const std::string& k, std::string v) {
    for (auto& [key, val] : items_) {
      if (key == k) {
        val = std::move(v);
        return;
      }
    }
    items_.emplace_back(k, std::move(v));
  }

  void erase(const std::string& k) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->first == k) {
        items_.erase(it);
        return;
      }
    }
  }

  void clear() { items_.clear(); }
  const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }

 private:
  std::vector<std::pair<std::string, std::string>> items_;
};

// Same, for the float-valued r()/e() scalar stores.
class OrderedFloatDict {
 public:
  bool contains(const std::string& k) const { return find(k) != nullptr; }

  const double* find(const std::string& k) const {
    for (const auto& [key, val] : items_) {
      if (key == k) return &val;
    }
    return nullptr;
  }

  void set(const std::string& k, double v) {
    for (auto& [key, val] : items_) {
      if (key == k) {
        val = v;
        return;
      }
    }
    items_.emplace_back(k, v);
  }

  void erase(const std::string& k) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->first == k) {
        items_.erase(it);
        return;
      }
    }
  }

  void clear() { items_.clear(); }
  const std::vector<std::pair<std::string, double>>& items() const { return items_; }

 private:
  std::vector<std::pair<std::string, double>> items_;
};

// A named matrix as the Python side stores it: {data, rownames, colnames}.
struct EvalMatrix {
  std::vector<std::vector<double>> data;  // row-major; NaN = Stata missing
  std::vector<std::string> rownames;
  std::vector<std::string> colnames;
};

struct EvalSession {
  OrderedDict locals;     // _locals
  OrderedDict globals;    // _globals
  OrderedDict scalars;    // _scalars (value strings)
  OrderedFloatDict rValues;   // _r_values
  OrderedDict rStrings;       // _r_strings
  OrderedFloatDict eScalars;  // _e_scalars
  // e-class macros. Until AER bundle H the only e() macros PIET could print
  // were e(cmd)/e(depvar)/e(vce), read straight off LastModel, and
  // runEreturnList's header says the rest are "silently omitted rather than
  // invented". pca has to publish eight (e(cmd), e(Ctype), e(title),
  // e(properties), e(predict), e(rotate_cmd), e(estat_cmd), e(marginsnotok)),
  // so the omission becomes a store instead of a disclaimer.
  OrderedDict eStrings;       // _e_strings
  std::vector<std::pair<std::string, EvalMatrix>> matrices;    // _matrices
  // Mata's own namespace (mata_subset.h, M0). SEPARATE from `matrices` because
  // Stata keeps them apart and paper 030 relies on it: `matrix rchange = e(b)`
  // and `mata: rchange = (rchange \ st_matrix("rchange"))` name two different
  // objects, so one shared store would join e(b) onto itself -- a silent wrong
  // answer. Only `st_matrix("X")` crosses over (it reads `matrices`); bare Mata
  // names and `getmata` read and write this store only. Mata matrices carry no
  // row/column names: `colnames` is sized to the column count and left empty so
  // a void 0 x c matrix keeps its c (mata::colsOf).
  std::vector<std::pair<std::string, EvalMatrix>> mataMatrices;
  std::vector<std::pair<std::string, EvalMatrix>> eMatrices;   // _e_matrices
  // r-class matrices (Stata's `r(table)` and friends). Added for AER bundle H's
  // sktest, which returns a matrix and not just scalars. Kept as a THIRD store
  // rather than folded into rValues/matrices because r() matrices have the same
  // lifetime as the rest of r() -- they are replaced wholesale by the next
  // r-class command -- while `matrices` is the user's own namespace that must
  // survive it.
  std::vector<std::pair<std::string, EvalMatrix>> rMatrices;   // _r_matrices
  OrderedDict varLabels;  // _var_meta[name]["label"] (the only key M1 reads)
  std::string cwd;        // session.cwd ("" = unset)

  // BUG-512: `_N` in a MACRO-context expression (`local n = _N`, `scalar s =
  // _N`, `` `=_N' ``, `return scalar N = _N`, `display _N*2`). This layer had no
  // way to know the observation count, so the name reached the evaluator as an
  // undefined identifier and `local n = _N` stored the two characters `_N`
  // (tests/eval/golden/aer_w2/w2_smalls.log section A: Stata stores 12).
  //
  // A PULL hook, not a projected value like varLabels. The count moves with
  // every data verb, and its consumers are every evalArith/expandMacros call
  // site in the tree (a dozen today); a projection would have to be refreshed
  // at each of them, and the one that forgot would go back to storing `_N` as
  // text -- silently. A hook is read at the moment of need and cannot be stale.
  // It keeps the layering BUG-208 drew: this header still knows nothing about
  // SessionState, which installs the hook in its constructor.
  //
  // Unset (the evalcase harness, session-less selftest callers) = this layer
  // does not know, and `_N`/`_n` stay exactly as unresolved as they were.
  std::function<long long()> obsCount;

  // runiform()/runiformint() in a MACRO-context expression (`local u =
  // runiform()`, `` `=runiform()' ``) follow `set seed` (w2c_functions.log
  // runiform_macro/runiformint_macro). A PULL hook for the same reason as
  // obsCount above: `set seed` lives on SessionState, which installs this in
  // its constructor. `epoch` advances on EVERY `set seed`, so re-issuing the
  // same seed restarts the stream exactly as Stata does; the draw counter
  // below restarts whenever the epoch it last saw moves. Unset (session-less
  // harnesses) = the default seed 123456789 (translate.cpp's, for the
  // engine's own draws) at epoch 0.
  struct RngSeedReading {
    std::optional<long long> seed;
    unsigned long long epoch = 0;
  };
  std::function<RngSeedReading()> rngSeed;
  unsigned long long rngEpochSeen = 0;  // the epoch rngDraws counts from
  unsigned long long rngDraws = 0;      // Philox counter of the next draw

  // session.last_model.result — raw result dict (coeftable + scalars) when
  // an estimation is "in memory". TODO(M2): becomes the full LastModel.
  std::optional<nlohmann::ordered_json> lastModelResult;

  const EvalMatrix* findMatrix(const std::string& name) const {
    for (const auto& [k, m] : matrices) {
      if (k == name) return &m;
    }
    for (const auto& [k, m] : eMatrices) {
      if (k == name) return &m;
    }
    return nullptr;
  }

  // e()/r() matrices are NOT reachable through findMatrix: that one resolves the
  // BARE-name namespace (`matlist A`), and folding the class stores into it
  // would let `matlist table` silently answer with r(table). Callers that have
  // already parsed the `e(...)`/`r(...)` wrapper come here instead.
  const EvalMatrix* findClassMatrix(char cls, const std::string& name) const {
    const auto& store = (cls == 'r') ? rMatrices : eMatrices;
    for (const auto& [k, m] : store) {
      if (k == name) return &m;
    }
    return nullptr;
  }

  // BUG-456 (2026-09-08): the e() namespace is ONE namespace in Stata --
  // `ereturn local x` after `ereturn scalar x` is refused (rc 110/152,
  // refute3_capture.log Y6pre). PIET keeps the macro and scalar halves in two
  // stores, so a stale entry in one could shadow the other after the bare-form
  // resolver learned to read both. These two setters are the cheap rule that
  // keeps a lookup from ever finding both: the last write wins and erases its
  // sibling. An EMPTY e-macro is ABSENT in Stata (`ereturn local em ""` ->
  // `assert e(em)==.` rc 0, `==""` r(109); T13-T15), so setting "" erases
  // rather than storing a present-but-empty string. Every writer of the two
  // stores goes through here; `.set` on the stores directly is the old,
  // shadowable shape.
  void setEString(const std::string& k, std::string v) {
    eScalars.erase(k);
    if (v.empty()) {
      eStrings.erase(k);
      return;
    }
    eStrings.set(k, std::move(v));
  }
  void setEScalar(const std::string& k, double v) {
    eStrings.erase(k);
    eScalars.set(k, v);
  }
};

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_SESSION_H
