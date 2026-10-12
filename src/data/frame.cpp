#include "data/frame.h"

#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "io/win_compat.h"  // pread/pwrite shim + UTF-8 path helpers (no-op on POSIX)
#include "core/error.h"     // EngineError("bad_input", ...) for the fweight honest-error paths
#include "core/eng_counters.h"  // W0: patch-vs-full write-plane ledger (P2PERF §W0)
#include "core/threads.h"   // MAXPERF Stage 3: ThreadPool for morsel-parallel collapse/merge
#include "data/radix_sort.h"  // C2-F8: value-identical LSD radix for gsort
#include "data/missing.h"     // Stata `.` / `.a`-`.z` kinds + their total order
#include "core/rng_uniform.h"
#include "core/distributions.h"  // normal/invnormal/ttail/invttail/invt: one copy, shared
#include "data/stata_dates.h"  // BUG-371: %td calendar, shared with PIET's pyeval layer
#include "data/ts_operators.h"  // BUG-369: L./F./D./S. inside expressions (gen/replace/if)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "data/frame_internal.h"

namespace cecon {
namespace {

using frame_detail::NAd;  // R2: single source in frame_internal.h
constexpr double INF = std::numeric_limits<double>::infinity();
// Stata treats missing as +infinity in comparisons (e.g. `. > 2` is true).
// Kept for the ORDERING-only call sites (sort keys); the expression
// comparators below use cecon::stata_cmp instead, because `mi()` collapses
// every missing to one value and so cannot tell `.` from `.a` (Stata's
// equality is kind-exact and its missings are ordered `. < .a < … < .z`).
inline double mi(double v) { return std::isnan(v) ? INF : v; }
// Stata's total order, extended-missing aware — see data/missing.h.
inline int scmp(double a, double b) { return cecon::stata_cmp(a, b); }
// Stata has no user-visible IEEE Inf concept: overflow (exp of a huge number),
// divide-by-zero, and out-of-domain math (ln of a non-positive number, etc.)
// all become `.` (missing) rather than a raw +-Inf that then silently keeps
// propagating/comparing as if it were an ordinary finite number. Every
// arithmetic/math-function case in ev() that can produce a non-finite result
// must funnel it through this so the isnan()-based missing checks used
// everywhere downstream (summarize, keep_if, ...) actually catch it. A
// genuine NaN input (already missing) maps to NAd unchanged; a finite result
// passes through untouched (no-op for the overwhelming common case).
inline double fin(double v) { return std::isfinite(v) ? v : NAd; }
// The truth value `!`, `&` and `|` read from an operand: nonzero is true, and a
// missing value is nonzero, so it is TRUE ([U] 13.2.4). Oracle w2c_functions.log
// operators_missing: `!x` is 0 on x = . and .a, `x & 1` and `x | 0` are 1 there,
// `. & 1` is 1, `. | 0` is 1. The result of the three operators is always 0/1,
// never missing. (They used to read a missing operand as false, and `!` passed
// it through as missing: BUG-620.)
inline bool truthy(double v) { return std::isnan(v) || v != 0.0; }

// ---------- Stata-style expression evaluator (recursive descent) ----------
// Grammar: or > and > cmp > add > mul > pow(right) > unary > primary.
// Supports + - * / ^, comparisons (missing=+inf), & | !, n-ary functions, a bare
// `.` missing literal, _pi, numbers, variables, and (inside a bysort context)
// _n / _N / var[index] subscripting.

// Draw coordinates for this invocation. A draw is a pure function of
// (seed, stream, node, row) -- see core/rng_uniform.h for why that matters when two
// evaluators walk the rows in different orders. Set once from the CLI.
int64_t g_rng_seed = 123456789;
int64_t g_rng_stream = 0;
int64_t g_rng_node_counter = 0;
// Whether the caller supplied the coordinates at all. rnormal() and uniform()
// refuse without them (see the parser): PIET's argv injection is keyed on the
// spelling `runiform`, so without this guard `gen a = rnormal()` then
// `gen b = rnormal()` would each run at the default (seed, stream 0) and return
// the SAME column twice -- noise that is silently perfectly correlated, and a
// `set seed` that is silently ignored.
bool g_rng_coords_set = false;

// The coordinates belong to ONE call, not to the process. Under `cobra serve` (PIET's
// default plane) one resident process answers every request, and a request that
// carries no --rng-seed/--rng-stream would otherwise inherit the previous request's
// coordinates: measured before this guard, `gen u = runiform()` then
// `gen z = rnormal()` in one PIET session answered z with rc 0 from u's stale
// (seed, stream), so the refusal above never fired. Every expression entry point of
// Frame opens one of these; when the OUTERMOST one returns, the coordinates are
// spent. (Consequence for a direct CLI caller: two --gen draws in one argv share one
// set of coordinates, so the second rnormal()/uniform() is refused -- PIET injects
// coordinates per expression and never sends that shape.)
int g_rng_call_depth = 0;
struct RngCallScope {
    RngCallScope() { ++g_rng_call_depth; }
    ~RngCallScope() {
        if (--g_rng_call_depth == 0) g_rng_coords_set = false;
    }
    RngCallScope(const RngCallScope&) = delete;
    RngCallScope& operator=(const RngCallScope&) = delete;
};

enum {
    F_ln, F_log, F_exp, F_sqrt, F_abs, F_sin, F_cos, F_tan, F_log10,
    F_asinh, F_ln1p, F_ln1m, F_int, F_float, F_round, F_floor, F_ceil,
    F_missing, F_cond, F_inlist, F_inrange, F_min, F_max, F_mod,
    // BUG-371 date family. Stata's %td value is a DAY COUNT from 1960-01-01=0.
    F_mdy, F_year, F_month, F_day, F_dow, F_doy, F_quarter, F_halfyear,
    // AER scope 2 / B3. The rest of Stata's time scales. Arithmetic lives in
    // data/stata_dates.h so PIET's pyeval layer computes the same digits from
    // the same code; every value is pinned to tests/validate/b3_funcs_oracle.json.
    F_ym, F_yq, F_yh, F_yw,
    F_mofd, F_qofd, F_hofd, F_yofd, F_wofd, F_week,
    F_dofm, F_dofq, F_dofh, F_dofy, F_dofw,
    F_cofd, F_Cofd, F_dofc, F_dofC,
    F_hh, F_mm, F_ss, F_hhC, F_mmC, F_ssC,
    F_hms, F_dhms,
    F_msofseconds, F_msofminutes, F_msofhours, F_seconds, F_minutes, F_hours,
    // Order-dependent: not a per-row function at all. See materialize_sums.
    F_sum,
    // Draws. Pure functions of (seed, stream, node, row) -- see core/rng_uniform.h.
    F_runiform, F_runiformint, F_rnormal, F_uniform,
    // Distribution functions (core/distributions.h) and sign().
    F_normal, F_invnormal, F_ttail, F_invttail, F_invt, F_sign,
    // group(k): needs _n/_N like a draw needs its row. See the ev() case.
    F_group
};
struct FnInfo { const char* name; int id; int amin; int amax; };
const FnInfo FNS[] = {
    {"ln", F_ln, 1, 1}, {"log", F_log, 1, 1}, {"exp", F_exp, 1, 1}, {"sqrt", F_sqrt, 1, 1},
    {"abs", F_abs, 1, 1}, {"sin", F_sin, 1, 1}, {"cos", F_cos, 1, 1}, {"tan", F_tan, 1, 1},
    {"log10", F_log10, 1, 1}, {"asinh", F_asinh, 1, 1}, {"ln1p", F_ln1p, 1, 1},
    {"ln1m", F_ln1m, 1, 1}, {"int", F_int, 1, 1}, {"round", F_round, 1, 2},
    // float(x): the value a float variable would hold — the store boundary's own
    // rule (frame_detail::round_to_store), so `count if u == float(1/3)` matches a
    // float column bit for bit. Oracle w2e_float_default fn_float_* arms.
    {"float", F_float, 1, 1},
    {"floor", F_floor, 1, 1}, {"ceil", F_ceil, 1, 1},
    // `mi()` is Stata's documented SYNONYM for missing() ([FN] Programming
    // functions) and the spelling replication do-files actually use; it was
    // absent, so `gen byte m = mi(x)` answered rc 1 while `missing(x)` on the
    // same line answered 0. Same entry, same arity — a synonym, not a second
    // implementation (BUG-431, found by the 2026-09-06 corpus census).
    {"missing", F_missing, 1, 256}, {"mi", F_missing, 1, 256},
    {"cond", F_cond, 3, 4}, {"inlist", F_inlist, 2, 256},
    {"inrange", F_inrange, 3, 3}, {"min", F_min, 1, 256}, {"max", F_max, 1, 256},
    // Stata's mod(x,y) is the FLOORED modulus x - y*floor(x/y), not C's fmod:
    // mod(-7,3) is 2, not -1. A non-positive modulus is missing, and so is a
    // missing operand. Oracle (StataBE 19.5, 2026-08-04): mod(7,3)=1,
    // mod(-7,3)=2, mod(7,-3)=., mod(7.5,2)=1.5, mod(7,0)=., mod(.,3)=.,
    // mod(7,.)=.
    {"mod", F_mod, 2, 2},
    // BUG-371 — the date family. `gen dt = mdy(1,1,year)` was an ordinary line a
    // researcher writes and the product refused it, which then cascaded: `format
    // %td dt` failed too, because dt never existed. Stata's %td scale is a signed
    // DAY COUNT with 1960-01-01 == 0 (so 1959-12-31 is -1), and every extractor
    // below is its exact inverse.
    {"mdy", F_mdy, 3, 3}, {"year", F_year, 1, 1}, {"month", F_month, 1, 1},
    {"day", F_day, 1, 1}, {"dow", F_dow, 1, 1}, {"doy", F_doy, 1, 1},
    {"quarter", F_quarter, 1, 1}, {"halfyear", F_halfyear, 1, 1},
    // B3. Note the case-sensitive pairs: %tC's family (Cofd/dofC/hhC/mmC/ssC)
    // is a DIFFERENT scale from %tc's (cofd/dofc/hh/mm/ss), differing by the
    // inserted leap seconds, and Stata spells the difference with a capital.
    {"ym", F_ym, 2, 2}, {"yq", F_yq, 2, 2}, {"yh", F_yh, 2, 2}, {"yw", F_yw, 2, 2},
    {"mofd", F_mofd, 1, 1}, {"qofd", F_qofd, 1, 1}, {"hofd", F_hofd, 1, 1},
    {"yofd", F_yofd, 1, 1}, {"wofd", F_wofd, 1, 1}, {"week", F_week, 1, 1},
    {"dofm", F_dofm, 1, 1}, {"dofq", F_dofq, 1, 1}, {"dofh", F_dofh, 1, 1},
    {"dofy", F_dofy, 1, 1}, {"dofw", F_dofw, 1, 1},
    {"cofd", F_cofd, 1, 1}, {"Cofd", F_Cofd, 1, 1},
    {"dofc", F_dofc, 1, 1}, {"dofC", F_dofC, 1, 1},
    {"hh", F_hh, 1, 1}, {"mm", F_mm, 1, 1}, {"ss", F_ss, 1, 1},
    {"hhC", F_hhC, 1, 1}, {"mmC", F_mmC, 1, 1}, {"ssC", F_ssC, 1, 1},
    {"hms", F_hms, 3, 3}, {"dhms", F_dhms, 4, 4},
    {"msofseconds", F_msofseconds, 1, 1}, {"msofminutes", F_msofminutes, 1, 1},
    {"msofhours", F_msofhours, 1, 1}, {"seconds", F_seconds, 1, 1},
    {"minutes", F_minutes, 1, 1}, {"hours", F_hours, 1, 1},
    {"sum", F_sum, 1, 1},
    {"runiform", F_runiform, 0, 2}, {"runiformint", F_runiformint, 2, 2},
    // Oracle tests/eval/golden/aer_w2/w2c_functions.log (StataBE 19.5, 2026-09-23),
    // data arms rnormal_dist / uniform_dist: rc 0, storage float, 200 draws. uniform()
    // is Stata's old spelling of runiform() and takes NO arguments (uniform(1,2) is
    // r(198), measured); rnormal() takes 0-2 (a third is r(198)).
    {"rnormal", F_rnormal, 0, 2}, {"uniform", F_uniform, 0, 0},
    // Same oracle, arms normal_data_hi/_lo, invnormal_data_tail/_ctl, ttail_data,
    // invttail_data, invt_data, sign_data, group_data.
    {"normal", F_normal, 1, 1}, {"invnormal", F_invnormal, 1, 1},
    {"ttail", F_ttail, 2, 2}, {"invttail", F_invttail, 2, 2}, {"invt", F_invt, 2, 2},
    {"sign", F_sign, 1, 1}, {"group", F_group, 1, 1},
};

// One switch, two callers. frame.cpp's documented way of going wrong is a name
// that reaches FNS[] and the scalar ev() but not the vectorised ev_fn_vec(),
// because that combination returns a silent column of missings with no error and
// a one-row unit test still passes. Writing the mapping once, and the case-label
// list once (CECON_TIME_UNARY_CASES below), removes the opportunity.
inline double stata_time_unary(int fn, double x) {
    switch (fn) {
        case F_mofd: return cecon::stata_scale_of_day(x, cecon::DS_month);
        case F_qofd: return cecon::stata_scale_of_day(x, cecon::DS_quarter);
        case F_hofd: return cecon::stata_scale_of_day(x, cecon::DS_half);
        case F_yofd: return cecon::stata_scale_of_day(x, cecon::DS_year);
        case F_wofd: return cecon::stata_scale_of_day(x, cecon::DS_week);
        case F_week: return cecon::stata_week_of_day(x);
        case F_dofm: return cecon::stata_day_of_scale(x, cecon::DS_month);
        case F_dofq: return cecon::stata_day_of_scale(x, cecon::DS_quarter);
        case F_dofh: return cecon::stata_day_of_scale(x, cecon::DS_half);
        case F_dofy: return cecon::stata_day_of_scale(x, cecon::DS_year);
        case F_dofw: return cecon::stata_day_of_scale(x, cecon::DS_week);
        case F_cofd: return cecon::stata_cofd(x);
        case F_Cofd: return cecon::stata_Cofd(x);
        case F_dofc: return cecon::stata_dofc(x);
        case F_dofC: return cecon::stata_dofC(x);
        case F_hh:   return cecon::stata_clock_part(x, cecon::CLK_hour, false);
        case F_mm:   return cecon::stata_clock_part(x, cecon::CLK_minute, false);
        case F_ss:   return cecon::stata_clock_part(x, cecon::CLK_second, false);
        case F_hhC:  return cecon::stata_clock_part(x, cecon::CLK_hour, true);
        case F_mmC:  return cecon::stata_clock_part(x, cecon::CLK_minute, true);
        case F_ssC:  return cecon::stata_clock_part(x, cecon::CLK_second, true);
        case F_msofseconds: return cecon::stata_msofseconds(x);
        case F_msofminutes: return cecon::stata_msofminutes(x);
        case F_msofhours:   return cecon::stata_msofhours(x);
        case F_seconds:     return cecon::stata_seconds(x);
        case F_minutes:     return cecon::stata_minutes(x);
        case F_hours:       return cecon::stata_hours(x);
    }
    return NAd;
}

// sign(x): -1, 0 or 1; every missing (.a included, measured) answers `.`; sign(-0) is 0.
inline double stata_sign(double x) {
    if (std::isnan(x)) return NAd;
    return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
}

// A distribution function's NaN (out of domain, or a missing argument) is Stata's `.`.
inline double stata_dist(double v) { return std::isnan(v) ? NAd : v; }

// group(k): splits the observations into k consecutive, nearly equal groups. It is
// absent from Stata 19's [FN] manual but still runs, so there is no written rule;
// the one below is read off StataBE 19.5 (2026-09-29, 78 (N, k) cells, N = 1..13,
// k = 1..5 and 2.5, plus by-groups and tiny k): observation n of N gets
//     1                                     if n == 1
//     int(k (n-1)/(N-1) + 1 - 1e-6)         otherwise,
// where the 1e-6 is Stata's own fuzz (k = 1 + 9.9e-7 on N = 2 answers 1, 1 + 1e-6
// answers 2; k = 1e-9 gives 1 0 0 0 ...). n and N are the observation number and
// count of the by-group under `by`, of the dataset otherwise; an `if` does not
// renumber (group(2) if _n > 2 on N = 6 answers 1 2 2 2 on rows 3-6). k <= 0 or
// missing is `.`. Under `in a/b` Stata renumbers within the range (N = b-a+1); that
// shape is refused, in the parser when the range is folded into the expression and
// in refuse_group_in_masked_range when it rides --gen-mask. Inside sum() the same
// by-group n and N apply (materialize_sums evaluates its argument per group).
inline double stata_group(double k, int64_t n, int64_t N) {
    if (std::isnan(k) || !(k > 0.0)) return NAd;
    if (n <= 1 || N <= 1) return 1.0;
    // BUG-577: the fuzz is folded as ONE constant, (1 - 1e-6). Written as
    // `+ 1.0 - 1e-6` it rounds twice, and group(1e-6) at n = N = 2 gave
    // floor(0.9999999999999999) = 0 where StataBE answers 1. The folded form
    // reproduces all 630 captured StataBE cells (N x k grid, tiny k, k near 1 and 1e6).
    return std::floor(k * static_cast<double>(n - 1) / static_cast<double>(N - 1) +
                      (1.0 - 1e-6));
}

#define CECON_TIME_UNARY_CASES                                                  \
    case F_mofd: case F_qofd: case F_hofd: case F_yofd: case F_wofd:            \
    case F_week: case F_dofm: case F_dofq: case F_dofh: case F_dofy:            \
    case F_dofw: case F_cofd: case F_Cofd: case F_dofc: case F_dofC:            \
    case F_hh: case F_mm: case F_ss: case F_hhC: case F_mmC: case F_ssC:        \
    case F_msofseconds: case F_msofminutes: case F_msofhours:                   \
    case F_seconds: case F_minutes: case F_hours
const FnInfo* find_fn(const std::string& s) {
    for (const FnInfo& f : FNS) if (s == f.name) return &f;
    return nullptr;
}

struct Node {
    enum K { Num, Var, ObsN, ObsBigN, Subscript, Neg, Not, Add, Sub, Mul, Div, Pow,
             Lt, Gt, Le, Ge, Eq, Ne, And, Or, Fn, Str } kind;
    double num = 0;
    const double* var = nullptr;
    int fn = 0;
    std::unique_ptr<Node> a, b;               // unary/binary children; Subscript: a=Var, b=index
    std::vector<std::unique_ptr<Node>> args;  // Fn arguments
    // String/factor equality support: a `Str` node carries a string literal; a
    // factor-column reference is a Var node with fcol set and var still null
    // until cmp() resolves `factorvar ==/!= "literal"` into an ordinary numeric
    // compare against the level code (materialised into owned). Any other use
    // of a factor/string operand hits ev()'s null/Str guard.
    std::string sval;
    const FrameColumn* fcol = nullptr;
    std::vector<double> owned;
};

// Per-row evaluation context. order != null means we are inside a bysort group:
// _n/_N and x[k] resolve within [g0,g1) of *order in sort order.
struct EvalCtx {
    int64_t row = 0;
    const std::vector<int64_t>* order = nullptr;
    int64_t g0 = 0, g1 = 0, pos = 0;
    // BUG-364: dataset row count, for the NO-bysort case. Stata defines `_n` as
    // the observation number in the current sort order and `_N` as the number of
    // observations, both of which exist with or without a `by` prefix; this is
    // what makes them meaningful outside one.
    int64_t n_all = 0;
};

double ev(const Node* n, const EvalCtx& c) {
    switch (n->kind) {
        case Node::Num: return n->num;
        case Node::Var:
            if (!n->var) throw std::runtime_error(
                "expr: string/factor variable used where a number is required");
            return n->var[c.row];
        case Node::Str:
            throw std::runtime_error("expr: string literal used where a number is required");
        // BUG-364: outside a bysort group these resolve against the DATASET, which
        // is what Stata does — `keep if _n<=100`, `gen first = x[1]`, `di _N` are
        // ordinary unprefixed forms. They used to throw "requires a bysort context",
        // an honest error for a construct Stata accepts, and no gate covered the
        // plain form (the corpus's only `_n`-in-filter case was the bysort variant).
        // In-group behaviour is untouched.
        case Node::ObsN:
            return static_cast<double>(c.order ? c.pos : c.row + 1);
        case Node::ObsBigN:
            return static_cast<double>(c.order ? (c.g1 - c.g0) : c.n_all);
        case Node::Subscript: {
            double kd = ev(n->b.get(), c);
            if (std::isnan(kd)) return NAd;
            int64_t k = static_cast<int64_t>(kd);  // truncate toward zero
            if (!c.order) {
                if (k < 1 || k > c.n_all) return NAd;   // out of range = missing, as in Stata
                return n->a->var[k - 1];
            }
            if (k < 1 || k > (c.g1 - c.g0)) return NAd;
            return n->a->var[(*c.order)[c.g0 + k - 1]];
        }
        case Node::Neg: return -ev(n->a.get(), c);
        case Node::Not: return truthy(ev(n->a.get(), c)) ? 0.0 : 1.0;
        case Node::Add: return fin(ev(n->a.get(), c) + ev(n->b.get(), c));
        case Node::Sub: return fin(ev(n->a.get(), c) - ev(n->b.get(), c));
        case Node::Mul: return fin(ev(n->a.get(), c) * ev(n->b.get(), c));
        case Node::Div: return fin(ev(n->a.get(), c) / ev(n->b.get(), c));
        case Node::Pow: return fin(std::pow(ev(n->a.get(), c), ev(n->b.get(), c)));
        case Node::Lt: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) <  0 ? 1.0 : 0.0;
        case Node::Gt: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) >  0 ? 1.0 : 0.0;
        case Node::Le: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) <= 0 ? 1.0 : 0.0;
        case Node::Ge: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) >= 0 ? 1.0 : 0.0;
        case Node::Eq: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) == 0 ? 1.0 : 0.0;
        case Node::Ne: return scmp(ev(n->a.get(), c), ev(n->b.get(), c)) != 0 ? 1.0 : 0.0;
        case Node::And: { double x = ev(n->a.get(), c), y = ev(n->b.get(), c);
            return (truthy(x) && truthy(y)) ? 1.0 : 0.0; }
        case Node::Or: { double x = ev(n->a.get(), c), y = ev(n->b.get(), c);
            return (truthy(x) || truthy(y)) ? 1.0 : 0.0; }
        case Node::Fn: {
            const auto& A = n->args;
            auto a0 = [&] { return ev(A[0].get(), c); };
            switch (n->fn) {
                case F_ln: case F_log: return fin(std::log(a0()));
                case F_exp: return fin(std::exp(a0()));
                case F_sqrt: return fin(std::sqrt(a0()));
                case F_abs: return std::fabs(a0());
                case F_sin: return std::sin(a0());
                case F_cos: return std::cos(a0());
                case F_tan: return std::tan(a0());
                case F_log10: return fin(std::log10(a0()));
                case F_asinh: return fin(std::asinh(a0()));
                case F_ln1p: return fin(std::log1p(a0()));
                case F_ln1m: return fin(std::log1p(-a0()));
                case F_int: { double x = a0(); return std::isnan(x) ? NAd : std::trunc(x); }
                case F_float: return frame_detail::round_to_store(a0(), StoreType::Float);
                case F_round: {
                    double x = a0();
                    if (std::isnan(x)) return NAd;
                    if (A.size() >= 2) {  // round(x, unit): Stata's unit*round(x/unit); unit 0/miss -> .
                        double u = ev(A[1].get(), c);
                        if (std::isnan(u)) return NAd;
                        return fin(std::round(x / u) * u);
                    }
                    return std::round(x);
                }
                case F_floor: { double x = a0(); return std::isnan(x) ? NAd : std::floor(x); }
                case F_ceil: { double x = a0(); return std::isnan(x) ? NAd : std::ceil(x); }
                case F_missing:
                    for (auto& q : A) if (std::isnan(ev(q.get(), c))) return 1.0;
                    return 0.0;
                case F_cond: {
                    double cc = ev(A[0].get(), c);
                    if (std::isnan(cc)) return A.size() == 4 ? ev(A[3].get(), c) : ev(A[1].get(), c);
                    return cc != 0.0 ? ev(A[1].get(), c) : ev(A[2].get(), c);
                }
                case F_inlist: {
                    double z = ev(A[0].get(), c);
                    for (size_t k = 1; k < A.size(); ++k) {
                        double v = ev(A[k].get(), c);
                        if ((std::isnan(z) && std::isnan(v)) || z == v) return 1.0;
                    }
                    return 0.0;
                }
                case F_inrange: {
                    double z = ev(A[0].get(), c), lo = ev(A[1].get(), c), hi = ev(A[2].get(), c);
                    if (std::isnan(z)) return 0.0;
                    bool lm = std::isnan(lo), hm = std::isnan(hi);
                    if (lm && hm) return 1.0;
                    if (lm) return z <= hi ? 1.0 : 0.0;
                    if (hm) return z >= lo ? 1.0 : 0.0;
                    return (lo <= z && z <= hi) ? 1.0 : 0.0;
                }
                case F_mod: {
                    double x = a0(), y = ev(A[1].get(), c);
                    if (std::isnan(x) || std::isnan(y) || y <= 0.0) return NAd;
                    return fin(x - y * std::floor(x / y));
                }
                case F_mdy:
                    return stata_mdy(a0(), ev(A[1].get(), c), ev(A[2].get(), c));
                case F_year: return stata_date_part(a0(), DP_year);
                case F_month: return stata_date_part(a0(), DP_month);
                case F_day: return stata_date_part(a0(), DP_day);
                case F_dow: return stata_date_part(a0(), DP_dow);
                case F_doy: return stata_date_part(a0(), DP_doy);
                case F_quarter: return stata_date_part(a0(), DP_quarter);
                case F_halfyear: return stata_date_part(a0(), DP_half);
                CECON_TIME_UNARY_CASES: return stata_time_unary(n->fn, a0());
                case F_sum:
                    // Filled by materialize_sums before evaluation starts.
                    return n->owned.empty() ? NAd : n->owned[static_cast<size_t>(c.row)];
                case F_runiform: {
                    const double u = cecon::rng_uniform(g_rng_seed, g_rng_stream,
                                                        static_cast<int64_t>(n->num), c.row);
                    if (A.empty()) return u;
                    const double lo = a0(), hi = ev(A[1].get(), c);
                    // Stata answers missing for a degenerate or inverted range
                    // (runiform(3,3) is `.`, measured).
                    if (std::isnan(lo) || std::isnan(hi) || !(lo < hi)) return NAd;
                    return lo + u * (hi - lo);
                }
                case F_runiformint: {
                    const double lo = a0(), hi = ev(A[1].get(), c);
                    if (std::isnan(lo) || std::isnan(hi) || hi < lo) return NAd;
                    if (lo != std::floor(lo) || hi != std::floor(hi)) return NAd;
                    return cecon::rng_uniform_int(g_rng_seed, g_rng_stream,
                                                  static_cast<int64_t>(n->num), c.row, lo, hi);
                }
                case F_uniform:
                    return cecon::rng_uniform(g_rng_seed, g_rng_stream,
                                              static_cast<int64_t>(n->num), c.row);
                case F_rnormal: {
                    // Inverse CDF (AS241) over the same 53-bit word runiform() uses,
                    // centred into the open interval so the quantile is always finite.
                    const uint64_t bits = cecon::uniform_bits(g_rng_seed, g_rng_stream,
                                                              static_cast<int64_t>(n->num), c.row);
                    const double u = (static_cast<double>(bits >> 11) + 0.5) *
                                     (1.0 / 9007199254740992.0);
                    const double z = cecon::normal_icdf(u);
                    if (A.empty()) return z;
                    const double m = a0();
                    const double sd = A.size() >= 2 ? ev(A[1].get(), c) : 1.0;
                    // rnormal(.,1), rnormal(0,-1) are `.`; rnormal(1,0) is 1 (measured).
                    if (std::isnan(m) || std::isnan(sd) || sd < 0.0) return NAd;
                    return fin(m + sd * z);
                }
                case F_normal: return stata_dist(cecon::stata_normal(a0()));
                case F_invnormal: return stata_dist(cecon::stata_invnormal(a0()));
                case F_ttail: return stata_dist(cecon::stata_ttail(a0(), ev(A[1].get(), c)));
                case F_invttail: return stata_dist(cecon::stata_invttail(a0(), ev(A[1].get(), c)));
                case F_invt: return stata_dist(cecon::stata_invt(a0(), ev(A[1].get(), c)));
                case F_sign: return stata_sign(a0());
                case F_group:
                    return stata_group(a0(), c.order ? c.pos : c.row + 1,
                                       c.order ? (c.g1 - c.g0) : c.n_all);
                case F_ym: return cecon::stata_ym(a0(), ev(A[1].get(), c));
                case F_yq: return cecon::stata_yq(a0(), ev(A[1].get(), c));
                case F_yh: return cecon::stata_yh(a0(), ev(A[1].get(), c));
                case F_yw: return cecon::stata_yw(a0(), ev(A[1].get(), c));
                case F_hms:
                    return cecon::stata_hms(a0(), ev(A[1].get(), c), ev(A[2].get(), c));
                case F_dhms:
                    return cecon::stata_dhms(a0(), ev(A[1].get(), c), ev(A[2].get(), c),
                                             ev(A[3].get(), c));
                case F_min: { double m = NAd; for (auto& q : A) { double v = ev(q.get(), c);
                    if (!std::isnan(v)) m = std::isnan(m) ? v : std::min(m, v); } return m; }
                case F_max: { double m = NAd; for (auto& q : A) { double v = ev(q.get(), c);
                    if (!std::isnan(v)) m = std::isnan(m) ? v : std::max(m, v); } return m; }
            }
            return NAd;
        }
    }
    return NAd;
}

// ---------- Vectorized (column-at-a-time) evaluator ----------
// Byte-identical to the per-row ev() above: every element replays the exact same
// scalar operation, in the same nesting order, through the same fin()/mi() funnels —
// only the loop structure changes. The tree is walked ONCE (each subexpression
// materialised as a length-N column) instead of re-dispatching the whole AST n_ times,
// so the tight per-node loops autovectorise and shed the per-row switch/pointer-chase
// overhead. Used only for order-free contexts (generate / keepif / egen_by inner-expr);
// the bysort path (bygen, _n/_N/subscript) keeps ev().
//
// vectorizable() gates entry: cond() evaluates only the taken branch per row, so forcing
// an unselected branch column-wide could raise where the scalar never would — fall back
// to ev() for any tree containing cond() or a bysort-only node (_n/_N/subscript). Every
// other operator/function evaluates all its operands unconditionally in ev() too, so
// materialising them column-wide changes nothing observable.
bool vectorizable(const Node* n) {
    switch (n->kind) {
        case Node::ObsN: case Node::ObsBigN: case Node::Subscript: return false;
        case Node::Fn:
            // A draw needs its ROW index, which only the scalar evaluator carries.
            if (n->fn == F_cond || n->fn == F_sum || n->fn == F_runiform ||
                n->fn == F_runiformint || n->fn == F_rnormal || n->fn == F_uniform ||
                n->fn == F_group) return false;
            break;
        default: break;
    }
    if (n->a && !vectorizable(n->a.get())) return false;
    if (n->b && !vectorizable(n->b.get())) return false;
    for (const auto& q : n->args) if (!vectorizable(q.get())) return false;
    return true;
}

// True when `fn` occurs anywhere in the tree.
bool ast_has_fn(const Node* n, int fn) {
    if (!n) return false;
    if (n->kind == Node::Fn && n->fn == fn) return true;
    if (ast_has_fn(n->a.get(), fn) || ast_has_fn(n->b.get(), fn)) return true;
    for (const auto& q : n->args) if (ast_has_fn(q.get(), fn)) return true;
    return false;
}

// group() whose `in a/b` range rides --gen-mask instead of the expression. PIET
// sends a running sum's qualifier that way (egen.cpp runBysortGen, maskOut), so
// `generate s = sum(group(3)) in 3/7` arrives as expr `sum(group(3))` with mask
// `_n>=3 & _n<=7`, which the parser's check on the expression text never sees.
// Stata numbers group() within the range; refuse rather than number over the
// dataset (BUG-576). A mask that is only an `if` does not renumber, and passes.
void refuse_group_in_masked_range(const Node* ast, const std::string& expr,
                                  const std::string& mask_expr) {
    if (mask_expr.find("_n>=") == std::string::npos ||
        mask_expr.find("_n<=") == std::string::npos || !ast_has_fn(ast, F_group))
        return;
    throw std::runtime_error("expr: group() cannot be combined with an `in` range (or an "
                             "`_n>=`/`_n<=` condition): Stata numbers group() within the "
                             "range in '" + expr + "'");
}

void ev_vec(const Node* n, int64_t N, std::vector<double>& out);

void ev_fn_vec(const Node* n, int64_t N, std::vector<double>& out) {
    const auto& A = n->args;
    // Single-argument math functions: evaluate arg0 into out, transform in place.
    switch (n->fn) {
        case F_ln: case F_log: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log(out[i])); return;
        case F_exp: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::exp(out[i])); return;
        case F_sqrt: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::sqrt(out[i])); return;
        case F_abs: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::fabs(out[i]); return;
        case F_sin: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::sin(out[i]); return;
        case F_cos: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::cos(out[i]); return;
        case F_tan: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = std::tan(out[i]); return;
        case F_log10: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log10(out[i])); return;
        case F_asinh: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::asinh(out[i])); return;
        case F_ln1p: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log1p(out[i])); return;
        case F_ln1m: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = fin(std::log1p(-out[i])); return;
        case F_int: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? NAd : std::trunc(x); } return;
        case F_float: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = frame_detail::round_to_store(out[i], StoreType::Float); return;
        case F_floor: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? NAd : std::floor(x); } return;
        case F_ceil: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? NAd : std::ceil(x); } return;
        // BUG-371 date extractors: 1-arg, same shape as the math functions above.
        case F_year: case F_month: case F_day: case F_dow:
        case F_doy: case F_quarter: case F_halfyear: {
            DatePart part = DP_year;
            switch (n->fn) {
                case F_month: part = DP_month; break;
                case F_day: part = DP_day; break;
                case F_dow: part = DP_dow; break;
                case F_doy: part = DP_doy; break;
                case F_quarter: part = DP_quarter; break;
                case F_halfyear: part = DP_half; break;
                default: break;
            }
            ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_date_part(out[i], part);
            return;
        }
        // B3 time scales: 1-arg, same shape as the extractors above and driven by
        // the SAME label list and the SAME mapping function as the scalar path.
        CECON_TIME_UNARY_CASES: {
            ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_time_unary(n->fn, out[i]);
            return;
        }
        case F_normal: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_normal(out[i])); return;
        case F_invnormal: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invnormal(out[i])); return;
        case F_sign: ev_vec(A[0].get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = stata_sign(out[i]); return;
        case F_round:
            if (A.size() < 2) {  // 1-arg round(x): std::round with missing passthrough
                ev_vec(A[0].get(), N, out);
                for (int64_t i = 0; i < N; ++i) { double x = out[i]; out[i] = std::isnan(x) ? NAd : std::round(x); }
                return;
            }
            break;  // 2-arg round(x,u) handled in the multi-arg section below
        default: break;
    }
    // Multi-arg / n-ary functions: materialise every argument as a column, then combine.
    std::vector<std::vector<double>> V(A.size());
    for (size_t k = 0; k < A.size(); ++k) ev_vec(A[k].get(), N, V[k]);
    out.resize(static_cast<size_t>(N));
    switch (n->fn) {
        case F_round:  // round(x, unit): unit*round(x/unit); unit 0/miss -> .
            for (int64_t i = 0; i < N; ++i) {
                double x = V[0][i]; if (std::isnan(x)) { out[i] = NAd; continue; }
                double u = V[1][i]; if (std::isnan(u)) { out[i] = NAd; continue; }
                out[i] = fin(std::round(x / u) * u);
            }
            return;
        case F_mdy:  // BUG-371 — same validation as the scalar path, one shared helper
            for (int64_t i = 0; i < N; ++i) out[i] = stata_mdy(V[0][i], V[1][i], V[2][i]);
            return;
        case F_ym:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_ym(V[0][i], V[1][i]);
            return;
        case F_yq:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yq(V[0][i], V[1][i]);
            return;
        case F_yh:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yh(V[0][i], V[1][i]);
            return;
        case F_yw:
            for (int64_t i = 0; i < N; ++i) out[i] = cecon::stata_yw(V[0][i], V[1][i]);
            return;
        case F_hms:
            for (int64_t i = 0; i < N; ++i)
                out[i] = cecon::stata_hms(V[0][i], V[1][i], V[2][i]);
            return;
        case F_dhms:
            for (int64_t i = 0; i < N; ++i)
                out[i] = cecon::stata_dhms(V[0][i], V[1][i], V[2][i], V[3][i]);
            return;
        case F_ttail:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_ttail(V[0][i], V[1][i]));
            return;
        case F_invttail:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invttail(V[0][i], V[1][i]));
            return;
        case F_invt:
            for (int64_t i = 0; i < N; ++i) out[i] = stata_dist(cecon::stata_invt(V[0][i], V[1][i]));
            return;
        case F_mod:  // floored modulus; y<=0 or any missing -> .
            for (int64_t i = 0; i < N; ++i) {
                double x = V[0][i], y = V[1][i];
                out[i] = (std::isnan(x) || std::isnan(y) || y <= 0.0)
                             ? NAd
                             : fin(x - y * std::floor(x / y));
            }
            return;
        case F_missing:
            for (int64_t i = 0; i < N; ++i) {
                double r = 0.0;
                for (const auto& v : V) if (std::isnan(v[i])) { r = 1.0; break; }
                out[i] = r;
            }
            return;
        case F_inlist:
            for (int64_t i = 0; i < N; ++i) {
                double z = V[0][i], r = 0.0;
                for (size_t k = 1; k < V.size(); ++k) {
                    double v = V[k][i];
                    if ((std::isnan(z) && std::isnan(v)) || z == v) { r = 1.0; break; }
                }
                out[i] = r;
            }
            return;
        case F_inrange:
            for (int64_t i = 0; i < N; ++i) {
                double z = V[0][i], lo = V[1][i], hi = V[2][i];
                if (std::isnan(z)) { out[i] = 0.0; continue; }
                bool lm = std::isnan(lo), hm = std::isnan(hi);
                if (lm && hm) { out[i] = 1.0; continue; }
                if (lm) { out[i] = z <= hi ? 1.0 : 0.0; continue; }
                if (hm) { out[i] = z >= lo ? 1.0 : 0.0; continue; }
                out[i] = (lo <= z && z <= hi) ? 1.0 : 0.0;
            }
            return;
        case F_min:
            for (int64_t i = 0; i < N; ++i) {
                double m = NAd;
                for (const auto& v : V) { double x = v[i]; if (!std::isnan(x)) m = std::isnan(m) ? x : std::min(m, x); }
                out[i] = m;
            }
            return;
        case F_max:
            for (int64_t i = 0; i < N; ++i) {
                double m = NAd;
                for (const auto& v : V) { double x = v[i]; if (!std::isnan(x)) m = std::isnan(m) ? x : std::max(m, x); }
                out[i] = m;
            }
            return;
        default: break;
    }
    // Unknown function id: ev() falls through to `return NAd;` — mirror that.
    std::fill(out.begin(), out.end(), NAd);
}

#undef CECON_TIME_UNARY_CASES

void ev_vec(const Node* n, int64_t N, std::vector<double>& out) {
    out.resize(static_cast<size_t>(N));
    switch (n->kind) {
        case Node::Num:
            std::fill(out.begin(), out.end(), n->num);
            return;
        case Node::Var:
            if (!n->var) throw std::runtime_error(
                "expr: string/factor variable used where a number is required");
            for (int64_t i = 0; i < N; ++i) out[i] = n->var[i];
            return;
        case Node::Str:
            throw std::runtime_error("expr: string literal used where a number is required");
        case Node::Neg:
            ev_vec(n->a.get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = -out[i];
            return;
        case Node::Not:
            ev_vec(n->a.get(), N, out);
            for (int64_t i = 0; i < N; ++i) out[i] = truthy(out[i]) ? 0.0 : 1.0;
            return;
        case Node::Add: case Node::Sub: case Node::Mul: case Node::Div: case Node::Pow:
        case Node::Lt: case Node::Gt: case Node::Le: case Node::Ge:
        case Node::Eq: case Node::Ne: case Node::And: case Node::Or: {
            ev_vec(n->a.get(), N, out);
            std::vector<double> rb;
            ev_vec(n->b.get(), N, rb);
            switch (n->kind) {
                case Node::Add: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] + rb[i]); break;
                case Node::Sub: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] - rb[i]); break;
                case Node::Mul: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] * rb[i]); break;
                case Node::Div: for (int64_t i = 0; i < N; ++i) out[i] = fin(out[i] / rb[i]); break;
                case Node::Pow: for (int64_t i = 0; i < N; ++i) out[i] = fin(std::pow(out[i], rb[i])); break;
                case Node::Lt: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) <  0 ? 1.0 : 0.0; break;
                case Node::Gt: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) >  0 ? 1.0 : 0.0; break;
                case Node::Le: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) <= 0 ? 1.0 : 0.0; break;
                case Node::Ge: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) >= 0 ? 1.0 : 0.0; break;
                case Node::Eq: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) == 0 ? 1.0 : 0.0; break;
                case Node::Ne: for (int64_t i = 0; i < N; ++i) out[i] = scmp(out[i], rb[i]) != 0 ? 1.0 : 0.0; break;
                case Node::And: for (int64_t i = 0; i < N; ++i)
                    out[i] = (truthy(out[i]) && truthy(rb[i])) ? 1.0 : 0.0; break;
                case Node::Or: for (int64_t i = 0; i < N; ++i)
                    out[i] = (truthy(out[i]) || truthy(rb[i])) ? 1.0 : 0.0; break;
                default: break;
            }
            return;
        }
        case Node::Fn:
            ev_fn_vec(n, N, out);
            return;
        default:
            // ObsN/ObsBigN/Subscript are excluded by vectorizable(); never reached here.
            throw std::runtime_error("expr: node not evaluable outside a bysort context");
    }
}

// Evaluate `ast` over all n rows in an order-free context, byte-identically to the
// per-row ev() loop but walking the tree once when the shape allows (vectorizable()).
// ---------- sum(): the one function whose answer depends on row ORDER --------
// Stata's sum(x) is a running total, so it is not a function of its arguments the
// way every other entry in FNS[] is -- row i's answer needs rows 0..i-1. It is
// materialised into the node BEFORE evaluation instead, once, in the order the
// caller is actually walking:
//   generate  natural row order, one running total
//   bysort    sort order, reset at every group boundary
// Semantics measured against StataBE (oracle keys sum.*): a missing input
// contributes 0 and the result is never missing -- sum() of a column whose first
// value is missing answers 0, not `.`.
// Under bysort the ARGUMENT is also evaluated in the group context (g0a/g1a/posa),
// so `_n`, `_N`, x[k] and group() inside sum() count within the by-group, as the
// same expression outside sum() does (BUG-576: `by g: gen t = sum(_n)` answered
// 1 3 6 4 9 15 instead of 1 3 6 1 3 6).
void materialize_sums(Node* n, int64_t N, const std::vector<int64_t>* order,
                      const std::vector<int64_t>* g0a, const std::vector<int64_t>* g1a,
                      const std::vector<int64_t>* posa, const std::vector<double>* mask);

// Whether an `if` qualifier's value selects its row. Stata's `if exp` keeps the
// rows where exp is nonzero, and a missing value is nonzero, so a missing
// condition SELECTS (w2m_bymask F03: `by gg: generate f03 = sum(x) if x[_n-1]`
// answers 20 60 120 | 10 40 90 -- the group's first row, whose x[_n-1] is
// missing, is in). The engine used to read a missing mask as false (BUG-837).
inline bool if_selects(double m) { return std::isnan(m) || m != 0.0; }

std::vector<double> eval_column_node(const Node* ast, int64_t n);

void materialize_sums(Node* n, int64_t N, const std::vector<int64_t>* order,
                      const std::vector<int64_t>* g0a, const std::vector<int64_t>* g1a,
                      const std::vector<int64_t>* posa, const std::vector<double>* mask) {
    if (!n) return;
    // Depth first, so a nested sum(sum(x)) is already filled when the outer one runs.
    materialize_sums(n->a.get(), N, order, g0a, g1a, posa, mask);
    materialize_sums(n->b.get(), N, order, g0a, g1a, posa, mask);
    for (auto& q : n->args) materialize_sums(q.get(), N, order, g0a, g1a, posa, mask);
    if (n->kind != Node::Fn || n->fn != F_sum || n->args.empty()) return;

    std::vector<double> arg;
    if (order == nullptr || vectorizable(n->args[0].get())) {
        arg = eval_column_node(n->args[0].get(), N);
    } else {
        // Same per-row context Frame::bygen builds for the outer expression.
        arg.resize(static_cast<size_t>(N));
        EvalCtx c;
        c.n_all = N;
        c.order = order;
        for (int64_t i = 0; i < N; ++i) {
            const size_t r = static_cast<size_t>(i);
            c.row = i; c.g0 = (*g0a)[r]; c.g1 = (*g1a)[r]; c.pos = (*posa)[r];
            arg[r] = ev(n->args[0].get(), c);
        }
    }
    n->owned.assign(static_cast<size_t>(N), NAd);
    // A row the `if` qualifier excludes contributes NOTHING and gets no value --
    // Stata leaves it missing and carries the total across it untouched.
    auto included = [mask](int64_t row) {
        return mask == nullptr || if_selects((*mask)[static_cast<size_t>(row)]);
    };
    double acc = 0.0;
    if (order == nullptr) {
        for (int64_t i = 0; i < N; ++i) {
            if (!included(i)) continue;   // stays NAd
            const double v = arg[static_cast<size_t>(i)];
            if (!std::isnan(v)) acc = fin(acc + v);
            n->owned[static_cast<size_t>(i)] = acc;
        }
        return;
    }
    int64_t cur_g0 = -1;
    for (int64_t t = 0; t < N; ++t) {
        const int64_t row = (*order)[static_cast<size_t>(t)];
        const int64_t g0 = (*g0a)[static_cast<size_t>(row)];
        if (g0 != cur_g0) { cur_g0 = g0; acc = 0.0; }
        if (!included(row)) continue;   // stays NAd
        const double v = arg[static_cast<size_t>(row)];
        if (!std::isnan(v)) acc = fin(acc + v);
        n->owned[static_cast<size_t>(row)] = acc;
    }
}

std::vector<double> eval_column_node(const Node* ast, int64_t n) {
    std::vector<double> out(static_cast<size_t>(n));
    if (vectorizable(ast)) {
        ev_vec(ast, n, out);
    } else {
        EvalCtx c;
        c.n_all = n;
        for (int64_t i = 0; i < n; ++i) { c.row = i; out[i] = ev(ast, c); }
    }
    return out;
}

std::vector<double> eval_column(const Node* ast, int64_t n) {
    std::vector<double> out(static_cast<size_t>(n));
    if (vectorizable(ast)) {
        ev_vec(ast, n, out);
    } else {
        EvalCtx c;
        c.n_all = n;  // BUG-364: `_N` / x[k] resolve against the dataset here
        for (int64_t i = 0; i < n; ++i) { c.row = i; out[i] = ev(ast, c); }
    }
    return out;
}

struct Parser {
    const std::string& s;
    size_t p = 0;
    const Frame& fr;
    Parser(const std::string& src, const Frame& f) : s(src), fr(f) {}

    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    bool eat(const char* tok) {
        ws();
        size_t n = std::strlen(tok);
        if (s.compare(p, n, tok) == 0) { p += n; return true; }
        return false;
    }
    [[noreturn]] void err(const std::string& m) {
        throw std::runtime_error("expr: " + m + " in '" + s + "'");
    }
    static std::unique_ptr<Node> mk(Node::K k) { auto n = std::make_unique<Node>(); n->kind = k; return n; }
    std::unique_ptr<Node> bin(Node::K k, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        auto n = mk(k); n->a = std::move(a); n->b = std::move(b); return n;
    }

    int depth = 0;
    // RAII recursion-depth guard: every nesting level passes through unary(), so a cap there
    // bounds total recursion and rejects pathological input before the call stack overflows.
    struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { if (++d > 1000) throw std::runtime_error("expr: expression nested too deeply"); } ~DepthGuard() { --d; } };
    std::unique_ptr<Node> parse() { auto n = orx(); ws(); if (p != s.size()) err("trailing input"); return n; }
    std::unique_ptr<Node> orx() {
        auto a = andx();
        while (eat("|")) a = bin(Node::Or, std::move(a), andx());
        return a;
    }
    std::unique_ptr<Node> andx() {
        auto a = cmp();
        while (eat("&")) a = bin(Node::And, std::move(a), cmp());
        return a;
    }
    std::unique_ptr<Node> cmp() {
        // Left-associative like Stata: `1<2<3` parses as `(1<2)<3`, not a syntax
        // error. Keep `<=`/`>=` ahead of `<`/`>` so the two-char ops win.
        auto a = add();
        for (;;) {
            ws();
            if (eat("<=")) a = bin(Node::Le, std::move(a), add());
            else if (eat(">=")) a = bin(Node::Ge, std::move(a), add());
            else if (eat("==")) a = eqne(Node::Eq, std::move(a), add());
            else if (eat("!=")) a = eqne(Node::Ne, std::move(a), add());
            else if (eat("<")) a = bin(Node::Lt, std::move(a), add());
            else if (eat(">")) a = bin(Node::Gt, std::move(a), add());
            else break;
        }
        return a;
    }
    // Resolve `factorvar ==/!= "literal"` (either operand order) into a numeric
    // compare against the factor's 0-based level code. A literal that is not a
    // level makes == constant-false / != constant-true (no row can match).
    // Missing factor rows (code -1 -> NaN -> +inf under mi()) never equal a
    // finite level code, matching Stata's `"" == "x"` is false. A lone factor
    // or string operand, or a string/numeric mix, is a parse error.
    std::unique_ptr<Node> eqne(Node::K k, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        Node* fac = nullptr; Node* lit = nullptr;
        if (a->fcol && b->kind == Node::Str) { fac = a.get(); lit = b.get(); }
        else if (b->fcol && a->kind == Node::Str) { fac = b.get(); lit = a.get(); }
        if (fac) {
            long idx = -1;
            for (size_t i = 0; i < fac->fcol->levels.size(); ++i)
                if (fac->fcol->levels[i] == lit->sval) { idx = static_cast<long>(i); break; }
            if (idx < 0) {  // literal is not a level of this factor
                auto cst = mk(Node::Num); cst->num = (k == Node::Eq) ? 0.0 : 1.0; return cst;
            }
            auto v = mk(Node::Var);
            const auto& codes = fac->fcol->codes;  // CowVec<int32_t> (borrowed or owned)
            v->owned.resize(codes.size());
            for (size_t i = 0; i < codes.size(); ++i)
                v->owned[i] = codes[i] < 0 ? NAd : static_cast<double>(codes[i]);
            v->var = v->owned.data();
            auto num = mk(Node::Num); num->num = static_cast<double>(idx);
            return bin(k, std::move(v), std::move(num));
        }
        if (a->fcol || b->fcol || a->kind == Node::Str || b->kind == Node::Str)
            err("string/factor comparison must be `factorvar ==/!= \"literal\"`");
        return bin(k, std::move(a), std::move(b));
    }
    std::unique_ptr<Node> add() {
        auto a = mul();
        for (;;) { ws(); if (eat("+")) a = bin(Node::Add, std::move(a), mul());
            else if (eat("-")) a = bin(Node::Sub, std::move(a), mul()); else break; }
        return a;
    }
    std::unique_ptr<Node> mul() {
        auto a = powx();
        for (;;) { ws(); if (eat("*")) a = bin(Node::Mul, std::move(a), powx());
            else if (eat("/")) a = bin(Node::Div, std::move(a), powx()); else break; }
        return a;
    }
    std::unique_ptr<Node> powx() {
        auto a = unary();
        ws();
        if (eat("^")) return bin(Node::Pow, std::move(a), powx());
        return a;
    }
    std::unique_ptr<Node> unary() {
        DepthGuard _g(depth);
        ws();
        if (p < s.size() && s[p] == '!' && (p + 1 >= s.size() || s[p + 1] != '=')) {
            ++p; auto n = mk(Node::Not); n->a = unary(); return n;
        }
        if (eat("-")) { auto n = mk(Node::Neg); n->a = unary(); return n; }
        if (eat("+")) return unary();
        return primary();
    }
    std::vector<std::unique_ptr<Node>> arglist() {  // '(' already consumed
        std::vector<std::unique_ptr<Node>> v;
        ws();
        if (p < s.size() && s[p] == ')') { ++p; return v; }
        v.push_back(orx());
        while (eat(",")) v.push_back(orx());
        if (!eat(")")) err("expected , or )");
        return v;
    }
    std::unique_ptr<Node> primary() {
        ws();
        if (eat("(")) { auto n = orx(); if (!eat(")")) err("expected )"); return n; }
        if (p < s.size() && s[p] == '"') {  // string literal (factor equality only)
            ++p; size_t st = p;
            while (p < s.size() && s[p] != '"') ++p;
            if (p >= s.size()) err("unterminated string literal");
            auto n = mk(Node::Str); n->sval = s.substr(st, p - st); ++p; return n;
        }
        // Extended missing literal `.a` … `.z` — a '.' followed by exactly one
        // lowercase letter that does NOT continue into an identifier (so
        // `.abc` is not a missing, it stays an error, and a variable named
        // `a` is untouched because the '.' must lead).
        if (p + 1 < s.size() && s[p] == '.' && s[p + 1] >= 'a' && s[p + 1] <= 'z' &&
            (p + 2 >= s.size() ||
             (!std::isalnum((unsigned char)s[p + 2]) && s[p + 2] != '_' && s[p + 2] != '.'))) {
            int kind = cecon::kMissA + (s[p + 1] - 'a');
            p += 2;
            auto n = mk(Node::Num);
            n->num = cecon::stata_missing(kind);
            return n;
        }
        // bare missing literal: a '.' not part of a number
        if (p < s.size() && s[p] == '.' && (p + 1 >= s.size() || !std::isdigit((unsigned char)s[p + 1]))) {
            ++p; auto n = mk(Node::Num); n->num = NAd; return n;
        }
        if (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.')) {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '.' ||
                                    ((s[p] == '+' || s[p] == '-') && (s[p - 1] == 'e' || s[p - 1] == 'E'))))
                ++p;
            // Strict numeric literal (C1-F11): the greedy scan above grabs the whole
            // [alnum.] run, but std::stod silently consumes only a valid prefix and
            // drops the rest (5.5.5->5.5, 5foo->5, 1e->1, 1.2.3->1.2). Reject any token
            // std::stod does not fully consume, matching Stata's r(198). Hex forms (0x10)
            // are consumed WHOLE by strtod but Stata rejects them, so bar 'x'/'X' too.
            std::string tok = s.substr(st, p - st);
            for (char c : tok)
                if (c == 'x' || c == 'X') err("invalid numeric literal '" + tok + "'");
            size_t consumed = 0;
            double val;
            try { val = std::stod(tok, &consumed); }
            catch (const std::exception&) { err("invalid numeric literal '" + tok + "'"); }
            if (consumed != tok.size()) err("invalid numeric literal '" + tok + "'");
            auto n = mk(Node::Num); n->num = val; return n;
        }
        if (p < s.size() && (std::isalpha((unsigned char)s[p]) || s[p] == '_')) {
            size_t st = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) ++p;
            std::string id = s.substr(st, p - st);
            if (id == "_pi") { auto n = mk(Node::Num); n->num = M_PI; return n; }
            if (id == "_n") return mk(Node::ObsN);
            if (id == "_N") return mk(Node::ObsBigN);
            // BUG-369: time-series operators INSIDE an expression. `regress y L.x`
            // has always worked (model.cpp's ts_prepass materialises the varlist),
            // but `gen ly = L.y` did not — the identifier scan above stops at '.',
            // leaving a bare "L" that failed as an unknown variable. Same operators,
            // same ts_apply, same tsset metadata: only this wiring was missing, which
            // is what made it a defect rather than an unimplemented feature.
            //
            // Deliberately conservative: the TS reading is taken ONLY when the whole
            // prefix is a pure L/F/D/S chain AND the base variable exists. Anything
            // else rewinds and falls through to the pre-existing behaviour, so no
            // expression that parsed before can change meaning here.
            if (p < s.size() && s[p] == '.' && cecon::is_ts_prefix(id)) {
                size_t save = p;
                std::string tok = id;
                size_t q = p;
                while (q < s.size() && (std::isalnum((unsigned char)s[q]) ||
                                        s[q] == '_' || s[q] == '.')) {
                    tok += s[q];
                    ++q;
                }
                size_t dot = tok.rfind('.');
                std::string pre = (dot == std::string::npos) ? std::string() : tok.substr(0, dot);
                std::string base = (dot == std::string::npos) ? std::string() : tok.substr(dot + 1);
                if (!base.empty() && cecon::is_ts_prefix(pre) && fr.has(base)) {
                    p = q;
                    cecon::TsOpSpec spec = cecon::parse_tsop(tok);
                    if (!spec.ok) err("bad time-series operator in '" + tok + "': " + spec.err);
                    if (fr.panel_time().empty())
                        err("time-series operator '" + tok + "' used but no time variable is "
                            "set; run `tsset t` (or `xtset id t`) first");
                    // Time/panel/value must be numeric: a FACTOR column has an empty
                    // .f64, so .data() is nullptr and ts_apply would deref it. Same
                    // guard cmds_ts.cpp::cmd_tsops carries, same reason.
                    auto numeric = [&](const std::string& nm, const char* role) -> const double* {
                        if (fr.col(nm).is_factor)
                            err(std::string(role) + " '" + nm + "' is a factor but must be "
                                "numeric for a time-series operator");
                        return fr.col(nm).f64.data();
                    };
                    const double* basep = numeric(base, "variable");
                    const double* tmp = numeric(fr.panel_time(), "timevar");
                    const double* pnp = fr.panel_id().empty() ? nullptr
                                                              : numeric(fr.panel_id(), "panelvar");
                    std::vector<double> tsout;
                    std::string tserr;
                    // E1 holds by construction: this materialises over the frame as it
                    // stands, and `if`/keepif is a separate later op on the result.
                    if (!cecon::ts_apply(basep, fr.n_rows(), tmp, pnp, fr.ts_delta(), spec,
                                         tsout, tserr))
                        err("time-series operator '" + tok + "': " + tserr);
                    auto tsv = mk(Node::Var);
                    tsv->owned = std::move(tsout);
                    tsv->var = tsv->owned.data();
                    return tsv;
                }
                // Pure-TS prefix but the base column does not exist: name the BASE in
                // the error, not the operator letter. Falling through would report
                // "unknown variable L" for `L.nosuch`, which points at the wrong token.
                if (!base.empty() && cecon::is_ts_prefix(pre) && !fr.has(id))
                    err("unknown variable " + base);
                p = save;  // not a TS token after all — restore and fall through
            }
            ws();
            if (p < s.size() && s[p] == '(') {  // function call
                // B3(b): the LITERAL pseudo-functions. td/tc/tC/tm/tq/tw/th and
                // the single-letter synonyms d/m/q/w/h/y take an unquoted date
                // literal, not an expression: Stata answers r(198) "td(x)
                // invalid" for a variable argument while dofm(x) is fine. So the
                // argument text is lifted here, before the expression parser can
                // choke on `15mar2020`, and the whole call collapses to a
                // constant. Same table and same parser as PIET's copy
                // (cecon::literal_scale_for / stata_datetime_literal), so the two
                // evaluators cannot disagree about what `tq(2005q1)` means.
                // B3(c): tin(a,b) / twithin(a,b) -- "times in" / "times within".
                // Both take LITERALS on the tsset scale and expand, at parse
                // time, into an ordinary range test on the time variable, so no
                // opcode and no per-row machinery are needed. tin includes both
                // endpoints and twithin excludes them (measured 8 vs 6 over the
                // same 12-quarter series).
                //
                // The literal's meaning depends on the tsset variable's DISPLAY
                // FORMAT -- 2010q1 on a %tq series, 02jan2020 on a %td one --
                // which is a deliberate exception to the "format is carriage
                // only" rule in frame.h: for these two functions the format is
                // the semantics, and Stata reads it the same way. A format the
                // engine does not recognise falls back to a plain number, and a
                // literal that does not fit the scale is an error rather than a
                // quiet wrong window (Stata: r(198) for tin(2010,2011) on a
                // quarterly series). No tsset at all is r(101) there; here it is
                // an honest engine error with the same meaning.
                if (id == "tin" || id == "twithin") {
                    size_t q = p + 1;
                    int depth = 1;
                    while (q < s.size() && depth) {
                        if (s[q] == '(') ++depth;
                        else if (s[q] == ')') --depth;
                        if (depth) ++q;
                    }
                    if (depth) err("expected ) after " + id + "(");
                    const std::string raw = s.substr(p + 1, q - p - 1);
                    size_t comma = std::string::npos;
                    int d2 = 0;
                    for (size_t k = 0; k < raw.size(); ++k) {
                        if (raw[k] == '(') ++d2;
                        else if (raw[k] == ')') --d2;
                        else if (raw[k] == ',' && d2 == 0) { comma = k; break; }
                    }
                    if (comma == std::string::npos)
                        err(id + "() takes two arguments");
                    const std::string tv = fr.panel_time();
                    if (tv.empty())
                        err(id + "() requires tsset");
                    const std::string fmt = fr.ts_format();
                    auto conv = [&](std::string lit) -> double {
                        // trim
                        size_t b = lit.find_first_not_of(" \t");
                        size_t e2 = lit.find_last_not_of(" \t");
                        if (b == std::string::npos) return NAd;
                        lit = lit.substr(b, e2 - b + 1);
                        cecon::LiteralScale sc;
                        bool scaled = true;
                        if (fmt.rfind("%tq", 0) == 0) sc = cecon::LiteralScale::Quarter;
                        else if (fmt.rfind("%tm", 0) == 0) sc = cecon::LiteralScale::Month;
                        else if (fmt.rfind("%tw", 0) == 0) sc = cecon::LiteralScale::Week;
                        else if (fmt.rfind("%th", 0) == 0) sc = cecon::LiteralScale::Half;
                        else if (fmt.rfind("%tC", 0) == 0) sc = cecon::LiteralScale::ClockLeap;
                        else if (fmt.rfind("%tc", 0) == 0) sc = cecon::LiteralScale::Clock;
                        else if (fmt.rfind("%td", 0) == 0) sc = cecon::LiteralScale::Day;
                        else scaled = false;   // %ty, %9.0g, unknown: a plain number
                        if (!scaled) {
                            try {
                                size_t used = 0;
                                double v = std::stod(lit, &used);
                                return used == lit.size() ? v : NAd;
                            } catch (const std::exception&) { return NAd; }
                        }
                        return cecon::stata_datetime_literal(lit, sc);
                    };
                    const double lo = conv(raw.substr(0, comma));
                    const double hi = conv(raw.substr(comma + 1));
                    if (std::isnan(lo) || std::isnan(hi))
                        err(id + "(" + raw + ") does not match the tsset scale");
                    p = q + 1;
                    auto mkvar = [&]() {
                        auto v = mk(Node::Var);
                        v->var = fr.col(tv).f64.data();
                        return v;
                    };
                    auto mknum = [&](double x) { auto n2 = mk(Node::Num); n2->num = x; return n2; };
                    const bool strict = (id == "twithin");
                    auto left = mk(strict ? Node::Gt : Node::Ge);
                    left->a = mkvar(); left->b = mknum(lo);
                    auto right = mk(strict ? Node::Lt : Node::Le);
                    right->a = mkvar(); right->b = mknum(hi);
                    auto both = mk(Node::And);
                    both->a = std::move(left); both->b = std::move(right);
                    return both;
                }
                cecon::LiteralScale ls;
                if (cecon::literal_scale_for(id, ls)) {
                    size_t q = p + 1;
                    int depth = 1;
                    while (q < s.size() && depth) {
                        if (s[q] == '(') ++depth;
                        else if (s[q] == ')') --depth;
                        if (depth) ++q;
                    }
                    if (depth) err("expected ) after " + id + "(");
                    const std::string raw = s.substr(p + 1, q - p - 1);
                    const double lit = cecon::stata_datetime_literal(raw, ls);
                    // An unparseable literal is an ERROR, not missing -- this is
                    // the one place in the date family where Stata refuses
                    // rather than answering `.` (r(198)).
                    if (std::isnan(lit)) err(id + "(" + raw + ") invalid");
                    p = q + 1;
                    auto n = mk(Node::Num);
                    n->num = lit;
                    return n;
                }
                const FnInfo* fi = find_fn(id);
                if (!fi) err("unknown function " + id);
                ++p;
                auto args = arglist();
                int na = static_cast<int>(args.size());
                if (na < fi->amin || na > fi->amax) err("wrong number of arguments to " + id);
                auto n = mk(Node::Fn); n->fn = fi->id; n->args = std::move(args);
                // group() under `in a/b`: Stata renumbers within the range (measured,
                // `group(3) in 3/7` on N = 7 is . . 1 1 2 3 3), but PIET folds the range
                // into this expression as the literal `_n>=a & _n<=b` (egen.cpp,
                // runBysortGen), indistinguishable here from an `if` that does NOT
                // renumber. Refuse that shape rather than answer with the dataset's
                // numbering; a spaced user `if _n >= a` is unaffected.
                if (n->fn == F_group && s.find("_n>=") != std::string::npos &&
                    s.find("_n<=") != std::string::npos)
                    err("group() cannot be combined with an `in` range (or an `_n>=`/`_n<=` "
                        "condition): Stata numbers group() within the range");
                if ((n->fn == F_rnormal || n->fn == F_uniform) && !g_rng_coords_set)
                    err(id + "() needs draw coordinates: the call carried no --rng-seed/"
                        "--rng-stream, and without them two successive draws would repeat "
                        "each other");
                // Every draw site gets its own ordinal, so two runiform() calls in
                // one expression are independent rather than identical.
                if (n->fn == F_runiform || n->fn == F_runiformint || n->fn == F_rnormal ||
                    n->fn == F_uniform)
                    n->num = static_cast<double>(g_rng_node_counter++);
                return n;
            }
            if (!fr.has(id)) err("unknown variable " + id);
            const FrameColumn& c = fr.col(id);
            if (c.is_factor) {
                // Factor reference: leave var null, carry the column so cmp()
                // can resolve `factorvar ==/!= "literal"`. Any numeric use of it
                // falls through to ev()'s null-var guard.
                auto fv = mk(Node::Var); fv->fcol = &c; return fv;
            }
            auto v = mk(Node::Var); v->var = c.f64.data();
            ws();
            if (p < s.size() && s[p] == '[') {  // subscript
                ++p; auto idx = orx(); if (!eat("]")) err("expected ]");
                auto sub = mk(Node::Subscript); sub->a = std::move(v); sub->b = std::move(idx);
                return sub;
            }
            return v;
        }
        err("unexpected token");
    }
};

// Stata percentile of an ascending-sorted non-missing vector (unit weights).
// Shared by egen median (p=50) and summarize,detail.
double stata_pctile(const std::vector<double>& sorted, double p) {
    int64_t N = static_cast<int64_t>(sorted.size());
    if (N == 0) return NAd;
    if (N == 1) return sorted[0];
    double Pi = N * p / 100.0;
    double fl = std::floor(Pi);
    if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < N)
        return (sorted[static_cast<int64_t>(Pi) - 1] + sorted[static_cast<int64_t>(Pi)]) / 2.0;
    int64_t idx = static_cast<int64_t>(fl) + 1;
    if (idx > N) idx = N;
    if (idx < 1) idx = 1;
    return sorted[idx - 1];
}

// Weighted Stata percentile (W:P5). `sorted` is ascending non-missing values
// paired index-for-index with their (positive) weights; W is their sum.
//
// This is the same rule stata_pctile above implements, written in cumulative
// weight rather than in rank — [R] summarize's weighted case is the unweighted
// one with the running count replaced by the running weight. Target = p·W/100;
// walk until the cumulative weight reaches it. Landing EXACTLY on a boundary
// averages with the next distinct observation (the unweighted branch's
// `fl == Pi` case); otherwise the observation that straddles the target is the
// percentile. With every wᵢ == 1 this reduces to stata_pctile term for term,
// which is what the aw/fw parity checks pin.
double stata_pctile_weighted(const std::vector<double>& sorted, const std::vector<double>& w,
                             double W, double p) {
    const int64_t N = static_cast<int64_t>(sorted.size());
    if (N == 0 || W <= 0.0) return NAd;
    if (N == 1) return sorted[0];
    const double target = W * p / 100.0;
    // Tolerance for "cumulative weight landed exactly on the target": weights
    // are doubles, so an exact == would make the boundary case unreachable for
    // non-integer weights while firing for integer ones (fweight). Scale it to
    // the magnitudes involved rather than using a fixed epsilon.
    const double tol = 1e-9 * std::max(1.0, std::fabs(W));
    double cum = 0.0;
    for (int64_t i = 0; i < N; ++i) {
        cum += w[i];
        if (cum > target + tol) return sorted[i];
        if (std::fabs(cum - target) <= tol) {
            // Boundary: average with the next observation, mirroring the
            // unweighted midpoint rule. At the top edge there is no next one.
            return (i + 1 < N) ? (sorted[i] + sorted[i + 1]) / 2.0 : sorted[i];
        }
    }
    return sorted[N - 1];
}

}  // namespace

// The published view of FNS above — same table, opcode ids dropped. Built once, in
// table order. Defined here rather than beside the dump's caller so there is exactly
// one list of function names in the engine: the one the parser resolves against.
void set_rng_coords(long long seed, long long stream) {
    g_rng_seed = static_cast<int64_t>(seed);
    g_rng_stream = static_cast<int64_t>(stream);
    g_rng_coords_set = true;
    // Node ordinals restart with every set of coordinates, so a draw is a function
    // of this call's argv alone. They used to run on for the life of the process:
    // under `cobra serve` the n-th draw site of a request got ordinal (all draw sites
    // any earlier request had parsed) + n, so the same (seed, stream) answered
    // differently depending on the resident process's history. Unchanged for a
    // one-shot process, whose counter starts at 0 anyway.
    g_rng_node_counter = 0;
}

const std::vector<ExprFnInfo>& expr_function_table() {
    static const std::vector<ExprFnInfo> T = [] {
        std::vector<ExprFnInfo> v;
        v.reserve(sizeof(FNS) / sizeof(FNS[0]) + 12);
        for (const FnInfo& f : FNS) v.push_back({f.name, f.amin, f.amax});
        // B3(b): the literal pseudo-functions are resolved in the PARSER, not
        // through FNS[], so a census built from the table alone reports them as
        // unsupported while `gen x = tq(2005q1)` works. They are published here
        // because this list answers "what names does the expression language
        // accept", and the answer includes these. Arity is 1: one literal.
        static const char* kLiteralFns[] = {"td", "tc", "tC", "tm", "tq", "tw",
                                            "th", "d", "m", "q", "w", "h", "y"};
        for (const char* n : kLiteralFns) v.push_back({n, 1, 1});
        // tin()/twithin() are expanded in the parser too -- into a range test on
        // the tsset variable -- so they are invisible to a table-only census for
        // the same reason.
        v.push_back({"tin", 2, 2});
        v.push_back({"twithin", 2, 2});
        return v;
    }();
    return T;
}

// ONESHOT-RAMDIET A: which columns does a run actually READ? The wave's premise is
// that most of a wide frame is materialized and then discarded unread, and that is a
// claim about behaviour, not about the do-file — so it gets counted here, at the one
// accessor every engine-side read passes through. ACQUISITIONS only: the counter never
// touches a payload byte, which keeps it inside the rule a per-item diagnostic already
// broke once (a payload-touching walk cost a2_mini +11.6% wall / +98k faults).
// ONESHOT-RAMDIET WS-A census: the tally lives in io/columnar.cpp (both read gates
// need it, and piet_native compiles that TU standalone — a Frame symbol there is an
// unresolvable link). These are the Frame-side forwarding shims.
void Frame::note_materialization(int64_t ncols) {
    cecon_coltouch_generation(ncols);
    cecon_ladder_probe_close();   // LADDER PROBE rides the same generation boundary
}

void Frame::record_column_touch(const std::string& name) {
    cecon_coltouch_note(name);
}

void Frame::type_census(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "a");   // APPEND: the resident changes shape
    if (f == nullptr) return;                      // through a run and the BIG frame is the target
    // Stata's ranges, copied from the writer's ladder (dta_writer.cpp:48-50) so the
    // census cannot promise a demotion the real ladder would refuse.
    const double kByteLo = -127, kByteHi = 100;
    const double kIntLo = -32767, kIntHi = 32740;
    const double kLongLo = -2147483647, kLongHi = 2147483620;
    int64_t tot8 = 0, totNarrow = 0;
    for (const FrameColumn& c : cols_) {
        if (c.is_factor) continue;  // already int32 codes; the ladder is about f64 columns
        const F64Reader v = c.f64.reader();   // LADDER: a diagnostic must not widen what it measures
        bool anyFinite = false, allIntegral = true, f32ok = true;
        double lo = 0, hi = 0;
        for (int64_t i = 0; i < n_; ++i) {
            const double x = v[static_cast<std::size_t>(i)];
            if (std::isnan(x)) continue;
            if (!anyFinite) { lo = hi = x; anyFinite = true; }
            else { if (x < lo) lo = x; if (x > hi) hi = x; }
            if (x != std::floor(x)) allIntegral = false;
            if (f32ok && static_cast<double>(static_cast<float>(x)) != x) f32ok = false;
        }
        const char* kind = "double";
        int width = 8;
        if (!anyFinite) { kind = "byte"; width = 1; }
        else if (allIntegral && lo >= kByteLo && hi <= kByteHi) { kind = "byte"; width = 1; }
        else if (allIntegral && lo >= kIntLo && hi <= kIntHi) { kind = "int"; width = 2; }
        else if (allIntegral && lo >= kLongLo && hi <= kLongHi) { kind = "long"; width = 4; }
        else if (f32ok) { kind = "float"; width = 4; }
        tot8 += n_ * 8;
        totNarrow += n_ * width;
        std::fprintf(f, "col %s rows=%lld now=8 narrow=%d kind=%s\n", c.name.c_str(),
                     static_cast<long long>(n_), width, kind);
    }
    std::fprintf(f, "TOTAL cols=%zu f64_bytes=%lld narrow_bytes=%lld saving_pct=%.1f\n", cols_.size(),
                 static_cast<long long>(tot8), static_cast<long long>(totNarrow),
                 tot8 > 0 ? 100.0 * (tot8 - totNarrow) / static_cast<double>(tot8) : 0.0);
    std::fclose(f);
}

const FrameColumn& Frame::col(const std::string& name) const {
    auto it = idx_.find(name);
    if (it == idx_.end()) throw std::runtime_error("no column: " + name);
    record_column_touch(name);
    return cols_[it->second];
}

const double* Frame::require_f64(const std::string& name) const {
    const FrameColumn& c = col(name);
    if (c.is_factor) throw std::runtime_error("column '" + name + "' is a factor, not f64");
    return c.f64.data();
}

void Frame::reindex() {
    idx_.clear();
    for (int i = 0; i < static_cast<int>(cols_.size()); ++i) idx_[cols_[i].name] = i;
}

// ---- sort-state plane (BUG-272; contract in frame.h) ------------------------------

// The claim's on-disk .hbin spelling — the varlist space-joined into ONE root-level
// string (Stata variable names cannot contain spaces, so the join is unambiguous and
// splits back losslessly). Shared by write_hbin_full / patch_hbin / as_dataset_view
// so the three carriers cannot drift.
// join_sort_keys: single source in frame_internal.h (R2 split)
using frame_detail::join_sort_keys;

void Frame::set_sorted_by(std::vector<std::string> keys) {
    // A claim naming a column this Frame does not have (or naming one twice) can
    // never be honored by a writer and could only have come from a malformed header
    // or a stale caller — install nothing rather than a claim we cannot stand behind.
    // "No claim" is always a safe answer; a wrong one never is.
    std::unordered_set<std::string> seen;
    for (const std::string& k : keys) {
        if (idx_.find(k) == idx_.end() || !seen.insert(k).second) {
            sort_keys_.clear();
            return;
        }
    }
    sort_keys_ = std::move(keys);
}

void Frame::clear_claim_if_key(const std::string& name) {
    if (sort_keys_.empty()) return;
    for (const std::string& k : sort_keys_) {
        if (k == name) {
            sort_keys_.clear();
            return;
        }
    }
}

FrameColumn& Frame::mutable_col(const std::string& name) {
    auto it = idx_.find(name);
    if (it == idx_.end()) throw std::runtime_error("no column: " + name);
    // BUG-272: the caller gets a MUTABLE reference and may rewrite the column's
    // values (the .dta reader's %tc rescale does exactly that) — if that column is a
    // sort-claim key, the claim is now in doubt, and doubt clears. Over-clearing here
    // costs a blank "Sorted by:"; metadata-only callers that must keep a claim can
    // re-install it afterwards (read_dta sets its claim AFTER its mutable_col passes).
    clear_claim_if_key(name);
    return cols_[it->second];
}

void Frame::append_f64_column(const std::string& name, std::vector<double> v) {
    FrameColumn c;
    c.name = name;
    c.is_factor = false;
    c.f64 = std::move(v);
    cols_.push_back(std::move(c));
}

// LADDER: bulk-append an already-narrow payload. Only take_rows calls this — the
// gathered values come from a column that already passed the float-exactness test,
// and a subset of exact values is exact.
void Frame::append_f64_column_narrow(const std::string& name, std::vector<float> v) {
    FrameColumn c;
    c.name = name;
    c.is_factor = false;
    c.f64.set_narrow_f32(std::move(v));
    cols_.push_back(std::move(c));
}

void Frame::append_factor_column(const std::string& name, const std::vector<std::string>& values,
                                 bool empty_is_missing) {
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes.resize(values.size());
    std::unordered_map<std::string, int32_t> dict;
    for (size_t i = 0; i < values.size(); ++i) {
        if (empty_is_missing && values[i].empty()) { c.codes[i] = -1; continue; }  // empty string ==
                                                                // missing (matches Frame::from_csv /
                                                                // pandas.factorize use_na_sentinel
                                                                // convention). Callers that need the
                                                                // .dta convention (empty string is a
                                                                // genuine, non-missing level — BUG-024)
                                                                // pass empty_is_missing=false.
        auto it = dict.find(values[i]);
        if (it == dict.end()) {
            int32_t code = static_cast<int32_t>(c.levels.size());
            dict.emplace(values[i], code);
            c.levels.push_back(values[i]);
            c.codes[i] = code;
        } else {
            c.codes[i] = it->second;
        }
    }
    cols_.push_back(std::move(c));
}

void Frame::append_factor_column_coded(const std::string& name, std::vector<int32_t> codes,
                                       std::vector<std::string> levels) {
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes = std::move(codes);
    c.levels = std::move(levels);
    cols_.push_back(std::move(c));
}

void Frame::borrow_columns_from(const Dataset& s) {
    n_ = s.n_rows();
    for (const Column& c : s.columns()) {
        FrameColumn fc;
        fc.name = c.name;
        fc.format = c.format;  // P1-A0: optional display format from the header ("" if absent)
        fc.label = c.label;    // BUG-198: same optionality, same "" = absent
        if (c.dtype == DType::Factor) {
            fc.is_factor = true;
            fc.codes.borrow(c.codes(), n_);  // clean: reference the source span, no copy
            // MAXPERF Stage 2 Wave β1: borrow the Dataset's own level strings instead of
            // deep-copying the dictionary (was ~0.4s for a 108K-string factor column).
            // Same lifetime contract as the codes/f64 borrows above: `s` (and whatever it
            // borrowed from) must outlive this Frame until a write materializes it.
            fc.levels.borrow(c.levels.data(), static_cast<int64_t>(c.levels.size()));
        } else if (c.narrow == Narrow::F32) {
            fc.f64.borrow_narrow_f32(c.f32(), n_);  // LADDER: borrow narrow, never widen here
        } else {
            fc.f64.borrow(c.f64(), n_);  // clean: reference the source span, no copy
        }
        cols_.push_back(std::move(fc));
    }
    reindex();
    // BUG-272: adopt the file's sort claim (root "sorted_by" header key, space-
    // joined). AFTER reindex(), because set_sorted_by validates every key against
    // idx_ — a malformed/stale claim installs nothing rather than a lie.
    if (!s.sorted_by().empty()) {
        std::vector<std::string> keys;
        const std::string& sb = s.sorted_by();
        size_t b = 0;
        while (b < sb.size()) {
            size_t e = sb.find(' ', b);
            if (e == std::string::npos) e = sb.size();
            if (e > b) keys.push_back(sb.substr(b, e - b));
            b = e + 1;
        }
        set_sorted_by(std::move(keys));
    }
}

Frame Frame::from_dataset(const Dataset& ds) {
    Frame f;
    f.src_path_ = ds.path();  // remember the source so a clean save can patch it
    if (ds.owns_mmap()) {
        // Hold an independent mmap of the same file alive for this Frame's lifetime, so
        // the borrowed column spans stay valid even after the caller's `ds` is destroyed
        // (e.g. a factory that returns the Frame — see tests/unit/test_d2.cpp makeframe).
        // mmap of the same file shares OS pages, so this is O(1) with no data copy. This
        // is the "shared_ptr<const Dataset> or equivalent" the design calls for.
        f.src_ = std::make_shared<Dataset>(Dataset::open(ds.path()));
        f.borrow_columns_from(*f.src_);
    } else {
        // `ds` is a non-owning view (Dataset::make_view — the serve resident frame's
        // view, contract_A §26): borrow its column spans directly. src_ stays null;
        // the caller keeps the backing buffers (the resident Frame) alive, and NOTHING
        // is re-opened — so a root file the client deleted mid-session can't break this.
        f.borrow_columns_from(ds);
    }
    return f;
}

Frame Frame::from_dataset_shared(std::shared_ptr<const Dataset> sp) {
    Frame f;
    f.src_ = std::move(sp);
    f.src_path_ = f.src_->path();
    f.borrow_columns_from(*f.src_);
    return f;
}

namespace {
// One view Column from one FrameColumn — shared by as_dataset_view (every
// column) and patch_dataset_view (changed columns only). For a factor column
// this is the expensive call: `.ref()` deep-copies the whole level dictionary
// into the Column's owned vector, and — required, not incidental — materializes
// fc's dictionary IN PLACE first if it was still borrowed, because a
// still-borrowed fc.levels may be borrowing the very view Dataset the caller is
// about to replace or overwrite (see ServeSession::rebuild_view). Build order
// in the patch path leans on exactly that: the new Column is FULLY built before
// the old view entry it replaces is touched.
Column view_column_of(const FrameColumn& fc) {
    Column c;
    c.name = fc.name;
    c.view_gen = fc.data->mut_gen;  // MECHOPT W2: the identity the patcher matches on
    c.format = fc.format;  // P1-A0: the resident view carries the format too, so a
                           // serve session's Datastore sees the same string a
                           // disk-loaded one does (contract_A §26 path parity)
    c.label = fc.label;    // BUG-198: the label layer rides the same parity
    if (fc.is_factor) {
        c.dtype = DType::Factor;
        c.data = static_cast<const void*>(fc.codes.data());  // const CowVec: no materialize
        c.nbytes = static_cast<int64_t>(fc.codes.size()) * 4;
        c.levels = fc.levels.ref();
    } else if (fc.f64.narrow()) {
        // LADDER: pass the narrow bytes through. Widening here is what made attempts
        // 1-6 vacuous — every view build re-widened the whole frame. The LOGICAL
        // dtype is unchanged; only storage narrows, and Column::f64() widens lazily
        // for the few consumers that genuinely need contiguous doubles.
        c.dtype = DType::F64;
        c.narrow = Narrow::F32;
        c.data = static_cast<const void*>(fc.f64.f32_ref().data());
        c.nbytes = static_cast<int64_t>(fc.f64.f32_ref().size()) * 4;
    } else {
        c.dtype = DType::F64;
        c.data = static_cast<const void*>(fc.f64.data());
        c.nbytes = static_cast<int64_t>(fc.f64.size()) * 8;
    }
    return c;
}
}  // namespace

Dataset Frame::as_dataset_view() const {
    std::vector<Column> cols;
    cols.reserve(cols_.size());
    for (const FrameColumn& fc : cols_) cols.push_back(view_column_of(fc));
    // BUG-272: the view carries the claim (same parity rationale as format/label
    // above — a Frame rebuilt over this view via from_dataset must see the same
    // claim a disk round trip would deliver).
    return Dataset::make_view(src_path_, n_, std::move(cols), join_sort_keys(sort_keys_));
}

bool Frame::patch_dataset_view(Dataset& view) const {
    // Preconditions under which an in-place patch is even meaningful. Row-count
    // change, a different source path, a different sort claim, or a real
    // mmap-backed Dataset all mean "this is not an incremental step of the same
    // view" — the caller full-rebuilds, which is also the only path that can
    // change those three fields (make_view stamps them at construction).
    if (view.owns_mmap()) return false;
    if (view.n_rows() != n_) return false;
    if (view.path() != src_path_) return false;
    if (view.sorted_by() != join_sort_keys(sort_keys_)) return false;

    // Assemble the new column vector in THIS frame's order, MOVING every entry
    // whose identity is unchanged and rebuilding the rest. Identity is the same
    // payload-pointer test the serve commit's changed/added delta uses
    // (cecon_cli fcol_ptr): an untouched column's pending twin borrows the very
    // buffer the old view entry points at, and adopt_resident_predecessor
    // preserves that pointer across the swap by SHARING the ColumnData — so
    // pointer equality here is exactly "nothing about this column's bytes
    // moved". format/label ride separately from the payload (a label/format
    // command changes them with the pointer unchanged), so they are compared
    // explicitly; nbytes changes only with row count or dtype width, both of
    // which also move the pointer, but the compare is one integer — kept for
    // the same self-healing reason. Add / drop / reorder need no special
    // cases: the walk is in frame order and anything unmatched is rebuilt.
    const std::vector<Column>& old = view.columns();
    std::unordered_map<uint64_t, const Column*> by_gen;
    by_gen.reserve(old.size());
    for (const Column& oc : old)
        if (oc.view_gen != 0) by_gen.emplace(oc.view_gen, &oc);

    std::vector<Column> next;
    next.reserve(cols_.size());
    for (const FrameColumn& fc : cols_) {
        const void* payload =
            fc.is_factor    ? static_cast<const void*>(fc.codes.data())
          : fc.f64.narrow() ? static_cast<const void*>(fc.f64.f32_ref().data())
                            : static_cast<const void*>(fc.f64.data());
        // Identity = the payload GENERATION, not the pointer. A pointer match
        // is vulnerable to allocator address reuse (drop a factor column, mint
        // another of the same rows: the codes buffer can land on the freed
        // address while the view entry still holds the OLD dictionary) and to
        // in-place writes on a uniquely-held column (same address, new bytes).
        // The generation is stamped at every ColumnData construction and
        // bumped at every mutable access (detach), so it never repeats; the
        // pointer/name/format/label compares below are belt over braces.
        auto it = by_gen.find(fc.data->mut_gen);
        const Column* oc = (it != by_gen.end()) ? it->second : nullptr;
        const DType want = fc.is_factor ? DType::Factor : DType::F64;
        const bool fc_narrow = !fc.is_factor && fc.f64.narrow();
        const int64_t wantBytes =
            fc.is_factor ? static_cast<int64_t>(fc.codes.size()) * 4
          : fc_narrow    ? static_cast<int64_t>(fc.f64.f32_ref().size()) * 4
                         : static_cast<int64_t>(fc.f64.size()) * 8;
        const Narrow wantNarrow = fc_narrow ? Narrow::F32 : Narrow::None;
        if (oc != nullptr && oc->data == payload && oc->dtype == want && oc->nbytes == wantBytes &&
            oc->narrow == wantNarrow &&
            oc->name == fc.name && oc->format == fc.format && oc->label == fc.label) {
            // Unchanged in every dimension the view carries — reuse the entry.
            // The move is O(1) (the level vector's buffer changes hands); the
            // source entry in `old` is left moved-from, which is fine: `old` is
            // wholesale-replaced by reset_view_columns below and never read
            // again. const_cast is confined to exactly this "about to be
            // discarded" vector.
            next.push_back(std::move(*const_cast<Column*>(oc)));
        } else {
            next.push_back(view_column_of(fc));
        }
    }
    return view.reset_view_columns(std::move(next));
}

void Frame::adopt_resident_predecessor(Frame& outgoing) {
    // Each pending column that still BORROWS a span whose bytes live in `outgoing`'s
    // OWNED buffer would dangle once `outgoing` is destroyed at the swap. Reattach it
    // by ADOPTING (sharing) outgoing's ColumnData: O(1), the data pointer is preserved
    // (the pending column borrowed exactly that buffer), and — unlike the old
    // buffer-level std::move steal — it is safe even when a stash / tempfile snapshot
    // ALSO shares that payload, because column-CoW then protects every sharer on the
    // next mutation (a steal-move would have emptied the shared ColumnData in place).
    // The pending→resident path builds pending's columns by BORROWING raw spans of the
    // resident's as_dataset_view() (fresh ColumnData that only borrows the bytes by
    // pointer), so the borrow-into-outgoing-owned-buffer hazard is NOT eliminated by
    // the sharing layer — this reattach stays required; only its mechanism simplifies.
    // Columns borrowing the shared ROOT mmap stay borrowed (the session keeps the root
    // Dataset alive; also inherited here via src_). Cost is O(columns touched).
    // Matched by PAYLOAD POINTER, not by name. The question this loop asks is
    // "whose buffer am I borrowing?", and a buffer's identity is its address —
    // the name was only ever a fast way to guess at it, and it guesses WRONG for
    // the one verb that changes a name while keeping the bytes. `rename` on the
    // resident frame produced a pending column whose new name matches no
    // outgoing column, so the lookup missed, the adopt was skipped, and the
    // column went on borrowing a buffer that was destroyed at the swap: the
    // renamed variable read back as uninitialised memory (measured:
    // `gen double side = …` then `rename side other_side` then `summarize`
    // returned 3.4e+110). The payload check below was already the real
    // predicate — the name lookup was the only thing standing in front of it.
    std::unordered_map<const void*, FrameColumn*> by_payload;
    by_payload.reserve(outgoing.cols_.size());
    for (FrameColumn& oc : outgoing.cols_) {
        if (oc.borrowed()) continue;  // outgoing also borrows (root) → nothing of its own to adopt
        by_payload.emplace(oc.payload(), &oc);
    }
    for (FrameColumn& nc : cols_) {
        if (!nc.borrowed()) continue;                 // we own it → nothing can dangle
        auto it = by_payload.find(nc.payload());
        if (it == by_payload.end()) continue;         // not borrowing an outgoing-owned buffer
        FrameColumn& oc = *it->second;
        if (oc.is_factor != nc.is_factor) continue;
        nc.data = oc.data;                            // adopt oc's payload by sharing (keeps its buffer alive)
    }
    // Inherit the root-mmap keepalive so columns still borrowing it survive the swap.
    if (!src_ && outgoing.src_) src_ = outgoing.src_;
    // BUG-272: the sort claim needs NO action here, and that is a judgment, not an
    // omission — this reattach changes buffer OWNERSHIP only (pointer-preserving
    // adopt), never a value, a name, or a row position. This frame keeps its own
    // claim; outgoing's claim dies with outgoing, exactly as its buffers would have.
}

namespace {
// Is a trimmed cell a "missing" marker for a numeric column?
bool csv_is_missing(const std::string& s) {
    return s.empty() || s == "." || s == "NA" || s == "N/A" || s == "na" ||
           s == "NaN" || s == "nan" || s == "null" || s == "NULL";
}
// Parse a full cell as a double; returns false if not entirely numeric.
bool csv_parse_num(const std::string& s, double& out) {
    const char* b = s.c_str();
    char* end = nullptr;
    out = std::strtod(b, &end);
    if (end == b) return false;
    while (*end == ' ' || *end == '\t') ++end;  // allow trailing space
    return *end == '\0';
}
// Read one logical CSV record into `fields`. Handles RFC-4180 quoting (a quoted
// field may contain the delimiter and embedded newlines; "" is a literal quote)
// and LF/CRLF endings. Returns false at clean EOF. `*was_quoted_or_delimited`
// (when non-null) is set true iff the record contains at least one delimiter
// or quote character — i.e. it is a REAL single-column-with-empty-value row,
// not a genuinely empty physical line. Distinguishing the two matters for a
// single-column CSV: `df.to_csv(na_rep="")` on a length-1 float column quotes
// the missing row as `""` (a real, delimited-looking field), which must NOT be
// treated the same as a truly blank line between records.
//
// BUG-292: `field_quoted` (when non-null) reports, PER FIELD, whether that field
// was written inside double quotes. RFC-4180 says a quoted field's content is
// literal, and this reader's caller trims every cell — so a value whose padding
// is part of the value (`tostring, usedisplayformat` under a left-justified
// format renders `42.50    `, len 9) arrived with its pad eaten. The flag is how
// the trim learns to leave a quoted cell alone.
bool csv_read_record(std::istream& in, char delim, std::vector<std::string>& fields,
                     bool* was_quoted_or_delimited = nullptr,
                     std::vector<char>* field_quoted = nullptr) {
    fields.clear();
    if (field_quoted) field_quoted->clear();
    std::string field;
    bool in_quotes = false, any = false, saw_structure = false, cur_quoted = false;
    int ch;
    auto endField = [&]() {
        fields.push_back(field);
        if (field_quoted) field_quoted->push_back(cur_quoted ? 1 : 0);
        field.clear();
        cur_quoted = false;
    };
    while ((ch = in.get()) != EOF) {
        any = true;
        char c = static_cast<char>(ch);
        if (in_quotes) {
            if (c == '"') {
                if (in.peek() == '"') { field.push_back('"'); in.get(); }
                else in_quotes = false;
            } else field.push_back(c);
        } else {
            if (c == '"') { in_quotes = true; saw_structure = true; cur_quoted = true; }
            else if (c == delim) { endField(); saw_structure = true; }
            else if (c == '\n') { endField(); if (was_quoted_or_delimited) *was_quoted_or_delimited = saw_structure; return true; }
            else if (c == '\r') { /* swallow; CRLF or lone CR */ }
            else field.push_back(c);
        }
    }
    if (!any) return false;
    endField();  // final record with no trailing newline
    if (was_quoted_or_delimited) *was_quoted_or_delimited = saw_structure;
    return true;
}
std::string csv_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}
// BUG-292: trim UNQUOTED cells only. A quoted cell is literal by RFC-4180, and
// PIET's setcol/tostring transport quotes every cell it writes, so this is what
// makes cell-edge whitespace survive the trip to the engine.
std::string csv_trim_unquoted(const std::string& s, bool quoted) {
    return quoted ? s : csv_trim(s);
}
}  // namespace

Frame Frame::from_csv(const std::string& path, char delim,
                      const std::vector<std::string>& force_factor) {
    cecon::engctr::addDataFileOpen();  // WS-D2: a .csv/.tsv `using` file is a physical open too
    std::ifstream in(to_native_path(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open CSV: " + path);
    std::vector<std::string> header;
    if (!csv_read_record(in, delim, header)) throw std::runtime_error("empty CSV: " + path);
    const int K = static_cast<int>(header.size());
    // Strip a leading UTF-8 BOM (EF BB BF) baked into the first header cell so the
    // first variable name is usable — Stata's `import delimited` drops it likewise.
    if (!header.empty() && header[0].size() >= 3 &&
        static_cast<unsigned char>(header[0][0]) == 0xEF &&
        static_cast<unsigned char>(header[0][1]) == 0xBB &&
        static_cast<unsigned char>(header[0][2]) == 0xBF)
        header[0].erase(0, 3);
    for (auto& h : header) h = csv_trim(h);

    std::vector<char> is_num(K, 1);  // pass-1: assume numeric until proven otherwise
    for (const auto& ff : force_factor) {
        for (int k = 0; k < K; ++k) if (header[k] == ff) is_num[k] = 0;
    }
    // --- pass 1: infer column types + count rows ---
    // A record is a genuinely blank line (skippable) only when it has a single
    // empty field AND that field carried no CSV structure (no delimiter, no
    // quotes) — for a single-column file (K==1), a real data row whose lone
    // value is empty (e.g. `df.to_csv(na_rep="")` quotes it as `""`, which DOES
    // carry structure) must still count as a row, or every all-missing row in
    // a 1-column CSV silently vanishes (undercounting nrows / misaligning any
    // caller that assumes CSV row order == source row order).
    int64_t nrows = 0;
    std::vector<std::string> rec;
    std::vector<char> quoted;
    bool structured;
    while (csv_read_record(in, delim, rec, &structured, &quoted)) {
        if (rec.size() == 1 && rec[0].empty() && !structured) continue;  // truly blank line
        ++nrows;
        for (int k = 0; k < K && k < static_cast<int>(rec.size()); ++k) {
            if (!is_num[k]) continue;
            // Type INFERENCE still trims either way: csv_parse_num tolerates the
            // padding, and a quoted ` 42 ` is a number in any reading.
            std::string cell = csv_trim(rec[k]);
            if (csv_is_missing(cell)) continue;
            double v;
            if (!csv_parse_num(cell, v)) is_num[k] = 0;
        }
    }
    // --- pass 2: materialize ---
    in.clear();
    in.seekg(0);
    csv_read_record(in, delim, rec);  // skip header

    Frame f;
    f.n_ = nrows;
    f.cols_.resize(K);
    std::vector<std::unordered_map<std::string, int32_t>> dict(K);
    for (int k = 0; k < K; ++k) {
        f.cols_[k].name = header[k];
        if (is_num[k]) f.cols_[k].f64.resize(static_cast<size_t>(nrows));
        else { f.cols_[k].is_factor = true; f.cols_[k].codes.resize(static_cast<size_t>(nrows)); }
    }
    int64_t row = 0;
    while (csv_read_record(in, delim, rec, &structured, &quoted)) {
        if (rec.size() == 1 && rec[0].empty() && !structured) continue;  // truly blank line
        for (int k = 0; k < K; ++k) {
            // BUG-292: a QUOTED cell keeps its edge whitespace (it is part of the
            // value); an unquoted one is trimmed exactly as before.
            const bool q = k < static_cast<int>(quoted.size()) && quoted[static_cast<size_t>(k)] != 0;
            std::string cell = (k < static_cast<int>(rec.size()))
                                   ? csv_trim_unquoted(rec[k], q)
                                   : std::string();
            FrameColumn& c = f.cols_[k];
            if (!c.is_factor) {
                double v = std::numeric_limits<double>::quiet_NaN();
                if (!csv_is_missing(cell)) csv_parse_num(cell, v);
                c.f64[static_cast<size_t>(row)] = v;
            } else {
                if (cell.empty()) { c.codes[static_cast<size_t>(row)] = -1; continue; }
                auto it = dict[k].find(cell);
                int32_t code;
                if (it == dict[k].end()) {
                    code = static_cast<int32_t>(c.levels.size());
                    dict[k].emplace(cell, code);
                    c.levels.push_back(cell);
                } else code = it->second;
                c.codes[static_cast<size_t>(row)] = code;
            }
        }
        ++row;
    }
    f.reindex();
    return f;
}

// `setcol` support: a HEADERLESS single-column CSV (one raw value per row) —
// reuses from_csv's own record reader (csv_read_record) and cell helpers
// (csv_trim/csv_is_missing/csv_parse_num) above, so the quoting/missing rules
// stay a single source of truth. See frame.h doc comment for the exact
// f64-vs-factor missing convention.
FrameColumn Frame::read_csv_column(const std::string& path, const std::string& dtype, int64_t expect_n) {
    if (dtype != "f64" && dtype != "factor")
        throw EngineError("bad_input", "setcol: --dtype must be 'f64' or 'factor' (got '" + dtype + "')", dtype);
    std::ifstream in(to_native_path(path), std::ios::binary);
    if (!in) throw EngineError("data_io", "setcol: cannot open --values file: " + path, path);

    FrameColumn c;
    c.is_factor = (dtype == "factor");
    std::unordered_map<std::string, int32_t> dict;
    std::vector<double> f64vals;
    std::vector<int32_t> codevals;
    std::vector<std::string> rec;
    std::vector<char> quoted;
    bool structured;
    int64_t n = 0;
    while (csv_read_record(in, ',', rec, &structured, &quoted)) {
        // Headerless SINGLE-column payload: every record is a data row — a bare
        // blank line and a quoted "" both mean a missing cell (there is no
        // "skippable line break" here; a stray trailing blank line is caught by
        // the row-count check below as an honest mismatch).
        (void)structured;
        // BUG-292: the setcol transport. PIET quotes EVERY cell it writes here
        // (handlers_data_verbs.cpp csvEscape), so this is the site where a
        // left-justified `tostring, usedisplayformat` render lost its pad and
        // came back at lens {5,6,4,5,9} where Stata stores all 9.
        const bool q0 = !quoted.empty() && quoted[0] != 0;
        std::string cell = rec.empty() ? std::string() : csv_trim_unquoted(rec[0], q0);
        if (c.is_factor) {
            if (cell.empty()) { codevals.push_back(-1); ++n; continue; }  // empty -> missing; "." etc. stay literal levels
            auto it = dict.find(cell);
            int32_t code;
            if (it == dict.end()) {
                code = static_cast<int32_t>(c.levels.size());
                dict.emplace(cell, code);
                c.levels.push_back(cell);
            } else {
                code = it->second;
            }
            codevals.push_back(code);
        } else {
            double v = std::numeric_limits<double>::quiet_NaN();
            if (!csv_is_missing(cell) && !csv_parse_num(cell, v))
                throw EngineError("bad_input", "setcol: --values row " + std::to_string(n + 1) +
                    " is not numeric: '" + cell + "'", cell);
            f64vals.push_back(v);
        }
        ++n;
    }
    if (n != expect_n)
        throw EngineError("bad_input", "setcol: --values has " + std::to_string(n) +
            " row(s), the dataset has " + std::to_string(expect_n) + " — row counts must match exactly", path);
    if (c.is_factor) c.codes = std::move(codevals);
    else c.f64 = std::move(f64vals);
    return c;
}

void Frame::setcol(const std::string& name, FrameColumn&& newcol, bool is_new) {
    // BUG-272: the !is_new branch replaces an existing column's payload wholesale —
    // a sort claim keyed on it is void. (is_new appends; a fresh name can't be a key.)
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    newcol.name = name;
    if (is_new) {
        if (it != idx_.end())
            throw EngineError("bad_input", "setcol: --new given but variable '" + name + "' already exists", name);
        cols_.push_back(std::move(newcol));
        reindex();
    } else {
        if (it == idx_.end())
            throw EngineError("missing_column",
                "setcol: variable '" + name + "' not found (pass --new to append it instead)", name);
        // Move-assign the payload+dtype into the EXISTING slot: every other column's
        // position/bytes are untouched, and write_hbin's patch path (name-keyed)
        // reuses this column's on-disk slot whenever the new dtype/size still fits,
        // falling back to a full rewrite automatically otherwise (patch_hbin's own
        // dtype/nbytes mismatch guard) — no structure_dirty_ needed either way.
        cols_[static_cast<size_t>(it->second)] = std::move(newcol);
    }
}

void Frame::set_or_replace(const std::string& name, std::vector<double> v, StoreType st) {
    // BUG-471 T1: the store boundary. Only the gen/egen/row-egen/bygen verbs pass a
    // non-Double type (cobra eval --store-type); Double skips the loop entirely.
    if (st != StoreType::Double)
        for (double& x : v) x = frame_detail::round_to_store(x, st);
    // BUG-272: replacing a column's VALUES invalidates any sort claim keyed on it
    // (gen/replace/egen/predict/... all funnel through here). A brand-new column can
    // never be a claim key (claims only ever name existing columns), so the append
    // branch below is untouched by construction and this check is a cheap no-op there.
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    if (it != idx_.end()) {
        FrameColumn& c = cols_[it->second];
        c.is_factor = false;
        c.codes.clear();
        c.levels.clear();
        c.f64 = std::move(v);
    } else {
        FrameColumn c;
        c.name = name;
        c.f64 = std::move(v);
        cols_.push_back(std::move(c));
        idx_[name] = static_cast<int>(cols_.size()) - 1;
    }
}

void Frame::set_or_replace_factor(const std::string& name, std::vector<int32_t> codes,
                                 std::vector<std::string> levels) {
    clear_claim_if_key(name);  // same reason as set_or_replace: the VALUES changed
    auto it = idx_.find(name);
    if (it != idx_.end()) {
        FrameColumn& c = cols_[it->second];
        c.is_factor = true;
        c.f64 = std::vector<double>();
        c.codes = std::move(codes);
        c.levels = std::move(levels);
        return;
    }
    FrameColumn c;
    c.name = name;
    c.is_factor = true;
    c.codes = std::move(codes);
    c.levels = std::move(levels);
    cols_.push_back(std::move(c));
    idx_[name] = static_cast<int>(cols_.size()) - 1;
}

void Frame::set_f64_column(const std::string& name, std::vector<double> v) {
    set_or_replace(name, std::move(v));
}

void Frame::generate(const std::string& name, const std::string& expr, bool require_new,
                     const std::string& mask_expr, StoreType st) {
    RngCallScope rng_call;  // draw coordinates are spent when this call returns
    if (require_new && idx_.find(name) != idx_.end())
        throw EngineError("bad_input", "generate: variable '" + name + "' already exists", name);
    // Without `by` the frame is the one group, so the mask's _n/_N/subscripts are
    // the dataset's and the column evaluator is the right one. A sum() inside the
    // mask is a plain running total, as it is inside the folded cond() form.
    std::vector<double> mask;
    if (!mask_expr.empty()) {
        Parser mp(mask_expr, *this);
        auto mast = mp.parse();
        materialize_sums(mast.get(), n_, nullptr, nullptr, nullptr, nullptr, nullptr);
        mask = eval_column(mast.get(), n_);
    }
    Parser parser(expr, *this);
    auto ast = parser.parse();
    refuse_group_in_masked_range(ast.get(), expr, mask_expr);
    materialize_sums(ast.get(), n_, nullptr, nullptr, nullptr, nullptr,
                     mask.empty() ? nullptr : &mask);
    std::vector<double> out = eval_column(ast.get(), n_);
    if (!mask.empty())
        for (int64_t i = 0; i < n_; ++i)
            if (!if_selects(mask[static_cast<size_t>(i)])) out[static_cast<size_t>(i)] = NAd;
    set_or_replace(name, std::move(out), st);
}

void Frame::xtset(const std::string& id_col, const std::string& time_col) {
    require_f64(id_col);
    require_f64(time_col);
    panel_id_col_ = id_col;
    panel_time_col_ = time_col;
}

void Frame::tsset(const std::string& time_col) {
    require_f64(time_col);
    panel_id_col_.clear();  // pure time series: ts_apply gets a null panel pointer
    panel_time_col_ = time_col;
}

void Frame::set_ts_delta(double d) {
    if (!(d > 0))
        throw EngineError("bad_input", "tsset delta must be positive");
    ts_delta_ = d;
}

void Frame::tssmooth_ma(const std::string& output_name, const std::string& input_name,
                        int lags, int leads, bool include_focal) {
    // BUG-370: a PANEL is not a requirement, only a possibility. This used to demand
    // xtset outright, so `tsset t` — an ordinary single time series, which is what
    // `tssmooth ma` is for in the first place — could not reach it at all. A pure
    // time series is simply the one-group case: `id == nullptr` below means every
    // row is the same panel, and the windowing math is untouched by that.
    if (panel_time_col_.empty())
        throw std::runtime_error(
            "tssmooth ma requires tsset (time) or xtset (panel id + time) first");
    if (lags < 0 || leads < 0) throw std::runtime_error("tssmooth ma: negative window");
    const double* id = panel_id_col_.empty() ? nullptr : require_f64(panel_id_col_);
    const double* tm = require_f64(panel_time_col_);
    const double* in = require_f64(input_name);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        if (id != nullptr && id[a] != id[b]) return id[a] < id[b];
        return tm[a] < tm[b];
    });

    std::vector<double> out(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (id == nullptr || id[order[g1]] == id[order[g0]])) ++g1;
        const int64_t glen = g1 - g0;
        int64_t lo = 0, hi = -1;
        for (int64_t j = 0; j < glen; ++j) {
            double tj = tm[order[g0 + j]];
            double tmin = tj - lags, tmax = tj + leads;
            while (lo < glen && tm[order[g0 + lo]] < tmin) ++lo;
            if (hi < lo - 1) hi = lo - 1;
            while (hi + 1 < glen && tm[order[g0 + hi + 1]] <= tmax) ++hi;
            double sum = 0;
            int64_t k = 0;
            for (int64_t w = lo; w <= hi; ++w) {
                // BUG-378: Stata's window(#l #c #f) has a CENTRE weight, and an
                // omitted #c defaults to 0 — measured live, not assumed:
                //   window(1 1 1) -> (1/3)*[x(t-1) + 1*x(t) + x(t+1)]
                //   window(1)     -> (1/1)*[x(t-1) + 0*x(t)]
                // So window(1) is a trailing average that EXCLUDES the focal
                // observation, a shape this function could not express at all.
                if (!include_focal && w == j) continue;
                double v = in[order[g0 + w]];
                if (!std::isnan(v)) { sum += v; ++k; }
            }
            if (k > 0) out[order[g0 + j]] = sum / static_cast<double>(k);
        }
        g0 = g1;
    }
    set_or_replace(output_name, std::move(out));
}

int64_t Frame::keep_if(const std::string& expr) {
    RngCallScope rng_call;
    note_materialization(static_cast<int64_t>(cols_.size()));  // WS-A census: close the generation
    Parser parser(expr, *this);
    auto ast = parser.parse();
    std::vector<double> vv = eval_column(ast.get(), n_);
    std::vector<char> keep(n_);
    int64_t m = 0;
    for (int64_t i = 0; i < n_; ++i) {
        double v = vv[i];
        // Stata's bare-expression `if` truthy test is `v != 0`, full stop — it does
        // NOT additionally require non-missing. Missing is represented internally
        // as a value that is never == 0, so `if x` keeps missing rows too (a classic
        // Stata gotcha; relational `if x>5` is unaffected since ev() never returns
        // NaN for a comparison node — see mi() above — so this predicate reduces to
        // the same `v != 0.0` there either way).
        keep[i] = (std::isnan(v) || v != 0.0) ? 1 : 0;
        m += keep[i];
    }
    // COMPACTION: a fresh buffer per column, and it stays that way (RAMDIET-2 null).
    //
    // The obvious suspicion was that this loop is the production line for the
    // retained-but-free pages the campaign has been chasing — 33 full-frame
    // compactions x ~205 columns = ~4,500 allocate-fill-free cycles at 31 different
    // sizes as the row count walks down, and `malloc_zone_pressure_relief` provably
    // will not take those regions back (0.0 MB on twelve consecutive calls).
    //
    // MEASURED, TWICE, AND NEITHER SHAPE WINS. A kept subsequence writes strictly
    // behind its own read (k <= i), so an owned buffer can be compacted forward onto
    // itself with no temporary and no allocation at all. Compacting in place and
    // keeping the capacity cost +11.5% peak footprint (586.7/589.5 -> 654.2/657.8 MB):
    // pinning every column at the pre-filter row count is worse than the churn it
    // avoids. Compacting in place and shrinking whenever capacity exceeded 2x the live
    // rows landed exactly on the baseline (589.8/586.4 MB), i.e. removing 30 of every
    // 31 allocations per cycle changes the peak by nothing measurable.
    //
    // So the peak is not set by this loop's allocation traffic. It is set by bytes
    // that are genuinely live (WS-A: 96.8% of every column materialized here is named
    // again before the next compaction) plus allocator retention that neither fewer
    // allocations nor pressure relief returns. The lever that remains is making the
    // DATA smaller, not making its allocation cheaper.
    // NMISS-CARRY (2026-08-20): count the survivors' missings AS THEY ARE GATHERED.
    // This loop already visits every kept element of every column, so the count is
    // free here — a branch on a value the iteration has in hand. Seeding the cache
    // from it means the header writer downstream states the number instead of
    // rescanning for it, and does so WITHOUT the write-time scan that made the
    // first version of this carry a wash: `drop|if` (434 calls, each writing a
    // frame this very filter had just invalidated) went 18.5 -> 43.6 s when the
    // writer forced the count, and the joinby/merge win it bought was cancelled.
    // Counted per column below, seeded after the row count is updated (n_ = m),
    // because seed_n_missing writes through to the same cache detach() clears.
    std::vector<int64_t> kept_missing;
    kept_missing.reserve(cols_.size());
    for (FrameColumn& col : cols_) {
        const FrameColumn& cc = col;  // const view: read borrowed/owned without materializing
        int64_t miss = 0;
        if (col.is_factor) {
            std::vector<int32_t> nc;
            nc.reserve(m);
            for (int64_t i = 0; i < n_; ++i) if (keep[i]) { if (cc.codes[i] < 0) ++miss; nc.push_back(cc.codes[i]); }
            col.codes = std::move(nc);
            kept_missing.push_back(miss);
        } else if (cc.f64.narrow()) {
            // LADDER: already narrow — gather narrow to narrow. The kept values are a
            // subset of values already proved float-exact, so no re-test is needed.
            const CowVec<float>& src = cc.f64.f32_ref();
            std::vector<float> nn;
            nn.reserve(m);
            for (int64_t i = 0; i < n_; ++i) if (keep[i]) { if (std::isnan(src[i])) ++miss; nn.push_back(src[i]); }
            col.f64.set_narrow_f32(std::move(nn));
            kept_missing.push_back(miss);
        } else {
            // LADDER: this loop is where a frame's owned bytes are born, so it is
            // where the width is decided. Pass one asks whether every KEPT value
            // survives a float round-trip EXACTLY; pass two writes at that width.
            // NaN disqualifies outright — a missing's NaN payload bits carry Stata's
            // extended missing (.a-.z) and float32 does not preserve them, so a
            // narrowed column would silently turn `.a` into plain `.`.
            const F64Reader r = cc.f64.reader();
            bool f32_ok = true;
            for (int64_t i = 0; f32_ok && i < n_; ++i) {
                if (!keep[i]) continue;
                const double x = r[static_cast<std::size_t>(i)];
                if (std::isnan(x) || static_cast<double>(static_cast<float>(x)) != x) f32_ok = false;
            }
            if (f32_ok) {
                // f32_ok implies NO NaN survived (isnan disqualifies the width
                // outright, just above), so this branch's missing count is 0 by
                // construction — not by counting.
                std::vector<float> nn;
                nn.reserve(m);
                for (int64_t i = 0; i < n_; ++i)
                    if (keep[i]) nn.push_back(static_cast<float>(r[static_cast<std::size_t>(i)]));
                col.f64.set_narrow_f32(std::move(nn));
            } else {
                std::vector<double> nf;
                nf.reserve(m);
                for (int64_t i = 0; i < n_; ++i)
                    if (keep[i]) {
                        const double x = r[static_cast<std::size_t>(i)];
                        if (std::isnan(x)) ++miss;
                        nf.push_back(x);
                    }
                col.f64 = std::move(nf);
            }
            kept_missing.push_back(miss);
        }
    }
    n_ = m;
    // Seed AFTER n_ is updated: the counts describe the post-filter column.
    for (std::size_t i = 0; i < cols_.size() && i < kept_missing.size(); ++i) {
        cols_[i].seed_n_missing(kept_missing[i]);
    }
    structure_dirty_ = true;  // row count changed → can't patch onto the source
    // BUG-272: the sort claim SURVIVES keep_if, and that is a theorem, not a guess —
    // the gathers above take rows in ascending index order (stable subsequence), and
    // a subsequence of a lexicographically sorted sequence is itself sorted by the
    // same keys. Stata agrees: `drop if`/`keep if` leaves its sortlist standing.
    return m;
}

Frame Frame::project(const std::vector<std::string>& names) const {
    Frame out;
    out.n_ = n_;
    out.src_ = src_;   // keepalive for any column still borrowing the root mapping
    out.cols_.reserve(names.size());
    for (const std::string& nm : names) {
        auto it = idx_.find(nm);
        if (it == idx_.end())
            throw EngineError("missing_column",
                              "project: no column '" + nm + "' in the resident frame", nm);
        out.cols_.push_back(cols_[it->second]);  // shares ColumnData; no payload copy
    }
    out.reindex();
    return out;
}

void Frame::drop(const std::string& name) {
    // BUG-272: a claim naming a dropped variable clears — the WHOLE claim, not a
    // truncation (a prefix would stay true, but the design rule is "doubt clears",
    // and nothing downstream needs the weaker prefix badly enough to reason about it).
    // Dropping a non-key column leaves the claim intact: row order is untouched and
    // every key still names a live column.
    clear_claim_if_key(name);
    auto it = idx_.find(name);
    if (it == idx_.end()) return;
    cols_.erase(cols_.begin() + it->second);
    reindex();
    structure_dirty_ = true;  // column set changed → can't patch onto the source
}

// ---- by-group helpers ----
namespace frame_detail {  // R2: shared by both frame TUs (see frame_internal.h)
// Materialize columns as doubles (factor codes cast to double) for grouping/sorting.
std::vector<std::vector<double>> cols_as_double(const Frame& fr, int64_t n,
                                                const std::vector<std::string>& names) {
    std::vector<std::vector<double>> out;
    for (const std::string& nm : names) {
        const FrameColumn& c = fr.col(nm);
        std::vector<double> v(n);
        if (c.is_factor) for (int64_t i = 0; i < n; ++i) v[i] = static_cast<double>(c.codes[i]);
        else {
            const F64Reader r = c.f64.reader();   // LADDER: no widening for key columns
            for (int64_t i = 0; i < n; ++i) v[i] = r[static_cast<std::size_t>(i)];
        }
        out.push_back(std::move(v));
    }
    return out;
}
// Compare rows a,b on key columns: NaN sorts last, NaN==NaN.
bool key_less(const std::vector<std::vector<double>>& K, int64_t a, int64_t b) {
    for (const auto& col : K) {
        double x = mi(col[a]), y = mi(col[b]);
        if (x < y) return true;
        if (x > y) return false;
    }
    return false;
}
bool key_eq(const std::vector<std::vector<double>>& K, int64_t a, int64_t b) {
    for (const auto& col : K) {
        double x = col[a], y = col[b];
        if (std::isnan(x) && std::isnan(y)) continue;
        if (x != y) return false;
    }
    return true;
}
}  // namespace frame_detail
using namespace frame_detail;

void Frame::egen_by(const std::string& out, EgenFn fn, const std::string& expr,
                    const std::vector<std::string>& by_cols, double pctile_p, StoreType st) {
    // The store type: every function here ends in ONE `gen <type> v = ...` (_gmean,
    // _gtotal, _gmax, ... .ado), so the result is rounded once, like a generate —
    // except default rank. _grank.ado:55-58 stores `_n`, then `replace v = v + (_N-1)/2`
    // over a tie, and replace WIDENS an integer variable (byte 2.7 -> float :810, byte
    // 200 -> int :876) where a fixed store type would truncate or store `.`: refused.
    // Float stays float under replace (:761), so float rank is the one final rounding.
    // track/field/unique only copy an earlier row's `_n` over a tie, which a single
    // store of the final rank reproduces (an out-of-range `_n` is `.` either way).
    if (fn == EgenFn::Rank && frame_detail::is_integer_store(st))
        throw EngineError("bad_input",
                          "egen rank(): --store-type byte|int|long is not supported (Stata builds "
                          "tied ranks with replace, which widens an integer variable)");
    RngCallScope rng_call;
    Parser parser(expr, *this);
    auto ast = parser.parse();
    std::vector<double> v = eval_column(ast.get(), n_);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }

    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        // gather non-missing v over the group
        std::vector<double> s;
        s.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) { double x = v[order[q]]; if (!std::isnan(x)) s.push_back(x); }
        const int64_t cnt = static_cast<int64_t>(s.size());

        // Per-row (non-broadcast) functions: write each row of the group individually.
        if (fn == EgenFn::Rank || fn == EgenFn::RankTrack || fn == EgenFn::RankField ||
            fn == EgenFn::RankUnique) {
            // The four egen rank() flavours, all off ONE ascending stable sort of the
            // group's non-missing values. Missing input => missing rank in every one.
            // Oracle private_oracle_b.log / _c.log on 10,10,20,30,30,30,40 (+3 missing):
            //   default    10 -> 1.5   30 -> 5     mean/midrank over the ranks spanned
            //   track      10 -> 1     30 -> 4     minimum rank, shared by every tied
            //                                      member (oracle_c: obs1 == obs2 == 1)
            //   field      10 -> 6     30 -> 2     descending competition rank
            //   unique     10 -> 1, and the SECOND 10 -> 2 (oracle_c is the capture
            //                                      that separates unique from track)
            std::vector<std::pair<double, int64_t>> vr;
            vr.reserve(cnt);
            for (int64_t q = g0; q < g1; ++q) { double x = v[order[q]]; if (!std::isnan(x)) vr.emplace_back(x, order[q]); }
            std::stable_sort(vr.begin(), vr.end(),
                             [](const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) { return a.first < b.first; });
            int64_t m = static_cast<int64_t>(vr.size());
            if (fn == EgenFn::RankUnique) {
                // Sequential 1..m in ascending order; the stable sort leaves tied rows
                // in OBSERVATION order, which is what breaks the tie.
                for (int64_t t = 0; t < m; ++t) out_col[vr[t].second] = static_cast<double>(t + 1);
                g0 = g1;
                continue;
            }
            int64_t i = 0;
            while (i < m) {
                int64_t j = i + 1;
                while (j < m && vr[j].first == vr[i].first) ++j;  // [i,j) is a tie block
                double rank;
                switch (fn) {
                    // ranks are (i+1)..(j) inclusive (1-based); mean = (i+1 + j)/2
                    case EgenFn::Rank: rank = (static_cast<double>(i + 1) + static_cast<double>(j)) / 2.0; break;
                    case EgenFn::RankTrack: rank = static_cast<double>(i + 1); break;
                    // field: 1 + (number of values strictly GREATER) = m - j + 1.
                    default: rank = static_cast<double>(m - j + 1); break;
                }
                for (int64_t t = i; t < j; ++t) out_col[vr[t].second] = rank;
                i = j;
            }
            g0 = g1;
            continue;
        }
        if (fn == EgenFn::Std) {
            // Standardize: (x - groupmean)/groupSd, sample SD (N-1). Missing rows stay
            // missing; if SD is 0 or <2 non-missing, all rows missing (Stata behavior).
            double agg_std = NAd;  // group SD
            double mean = NAd;
            if (cnt >= 2) {
                double t = 0; for (double x : s) t += x; mean = t / cnt;
                double ss = 0; for (double x : s) ss += (x - mean) * (x - mean);
                agg_std = std::sqrt(ss / (cnt - 1));
            }
            for (int64_t q = g0; q < g1; ++q) {
                double x = v[order[q]];
                if (!std::isnan(x) && !std::isnan(agg_std) && agg_std != 0.0)
                    out_col[order[q]] = (x - mean) / agg_std;
                // else leave missing
            }
            g0 = g1;
            continue;
        }

        // Broadcast (scalar-per-group) functions.
        double agg = NAd;
        switch (fn) {
            case EgenFn::Count: agg = static_cast<double>(cnt); break;
            case EgenFn::Sum: { double t = 0; for (double x : s) t += x; agg = t; break; }  // 0 if empty
            case EgenFn::Max: if (cnt) agg = *std::max_element(s.begin(), s.end()); break;
            case EgenFn::Min: if (cnt) agg = *std::min_element(s.begin(), s.end()); break;
            case EgenFn::Mean: if (cnt) { double t = 0; for (double x : s) t += x; agg = t / cnt; } break;
            case EgenFn::Sd:
                if (cnt >= 2) {
                    double t = 0; for (double x : s) t += x; double mean = t / cnt;
                    double ss = 0; for (double x : s) ss += (x - mean) * (x - mean);
                    agg = std::sqrt(ss / (cnt - 1));
                }
                break;
            // egenmore `var` — the SAME sum-of-squares as Sd without the root, so the
            // two can never disagree about the divisor or the singleton rule. Oracle
            // private_oracle_c.log: var(r) == summarize's r(Var)
            // == %21x +1.0124924924925X+007 == 900/7.
            case EgenFn::Var:
                if (cnt >= 2) {
                    double t = 0; for (double x : s) t += x; double mean = t / cnt;
                    double ss = 0; for (double x : s) ss += (x - mean) * (x - mean);
                    agg = ss / (cnt - 1);
                }
                break;
            case EgenFn::Median:
                if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, 50.0); }
                break;
            case EgenFn::Pctile:
                if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, pctile_p); }
                break;
            case EgenFn::Iqr:
                if (cnt) { std::sort(s.begin(), s.end());
                           agg = stata_pctile(s, 75.0) - stata_pctile(s, 25.0); }
                break;
            case EgenFn::Rank: case EgenFn::Std:
            case EgenFn::RankTrack: case EgenFn::RankField: case EgenFn::RankUnique:
                break;  // handled above
        }
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = agg;
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col), st);
}

// egen tag(varlist): 1 on the first obs (current data order) of each distinct key
// combination, 0 elsewhere; missing-key rows get missing. Distinct from group():
// tag scans in DATA order and marks the first occurrence of each unseen key.
void Frame::egen_tag(const std::string& out, const std::vector<std::string>& by_cols) {
    if (by_cols.empty()) throw std::runtime_error("egen tag(): requires a varlist");
    std::vector<std::vector<double>> K = cols_as_double(*this, n_, by_cols);
    // Stable-sort row indices by key to detect the first row of each key group, but
    // mark in the ORIGINAL data order: a key is "tagged" at its lowest original index.
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) {
                         if (key_less(K, a, b)) return true;
                         if (key_less(K, b, a)) return false;
                         return a < b;  // tie-break by original index → lowest index first
                     });
    std::vector<double> out_col(n_, 0.0);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && key_eq(K, order[g1], order[g0])) ++g1;
        // Is this group's key missing in any component? → tag missing for all its rows.
        bool key_missing = false;
        for (const auto& col : K) if (std::isnan(col[order[g0]])) { key_missing = true; break; }
        if (key_missing) {
            for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = NAd;
        } else {
            // first row of the group in ORIGINAL order = the smallest original index
            int64_t first = order[g0];
            for (int64_t q = g0 + 1; q < g1; ++q) if (order[q] < first) first = order[q];
            out_col[first] = 1.0;
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col));
}

// egen group(varlist): dense 1..K over sorted distinct key combinations. Missing-key
// rows get missing (Stata default) unless missing_as_group (missings sort last and
// form their own group ids).
void Frame::egen_group(const std::string& out, const std::vector<std::string>& by_cols,
                       bool missing_as_group, StoreType st) {
    if (by_cols.empty()) throw std::runtime_error("egen group(): requires a varlist");
    // Stata `egen group()` numbers levels in SORTED value order — strings
    // alphabetically, numerics ascending (C1-F8 / golden G7). For a factor
    // (string) by-column the sort key must be the LABEL's rank, NOT the stored
    // first-seen factor code that cols_as_double() would hand back. Build the
    // keys here so a factor code maps to its label's alphabetical rank; missing
    // (code < 0) keeps its negative sentinel (unchanged missing semantics).
    std::vector<std::vector<double>> K;
    K.reserve(by_cols.size());
    for (const auto& nm : by_cols) {
        const FrameColumn& c = col(nm);
        std::vector<double> v(n_);
        if (c.is_factor) {
            std::vector<int> perm(c.levels.size());
            std::iota(perm.begin(), perm.end(), 0);
            std::stable_sort(perm.begin(), perm.end(),
                             [&](int a, int b) { return c.levels[a] < c.levels[b]; });
            std::vector<int> rank(c.levels.size());
            for (int r = 0; r < static_cast<int>(perm.size()); ++r) rank[perm[r]] = r;
            for (int64_t i = 0; i < n_; ++i) {
                int32_t cd = c.codes[i];
                v[i] = (cd < 0) ? static_cast<double>(cd) : static_cast<double>(rank[cd]);
            }
        } else {
            for (int64_t i = 0; i < n_; ++i) v[i] = c.f64[i];
        }
        K.push_back(std::move(v));
    }
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) { return key_less(K, a, b); });  // NaN sorts last
    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    int64_t gid = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && key_eq(K, order[g1], order[g0])) ++g1;
        bool key_missing = false;
        for (const auto& col : K) if (std::isnan(col[order[g0]])) { key_missing = true; break; }
        if (key_missing && !missing_as_group) {
            // leave the whole group as missing, do NOT consume a group id
        } else {
            ++gid;
            for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = static_cast<double>(gid);
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(out_col), st);
}

// egen seq(): running 1..N over the data's current order; resets at each by-group.
void Frame::egen_seq(const std::string& out, const std::vector<std::string>& by_cols,
                     double from, double to, bool has_to, int64_t block) {
    // BUG-433: the k-th observation of a group (k counted from 0) gets
    //     from + ((k / block) mod span)      when to() was given, span = to-from+1
    //     from + (k / block)                 otherwise
    // so `b(2)` is 1 1 2 2 3 3 ... and `f(1) t(3)` is 1 2 3 1 2 3 ...  The bare
    // form is from=1, block=1, no to, i.e. exactly the old 1..n.
    if (block < 1) block = 1;
    const double span = has_to ? (to - from + 1.0) : 0.0;
    auto value_at = [&](int64_t k) {
        const int64_t step = k / block;
        if (has_to && span >= 1.0) {
            const int64_t m = static_cast<int64_t>(span);
            return from + static_cast<double>(((step % m) + m) % m);
        }
        return from + static_cast<double>(step);
    };
    std::vector<double> out_col(n_, NAd);
    if (by_cols.empty()) {
        for (int64_t i = 0; i < n_; ++i) out_col[i] = value_at(i);
    } else {
        std::vector<std::vector<double>> K = cols_as_double(*this, n_, by_cols);
        // Stable sort by key preserves original order within group → seq follows
        // the data's order inside each by-group (Stata bysort … : egen seq()).
        std::vector<int64_t> order(n_);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
        int64_t g0 = 0;
        while (g0 < n_) {
            int64_t g1 = g0 + 1;
            while (g1 < n_ && key_eq(K, order[g1], order[g0])) ++g1;
            int64_t c = 0;
            for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = value_at(c++);
            g0 = g1;
        }
    }
    set_or_replace(out, std::move(out_col));
}

// egen mode(x) [, by() minmode|maxmode|nummode(#) missing] — B4/W6.
// Mechanism read from /Applications/StataNow/ado/base/_/_gmode.ado (v3.0.0), which is
// the method identification behind the oracle: it counts each value's frequency within
// the by-group, zeroes the frequency of missing unless `missing` is given, and then
// picks among the values that share the maximum frequency. minmode/maxmode/nummode()
// are opts_exclusive there and here. In the ado's sorts missing orders LAST ascending
// (Stata's `. > any number`), so it is the largest candidate for maxmode and never the
// smallest for minmode — reproduced below by sorting NaN to the end.
//
// Oracle private_oracle_b.log. On t = 1,1,2,2 and six missings, whose modes TIE:
//   default -> missing everywhere · maxmode -> 2 · minmode -> 1 · missing -> `.` itself
//              wins with frequency 6, so the answer is missing.
// On m = 1,1,2,2,3,3,3,4,4,. (a unique mode) -> 3.
void Frame::egen_mode(const std::string& out, const std::string& expr,
                      const std::vector<std::string>& by_cols, const ModeOpts& mo) {
    RngCallScope rng_call;
    int excl = (mo.minmode ? 1 : 0) + (mo.maxmode ? 1 : 0) + (mo.nummode > 0 ? 1 : 0);
    if (excl > 1)
        throw EngineError("bad_input",
                          "egen mode(): minmode, maxmode and nummode() are mutually exclusive");
    if (mo.nummode < 0)
        throw EngineError("bad_input", "egen mode(): nummode() must be a positive integer");
    Parser parser(expr, *this);
    auto ast = parser.parse();
    std::vector<double> v = eval_column(ast.get(), n_);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    // NaN sorts last — see the header comment. std::sort's comparator must be a strict
    // weak ordering, and `a < b` with NaN operands is not, so it is spelled out.
    auto less_nan_last = [](double a, double b) {
        if (std::isnan(a)) return false;
        if (std::isnan(b)) return true;
        return a < b;
    };
    std::vector<double> out_col(n_, NAd);
    int64_t max_modes_any_group = 0;
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        std::vector<double> s;
        s.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) {
            double x = v[order[q]];
            if (mo.missing_as_value || !std::isnan(x)) s.push_back(x);
        }
        std::sort(s.begin(), s.end(), less_nan_last);
        // runs of equal values -> (value, frequency); NaNs form one trailing run.
        std::vector<std::pair<double, int64_t>> freq;
        for (size_t i = 0; i < s.size();) {
            size_t j = i + 1;
            while (j < s.size() &&
                   ((std::isnan(s[i]) && std::isnan(s[j])) || s[j] == s[i])) ++j;
            freq.emplace_back(s[i], static_cast<int64_t>(j - i));
            i = j;
        }
        double answer = NAd;
        if (!freq.empty()) {
            int64_t best = 0;
            for (const auto& f : freq) best = std::max(best, f.second);
            std::vector<double> modes;  // already ascending, NaN last
            for (const auto& f : freq) if (f.second == best) modes.push_back(f.first);
            max_modes_any_group = std::max(max_modes_any_group,
                                           static_cast<int64_t>(modes.size()));
            if (mo.nummode > 0) {
                if (static_cast<size_t>(mo.nummode) <= modes.size())
                    answer = modes[static_cast<size_t>(mo.nummode) - 1];
                // else: fewer modes than asked for -> missing (Stata warns and does
                // the same); NOT an error unless EVERY group is short (checked below).
            } else if (mo.minmode) {
                answer = modes.front();
            } else if (mo.maxmode) {
                answer = modes.back();
            } else if (modes.size() == 1) {
                answer = modes.front();
            }
            // plain mode with a tie -> missing (Stata also warns)
        }
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = answer;
        g0 = g1;
    }
    // _gmode's CheckNummode / CheckByNummodeEveryGroup: r(198) when nummode() exceeds
    // the mode count of EVERY group (egen-six captured `nummode(4)` at rc 198 with
    // "The largest number of modes in a group is 2.").
    if (mo.nummode > 0 && max_modes_any_group > 0 &&
        static_cast<int64_t>(mo.nummode) > max_modes_any_group)
        throw EngineError("bad_input",
                          "egen mode(): nummode(" + std::to_string(mo.nummode) +
                          ") is greater than the number of modes in every group; the largest "
                          "number of modes in a group is " +
                          std::to_string(max_modes_any_group));
    set_or_replace(out, std::move(out_col));
}

// egen nvals(varlist) [, by() missing] — egenmore, ACCEPTED-NAMED (see frame.h).
// The number of DISTINCT values, or of distinct combinations for a multi-variable
// varlist. Oracle private_oracle_c.log: nvals(r) = 4 over 10,10,20,30,30,30,40;
// the egen-six capture's nvals(g v) = 5 is the varlist form, and nvals(s) = 5 shows
// it counts a string variable's distinct values too (a factor here).
void Frame::egen_nvals(const std::string& out, const std::vector<std::string>& vars,
                       const std::vector<std::string>& by_cols, bool missing_as_value,
                       StoreType st) {
    if (vars.empty()) throw EngineError("bad_input", "egen nvals(): requires a varlist");
    // A factor column's CODE identifies its level, so cols_as_double is a faithful
    // key for both kinds (missing is NaN for f64 and a negative code for a factor —
    // normalized to NaN here so one missing test covers both).
    std::vector<std::vector<double>> V;
    V.reserve(vars.size());
    for (const auto& nm : vars) {
        const FrameColumn& c = col(nm);
        std::vector<double> x(n_);
        for (int64_t i = 0; i < n_; ++i)
            x[i] = c.is_factor ? (c.codes[i] < 0 ? NAd : static_cast<double>(c.codes[i]))
                               : c.f64[i];
        V.push_back(std::move(x));
    }
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by_cols.empty()) {
        K = cols_as_double(*this, n_, by_cols);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    std::vector<double> out_col(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by_cols.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        std::vector<std::vector<double>> rows;
        rows.reserve(g1 - g0);
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            bool any_missing = false;
            std::vector<double> key(V.size());
            for (size_t k = 0; k < V.size(); ++k) {
                key[k] = V[k][r];
                if (std::isnan(key[k])) any_missing = true;
            }
            if (any_missing && !missing_as_value) continue;
            rows.push_back(std::move(key));
        }
        // NaN-safe lexicographic ordering (NaN last), then count the distinct runs.
        auto key_cmp = [](const std::vector<double>& a, const std::vector<double>& b) {
            for (size_t k = 0; k < a.size(); ++k) {
                bool na = std::isnan(a[k]), nb = std::isnan(b[k]);
                if (na && nb) continue;
                if (na) return false;
                if (nb) return true;
                if (a[k] < b[k]) return true;
                if (a[k] > b[k]) return false;
            }
            return false;
        };
        std::sort(rows.begin(), rows.end(), key_cmp);
        int64_t distinct = 0;
        for (size_t i = 0; i < rows.size(); ++i)
            if (i == 0 || key_cmp(rows[i - 1], rows[i])) ++distinct;
        double answer = rows.empty() ? NAd : static_cast<double>(distinct);
        for (int64_t q = g0; q < g1; ++q) out_col[order[q]] = answer;
        g0 = g1;
    }
    // _gnvals.ado (egenmore, the copy bundled with the corpus): `gen <type> g = _n==1`,
    // then `replace g = sum(g)` and `replace g = g[_N]`. Under float the last running
    // sum is stored once, so the count is rounded once; an integer type WIDENS under
    // replace (w2e_float_default replace_byte_overflow :876, replace_int_overflow :893),
    // so the count is exact.
    if (frame_detail::is_integer_store(st)) st = StoreType::Double;
    set_or_replace(out, std::move(out_col), st);
}

// ---- Stata's %w.0g, and string()/strofreal() on top of it (B4/W7) ----------
// Contract, the cell-by-cell derivation and the one UNPINNED edge are documented at
// the declaration in frame.h. Oracle: tests/eval/golden/aer_b4/private_oracle_g2.log.
std::string stata_format_g(double v, int width) {
    if (std::isnan(v)) return ".";          // g2 `missing` cell: string(.) = "."
    if (width < 7) width = 7;               // below w7 the budget degenerates; no cell pins it
    const int S = width - 2;                // significant-digit budget: 7/8/10 at w9/w10/w12
    if (v == 0.0) return "0";               // catches -0.0 too (-0.0 == 0.0)
    const bool neg = std::signbit(v);
    const double a = std::fabs(v);
    char buf[64];
    // Round to S significant digits FIRST, then read the decimal exponent off the
    // rounded value — so a carry (9.9999e6 -> 1.0e7 at S=7) moves the branch too.
    std::snprintf(buf, sizeof buf, "%.*e", S - 1, a);
    const char* ep = std::strchr(buf, 'e');
    const int e = ep ? std::atoi(ep + 1) : 0;
    std::string s;
    if (e >= S || e < -5) {
        int md = S - 5;                     // mantissa DECIMALS = budget-5 (budget-4 sig digits)
        if (md < 0) md = 0;
        std::snprintf(buf, sizeof buf, "%.*e", md, a);
        s = buf;
    } else {
        const int n = (e >= 0) ? (e + 1) : 0;   // digits left of the point ("" for |x|<1)
        int d = S - n;
        if (d < 0) d = 0;
        std::snprintf(buf, sizeof buf, "%.*f", d, a);
        s = buf;
        if (s.find('.') != std::string::npos) {   // strip trailing zeros, then a bare "."
            size_t last = s.find_last_not_of('0');
            if (s[last] == '.') --last;
            s.erase(last + 1);
        }
        if (s.size() >= 2 && s[0] == '0' && s[1] == '.') s.erase(0, 1);  // .5, never 0.5
    }
    return neg ? "-" + s : s;
}

std::string stata_string_of(double v, const std::string& fmt) {
    if (fmt.empty()) return stata_format_g(v, 9);   // g2: the default IS %9.0g
    // Parse %[w][.d]<conv>. Only the two forms the captures pin are accepted.
    size_t p = 0;
    if (p < fmt.size() && fmt[p] == '%') ++p;
    const size_t wst = p;
    while (p < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[p]))) ++p;
    const std::string ws = fmt.substr(wst, p - wst);
    std::string ds;
    if (p < fmt.size() && fmt[p] == '.') {
        ++p;
        const size_t dst = p;
        while (p < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[p]))) ++p;
        ds = fmt.substr(dst, p - dst);
    }
    const std::string tail = fmt.substr(p);
    const int w = ws.empty() ? 9 : std::stoi(ws);
    const int d = ds.empty() ? 0 : std::stoi(ds);
    if (tail == "g" && d == 0) return stata_format_g(v, w);
    if (tail == "f") {
        // Missing prints as "." under any numeric format. NOT blank-padded to w:
        // egen-six `len_f92` = 4 for string(3,"%9.2f"), fn.pdf p.171 agrees.
        if (std::isnan(v)) return ".";
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", d, v);
        return buf;
    }
    throw EngineError("unsupported",
        "string()/egen concat format(): only %w.0g and %w.Nf are supported — '" + fmt +
        "' has no captured Stata rendering in this build, and guessing one would put a "
        "silently wrong value in a join key");
}

// These interfaces remain so callers receive an explicit public-beta refusal.
// Implementations withheld pending redistribution provenance clearance.
void Frame::egen_concat(const std::string&, const std::vector<std::string>&,
                        const ConcatOpts&) {
    throw EngineError("unsupported", "egen concat(): unavailable in this public beta (provenance review)");
}
void Frame::egen_cut(const std::string&, const std::string&, const CutOpts&, StoreType) {
    throw EngineError("unsupported", "egen cut(): unavailable in this public beta (provenance review)");
}

void Frame::row_egen(const std::string& out, RowFn fn, const std::vector<std::string>& cols,
                     StoreType st) {
    if (cols.empty()) throw std::runtime_error("egen row function: empty varlist");
    // The store type, per function, from the ado that builds the result:
    //  * rowmean/rowsd: one `gen <type> h = ...` (_growmean.ado, _growsd.ado), so the
    //    value is rounded once, like any generate.
    //  * rowtotal/rowmax/rowmin: `gen <type> g`, then `replace g = ...` per variable
    //    (_growtotal/_growmax/_growmin.ado). Float stays float under replace (:761), so
    //    float is modelled: total rounds every partial sum (below); max/min round the
    //    pick once, which equals the ado's per-step rounding only while every input is
    //    within float's range (float() is monotone there). Past that range the ado's
    //    ORDER decides (_growmax.ado:19 stores `.` for an out-of-range x1 and the next
    //    replace takes x2, so rowmax(1e39,5) is 5 where this returns `.`), and what
    //    replace stores into a float for an out-of-range value is not captured: OPEN
    //    (float-default follow-up FD-T1-A). An integer type WIDENS under replace (byte
    //    2.7 -> float :810, byte 200 -> int :876, int -> long :893), which a fixed store
    //    type cannot express: refused, not truncated.
    //  * rowmiss/rownonmiss: integer counts, `replace g = g + ...` — widening keeps
    //    them exact under every type, so they are stored unrounded.
    if (frame_detail::is_integer_store(st) &&
        (fn == RowFn::Total || fn == RowFn::Max || fn == RowFn::Min)) {
        const char* nm = fn == RowFn::Total ? "rowtotal" : fn == RowFn::Max ? "rowmax" : "rowmin";
        throw EngineError("bad_input", std::string("egen ") + nm +
                                           "(): --store-type byte|int|long is not supported (Stata "
                                           "builds it with replace, which widens an integer variable)");
    }
    const StoreType store = (fn == RowFn::Miss || fn == RowFn::NonMiss) ? StoreType::Double : st;
    std::vector<const double*> p;
    p.reserve(cols.size());
    for (const auto& c : cols) p.push_back(require_f64(c));
    const int ncol = static_cast<int>(p.size());

    std::vector<double> out_col(n_, NAd);
    std::vector<double> s;
    s.reserve(ncol);
    for (int64_t i = 0; i < n_; ++i) {
        s.clear();
        for (int j = 0; j < ncol; ++j) {
            double x = p[j][i];
            if (!std::isnan(x)) s.push_back(x);
        }
        const int m = static_cast<int>(s.size());
        double r = NAd;
        switch (fn) {
            case RowFn::NonMiss: r = m; break;
            case RowFn::Miss: r = ncol - m; break;
            case RowFn::Total: {  // 0 if all missing
                double t = 0;
                if (st == StoreType::Float) {
                    // _growtotal.ado: `gen <type> g = x1`, then `replace g = g + xk`, and
                    // replace keeps a float variable float (w2e_float_default
                    // replace_float_23 :761), so under float every partial sum is stored
                    // as a float (a partial sum past float's range is taken to store `.`,
                    // the generate rule of :530; replace's rule there is uncaptured,
                    // FD-T1-A). Oracle egen_rowtotal_untyped :1038 is
                    // float(float(1/7) + 1/11) = +1.dec0d60000000X-003, one float ulp
                    // above the single final rounding (…d4). A missing xk adds 0, which
                    // leaves a stored partial sum unchanged, so skipping it is the same.
                    // (Byte/Int/Long never get here: refused above.)
                    for (double x : s) t = frame_detail::round_to_store(t + x, st);
                } else {
                    for (double x : s) t += x;
                }
                r = t;
                break;
            }
            case RowFn::Max: if (m) r = *std::max_element(s.begin(), s.end()); break;
            case RowFn::Min: if (m) r = *std::min_element(s.begin(), s.end()); break;
            case RowFn::Mean: if (m) { double t = 0; for (double x : s) t += x; r = t / m; } break;
            case RowFn::Sd:
                if (m >= 2) {
                    double t = 0; for (double x : s) t += x; double mean = t / m;
                    double ss = 0; for (double x : s) ss += (x - mean) * (x - mean);
                    r = std::sqrt(ss / (m - 1));
                }
                break;
        }
        out_col[i] = r;
    }
    set_or_replace(out, std::move(out_col), store);
}

namespace {
// Shared by both xtile spellings: group = 1 + (number of cutpoints strictly below
// the value), missing in -> missing out. Keeping this in one place is what makes
// `nquantiles()` and `cutpoints()` provably the same command with a different
// source of edges.
std::vector<double> xtile_assign(const double* x, int64_t n, const std::vector<double>& cut) {
    std::vector<double> o(static_cast<size_t>(n), NAd);
    for (int64_t i = 0; i < n; ++i) {
        if (std::isnan(x[i])) continue;
        int g = 1;
        for (double c : cut) if (x[i] > c) ++g;
        o[static_cast<size_t>(i)] = static_cast<double>(g);
    }
    return o;
}
}  // namespace

void Frame::xtile(const std::string& out, const std::string& var, int nq) {
    const double* x = require_f64(var);
    if (nq < 2) throw std::runtime_error("xtile: nq must be >= 2");
    std::vector<double> sorted;
    for (int64_t i = 0; i < n_; ++i) if (!std::isnan(x[i])) sorted.push_back(x[i]);
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> cut;
    for (int i = 1; i < nq; ++i) cut.push_back(stata_pctile(sorted, 100.0 * i / nq));
    set_or_replace(out, xtile_assign(x, n_, cut));
}

void Frame::xtile_cuts(const std::string& out, const std::string& var,
                       const std::string& cutvar) {
    const double* x = require_f64(var);
    const double* c = require_f64(cutvar);
    // The cutpoints ARE the non-missing values of cutvar, deduplicated and sorted.
    // Stata reads them the same way, which is why a cutpoints() variable is
    // normally short and padded with missings.
    std::vector<double> cut;
    for (int64_t i = 0; i < n_; ++i) if (!std::isnan(c[i])) cut.push_back(c[i]);
    std::sort(cut.begin(), cut.end());
    cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
    if (cut.empty())
        throw std::runtime_error("xtile: cutpoints(" + cutvar + ") has no non-missing values");
    set_or_replace(out, xtile_assign(x, n_, cut));
}

void Frame::ipolate(const std::string& out, const std::string& yvar, const std::string& xvar,
                    bool epolate, const std::vector<std::string>& by) {
    const double* y = require_f64(yvar);
    const double* x = require_f64(xvar);
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by.empty()) {
        K = cols_as_double(*this, n_, by);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    std::vector<double> o(n_, NAd);
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        // anchors: (x,y) both non-missing, one per DISTINCT x — Stata averages y across
        // tied x values before building the interpolation grid.
        std::vector<std::pair<double, double>> pts;
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            if (!std::isnan(x[r]) && !std::isnan(y[r])) pts.emplace_back(x[r], y[r]);
        }
        std::sort(pts.begin(), pts.end());
        std::vector<std::pair<double, double>> anc;
        for (size_t p = 0; p < pts.size(); ) {
            size_t e = p + 1;
            double sum = pts[p].second;
            while (e < pts.size() && pts[e].first == pts[p].first) { sum += pts[e].second; ++e; }
            anc.emplace_back(pts[p].first, sum / static_cast<double>(e - p));
            p = e;
        }
        for (int64_t q = g0; q < g1; ++q) {
            int64_t r = order[q];
            if (!std::isnan(y[r])) { o[r] = y[r]; continue; }  // observed y copies through regardless of x
            if (std::isnan(x[r])) continue;        // can't place a missing-y row without x
            if (anc.empty()) continue;
            double xr = x[r];
            // locate first anchor with x >= xr
            size_t hi = 0;
            while (hi < anc.size() && anc[hi].first < xr) ++hi;
            if (hi < anc.size() && anc[hi].first == xr) { o[r] = anc[hi].second; continue; }
            if (hi == 0) {  // below the range
                if (epolate && anc.size() >= 2) {
                    auto &a = anc[0], &b = anc[1];
                    o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
                }
            } else if (hi == anc.size()) {  // above the range
                if (epolate && anc.size() >= 2) {
                    auto &a = anc[anc.size() - 2], &b = anc[anc.size() - 1];
                    o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
                }
            } else {  // interpolate between anc[hi-1] and anc[hi]
                auto &a = anc[hi - 1], &b = anc[hi];
                o[r] = a.second + (b.second - a.second) * (xr - a.first) / (b.first - a.first);
            }
        }
        g0 = g1;
    }
    set_or_replace(out, std::move(o));
}

std::vector<std::string> Frame::mkspline_cubic(const std::string& stub, const std::string& var,
                                               int nknots, std::vector<double> knots) {
    const double* xp = require_f64(var);
    std::vector<double> x(xp, xp + n_);  // stable copy (set_or_replace may grow cols_)

    std::vector<double> tk;
    if (!knots.empty()) {
        tk = std::move(knots);  // user-supplied knots: used as-is, no clamping
    } else {
        std::vector<double> s;
        s.reserve(n_);
        for (int64_t i = 0; i < n_; ++i) if (!std::isnan(x[i])) s.push_back(x[i]);
        std::sort(s.begin(), s.end());
        const int m = static_cast<int>(s.size());
        if (m < 5) throw std::runtime_error("mkspline: too few non-missing observations");
        // Stata centile (default (n+1)p interpolation)
        auto cent = [&](double p) -> double {
            double R = (m + 1.0) * p / 100.0;
            int r = static_cast<int>(std::floor(R));
            double fr = R - r;
            if (r < 1) return s.front();
            if (r >= m) return s.back();
            return s[r - 1] + fr * (s[r] - s[r - 1]);
        };
        static const std::vector<double> P3{10, 50, 90}, P4{5, 35, 65, 95},
            P5{5, 27.5, 50, 72.5, 95}, P6{5, 23, 41, 59, 77, 95},
            P7{2.5, 18.33, 34.17, 50, 65.83, 81.67, 97.5};
        const std::vector<double>* pp = nullptr;
        switch (nknots) {
            case 3: pp = &P3; break; case 4: pp = &P4; break; case 5: pp = &P5; break;
            case 6: pp = &P6; break; case 7: pp = &P7; break;
            default: throw std::runtime_error("mkspline: nknots must be 3-7 for default knots");
        }
        for (double p : *pp) tk.push_back(cent(p));
        // outer-knot boundary clamping (default placement only)
        double mn = cent(5.0 / (m + 1) * 100.0), mx = cent((m - 4.0) / (m + 1) * 100.0);
        if (tk.front() < mn) tk.front() = mn;
        if (tk.back() > mx) tk.back() = mx;
    }

    const int nk = static_cast<int>(tk.size());
    if (nk < 3) throw std::runtime_error("mkspline: need at least 3 knots");
    if (!(tk[0] < tk[1]) || !(tk[nk - 1] > tk[nk - 2]))
        throw std::runtime_error("mkspline: sample size too small for this many knots");
    const double t1 = tk[0], tnk = tk[nk - 1], tkm1 = tk[nk - 2];
    const double span2 = (tnk - t1) * (tnk - t1);

    std::vector<std::string> names;
    set_or_replace(stub + "1", x);  // first term = raw x (linear)
    names.push_back(stub + "1");
    for (int j = 1; j <= nk - 2; ++j) {
        const double tj = tk[j - 1];
        const double c2 = (tnk - tj) / (tnk - tkm1);
        const double c3 = (tkm1 - tj) / (tnk - tkm1);
        std::vector<double> v(n_);
        for (int64_t i = 0; i < n_; ++i) {
            double xi = x[i];
            if (std::isnan(xi)) { v[i] = NAd; continue; }
            double uj = xi - tj;     if (uj < 0) uj = 0;
            double ukm1 = xi - tkm1; if (ukm1 < 0) ukm1 = 0;
            double unk = xi - tnk;   if (unk < 0) unk = 0;
            v[i] = (uj * uj * uj - ukm1 * ukm1 * ukm1 * c2 + unk * unk * unk * c3) / span2;
        }
        std::string nm = stub + std::to_string(j + 1);
        set_or_replace(nm, std::move(v));
        names.push_back(nm);
    }
    return names;
}

void Frame::bygen(const std::string& out, const std::string& expr,
                  const std::vector<std::string>& by_cols,
                  const std::vector<std::string>& sort_cols,
                  const std::string& mask_expr, StoreType st) {
    RngCallScope rng_call;
    Parser parser(expr, *this);
    auto ast = parser.parse();
    refuse_group_in_masked_range(ast.get(), expr, mask_expr);
    std::unique_ptr<Node> mast;
    if (!mask_expr.empty()) {
        Parser mp(mask_expr, *this);
        mast = mp.parse();
    }

    std::vector<std::string> sortkeys = by_cols;
    sortkeys.insert(sortkeys.end(), sort_cols.begin(), sort_cols.end());
    std::vector<std::vector<double>> KS = cols_as_double(*this, n_, sortkeys);
    std::vector<std::vector<double>> KB = cols_as_double(*this, n_, by_cols);

    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int64_t a, int64_t b) { return key_less(KS, a, b); });

    std::vector<int64_t> g0a(n_), g1a(n_), posa(n_);
    int64_t q = 0;
    while (q < n_) {
        int64_t r = q + 1;
        while (r < n_ && (by_cols.empty() || key_eq(KB, order[r], order[q]))) ++r;
        for (int64_t t = q; t < r; ++t) {
            int64_t row = order[t];
            g0a[row] = q; g1a[row] = r; posa[row] = t - q + 1;
        }
        q = r;
    }

    EvalCtx c;
    c.n_all = n_;
    c.order = &order;

    // The `if` of a by-prefixed command is evaluated inside each by-group, like the
    // expression: its _n, _N, x[1], x[_N], x[_n-1] and x[_n+1] count within the
    // group (w2m_bymask A01: `by gg: generate a01 = sum(_n) if _n > 1` answers
    // . 2 5 | . 2 5). It used to be evaluated over the dataset before the groups
    // existed, which answered . 2 5 | 1 3 6 at rc 0 (BUG-836). A sum() inside it
    // runs per group, as it does inside the folded cond() form.
    std::vector<double> mask;
    if (mast) {
        materialize_sums(mast.get(), n_, &order, &g0a, &g1a, &posa, nullptr);
        mask.resize(static_cast<size_t>(n_));
        for (int64_t i = 0; i < n_; ++i) {
            c.row = i; c.g0 = g0a[i]; c.g1 = g1a[i]; c.pos = posa[i];
            mask[static_cast<size_t>(i)] = ev(mast.get(), c);
        }
    }

    // sum() restarts at every group boundary and walks in SORT order, so it is
    // materialised here where that order exists -- not row by row below, which
    // iterates in original order.
    materialize_sums(ast.get(), n_, &order, &g0a, &g1a, &posa,
                     mask.empty() ? nullptr : &mask);

    std::vector<double> out_col(n_);
    for (int64_t i = 0; i < n_; ++i) {
        c.row = i; c.g0 = g0a[i]; c.g1 = g1a[i]; c.pos = posa[i];
        out_col[i] = ev(ast.get(), c);
    }
    if (!mask.empty())
        for (int64_t i = 0; i < n_; ++i)
            if (!if_selects(mask[static_cast<size_t>(i)])) out_col[i] = NAd;
    set_or_replace(out, std::move(out_col), st);
}

Frame::Summary Frame::summarize(const std::string& var, bool detail, const std::string& wvar,
                                bool analytic) const {
    // W:P5: weighted detail/percentiles now land (stata_pctile_weighted, above)
    // for BOTH weight kinds — the previous honest reject was a real gap, not a
    // semantic one: `summarize [aw=w] var, detail` is a legal Stata command and
    // the answerbook pins its full table.
    // A string/factor variable has no numeric observations. Stata's `summarize` on a string
    // variable SUCCEEDS with r(N)=0 (every value is numerically missing) — it is NOT an error —
    // so mirror that instead of throwing require_f64's "is a factor" runtime_error.
    if (col(var).is_factor) {
        Summary sf; sf.detail = detail;
        sf.mean = sf.sd = sf.var = sf.min = sf.max = NAd;
        if (detail) {
            sf.p1 = sf.p5 = sf.p10 = sf.p25 = sf.p50 = sf.p75 = sf.p90 = sf.p95 = sf.p99 = NAd;
            sf.skewness = sf.kurtosis = NAd;
        }
        return sf;
    }
    const double* x = require_f64(var);
    Summary s;
    s.detail = detail;
    std::vector<double> vals;
    double sum = 0;
    s.min = INF; s.max = -INF;
    if (wvar.empty()) {
        // ---- unweighted path: BYTE-UNCHANGED from pre-fweight cobra ----
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(x[i])) continue;
            ++s.n;
            sum += x[i];
            s.min = std::min(s.min, x[i]);
            s.max = std::max(s.max, x[i]);
            if (detail) vals.push_back(x[i]);
        }
        // No non-missing observations: Stata posts r(N)=0 but does NOT post r(mean)/r(sd)
        // (they read as missing) — emit missing rather than the struct-default 0.0.
        if (s.n == 0) { s.mean = s.sd = s.var = s.min = s.max = NAd; return s; }
        s.sum = sum;
        // BUG-344: Σw with w≡1 IS the observation count, and that is exactly what Stata
        // posts in r(sum_w) on an unweighted summarize. Leaving the struct default (0)
        // here made "unweighted" indistinguishable from "no weight total", so the only
        // honest value a caller could publish was none at all.
        s.sum_w = static_cast<double>(s.n);
        s.mean = sum / s.n;
        if (s.n >= 2) {
            double ss = 0;
            for (int64_t i = 0; i < n_; ++i) if (!std::isnan(x[i])) ss += (x[i] - s.mean) * (x[i] - s.mean);
            s.var = ss / (s.n - 1);
            s.sd = std::sqrt(s.var);
        } else {
            s.var = s.sd = NAd;
        }
        if (detail) {
            std::sort(vals.begin(), vals.end());
            s.p1 = stata_pctile(vals, 1);   s.p5 = stata_pctile(vals, 5);
            s.p10 = stata_pctile(vals, 10); s.p25 = stata_pctile(vals, 25);
            s.p50 = stata_pctile(vals, 50); s.p75 = stata_pctile(vals, 75);
            s.p90 = stata_pctile(vals, 90); s.p95 = stata_pctile(vals, 95);
            s.p99 = stata_pctile(vals, 99);
            // population moments for skewness/kurtosis (Stata convention)
            double m2 = 0, m3 = 0, m4 = 0;
            for (double v : vals) { double d = v - s.mean; m2 += d * d; m3 += d * d * d; m4 += d * d * d * d; }
            m2 /= s.n; m3 /= s.n; m4 /= s.n;
            double sp = std::sqrt(m2);
            s.skewness = sp > 0 ? m3 / (sp * sp * sp) : NAd;
            s.kurtosis = m2 > 0 ? m4 / (m2 * m2) : NAd;
        }
        return s;
    }
    // ---- weighted path (Stata fweight): N=Sum(w), mean=Sum(w*x)/Sum(w), sd denom Sum(w)-1 ----
    const double* w = require_f64(wvar);
    // Validate the WHOLE weight column up front (Stata checks fweight integrality over the
    // sample regardless of which other variable ends up missing on a given row): any non-missing
    // weight must be a non-negative integer, else honest bad_input (never silently rounds).
    // BUG-107: a NEGATIVE weight is a hard error too (Stata "negative weights encountered") —
    // previously only integrality was checked, so w=-1 silently made sumwx go negative → a
    // reported mean/sum OUTSIDE the data range.
    for (int64_t i = 0; i < n_; ++i) {
        if (std::isnan(w[i])) continue;
        if (w[i] < 0.0)
            throw EngineError("bad_input",
                "summarize: " + std::string(analytic ? "aweight" : "fweight") + " '" + wvar +
                "' has a negative value (" + std::to_string(w[i]) +
                "); " + (analytic ? "analytic" : "frequency") + " weights must be non-negative", wvar);
        // Integrality is a frequency-weight rule only: an analytic weight is a
        // precision/importance scale and 1.5 is a perfectly legal value.
        if (!analytic && w[i] != std::floor(w[i]))
            throw EngineError("bad_input",
                "summarize: fweight '" + wvar + "' has a non-integer value (" +
                std::to_string(w[i]) + "); Stata requires integer frequency weights", wvar);
    }
    double sumw = 0.0, sumwx = 0.0;
    int64_t nrows = 0;
    // W:P5 detail: collect the (value, weight) pairs the moment loop below
    // would otherwise have to re-derive, so percentiles and the weighted
    // moments read the SAME included-row set as N/mean/var.
    std::vector<std::pair<double, double>> wvals;
    for (int64_t i = 0; i < n_; ++i) {
        // A missing weight drops the row outright (as if absent from the dataset). A missing `x`
        // excludes the row from N/mean/var (its already-validated weight contributes nowhere).
        // BUG-107: a zero weight = zero frequency = the obs is not in the dataset, so it must be
        // excluded from min/max too (it already contributes nothing to sumw/sumwx/N).
        if (std::isnan(w[i]) || std::isnan(x[i]) || w[i] == 0.0) continue;
        ++nrows;
        sumw += w[i];
        sumwx += w[i] * x[i];
        s.min = std::min(s.min, x[i]);
        s.max = std::max(s.max, x[i]);
        if (detail) wvals.emplace_back(x[i], w[i]);
    }
    if (sumw == 0.0) { s.n = 0; s.mean = s.sd = s.var = s.min = s.max = NAd; return s; }
    // The one place the two weight kinds genuinely diverge: a frequency weight IS the
    // observation count, an analytic weight is a relative precision over `nrows` real
    // observations. Stata prints the former as Obs and the latter as Obs + Weight.
    s.n = analytic ? nrows : static_cast<int64_t>(std::llround(sumw));
    s.sum_w = sumw;
    s.sum = sumwx;
    s.mean = sumwx / sumw;
    // Denominator: fw divides by Sum(w)-1 (the replicated sample size); aw rescales the
    // weights to sum to n first, so the divisor is n-1 on the ORIGINAL observation count.
    const double vscale = analytic ? static_cast<double>(nrows) / sumw : 1.0;
    const double vdenom = analytic ? static_cast<double>(nrows) - 1.0 : sumw - 1.0;
    if (vdenom >= 1.0) {
        double ss = 0;
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(w[i]) || std::isnan(x[i])) continue;
            double d = x[i] - s.mean;
            ss += w[i] * vscale * d * d;
        }
        s.var = ss / vdenom;
        s.sd = std::sqrt(s.var);
    } else {
        s.var = s.sd = NAd;
    }
    if (detail) {
        std::sort(wvals.begin(), wvals.end(),
                  [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                      return a.first < b.first;
                  });
        std::vector<double> pv, pw;
        pv.reserve(wvals.size());
        pw.reserve(wvals.size());
        for (const auto& vw : wvals) { pv.push_back(vw.first); pw.push_back(vw.second); }
        s.p1 = stata_pctile_weighted(pv, pw, sumw, 1);
        s.p5 = stata_pctile_weighted(pv, pw, sumw, 5);
        s.p10 = stata_pctile_weighted(pv, pw, sumw, 10);
        s.p25 = stata_pctile_weighted(pv, pw, sumw, 25);
        s.p50 = stata_pctile_weighted(pv, pw, sumw, 50);
        s.p75 = stata_pctile_weighted(pv, pw, sumw, 75);
        s.p90 = stata_pctile_weighted(pv, pw, sumw, 90);
        s.p95 = stata_pctile_weighted(pv, pw, sumw, 95);
        s.p99 = stata_pctile_weighted(pv, pw, sumw, 99);
        // Weighted population moments about the weighted mean — the same
        // Stata convention the unweighted branch uses, with the row count
        // replaced by the weight total. Both weight kinds share this: the
        // fw/aw divergence lives in the VARIANCE denominator (above), not in
        // these shape statistics, which are scale-free ratios.
        double m2 = 0, m3 = 0, m4 = 0;
        for (const auto& vw : wvals) {
            const double d = vw.first - s.mean;
            const double dd = d * d;
            m2 += vw.second * dd;
            m3 += vw.second * dd * d;
            m4 += vw.second * dd * dd;
        }
        m2 /= sumw; m3 /= sumw; m4 /= sumw;
        const double sp = std::sqrt(m2);
        s.skewness = sp > 0 ? m3 / (sp * sp * sp) : NAd;
        s.kurtosis = m2 > 0 ? m4 / (m2 * m2) : NAd;
    }
    return s;
}

Frame Frame::collapse(const std::vector<CollapseSpec>& specs,
                      const std::vector<std::string>& by, const std::string& wvar) const {
    std::vector<int64_t> order(n_);
    std::iota(order.begin(), order.end(), 0);
    std::vector<std::vector<double>> K;
    if (!by.empty()) {
        K = cols_as_double(*this, n_, by);
        std::stable_sort(order.begin(), order.end(),
                         [&](int64_t a, int64_t b) { return key_less(K, a, b); });
    }
    std::vector<const double*> vp;
    for (const auto& s : specs) vp.push_back(require_f64(s.var));
    // by-columns may be factor OR numeric: K (above) already casts a factor by-column
    // to its int32 code as a double (cols_as_double), so grouping just works. Track
    // which by-columns are factor so the output can rebuild a fresh, self-consistent
    // dictionary below instead of require_f64()-rejecting them outright (this WAS the
    // "column 'X' is a factor, not f64" reject some callers depended on — see the A12
    // dependent sweep in frame.h's collapse() doc comment / the CONTRACT-engine.md diff).
    std::vector<char> by_is_factor(by.size());
    for (size_t bc = 0; bc < by.size(); ++bc) by_is_factor[bc] = col(by[bc]).is_factor ? 1 : 0;
    // fweight (optional): grouping itself (the `by` columns above) is NOT affected by weights.
    const double* wcol = nullptr;
    if (!wvar.empty()) {
        wcol = require_f64(wvar);
        // Validate the WHOLE weight column up front, same rule/rationale as Frame::summarize
        // (BUG-107: negative weights are a hard error, not just non-integers).
        for (int64_t i = 0; i < n_; ++i) {
            if (std::isnan(wcol[i])) continue;
            if (wcol[i] < 0.0)
                throw EngineError("bad_input",
                    "collapse: fweight '" + wvar + "' has a negative value (" +
                    std::to_string(wcol[i]) + "); frequency weights must be non-negative", wvar);
            if (wcol[i] != std::floor(wcol[i]))
                throw EngineError("bad_input",
                    "collapse: fweight '" + wvar + "' has a non-integer value (" +
                    std::to_string(wcol[i]) + "); Stata requires integer frequency weights", wvar);
        }
    }

    // group boundaries over `order`
    std::vector<std::pair<int64_t, int64_t>> groups;
    int64_t g0 = 0;
    while (g0 < n_) {
        int64_t g1 = g0 + 1;
        while (g1 < n_ && (by.empty() || key_eq(K, order[g1], order[g0]))) ++g1;
        groups.emplace_back(g0, g1);
        g0 = g1;
    }
    int64_t G = static_cast<int64_t>(groups.size());

    std::vector<std::pair<std::string, std::vector<double>>> outcols;
    for (size_t b = 0; b < by.size(); ++b) outcols.push_back({by[b], std::vector<double>(G, NAd)});
    for (const auto& s : specs) outcols.push_back({s.out.empty() ? s.var : s.out, std::vector<double>(G, NAd)});

    // MAXPERF Stage 3 — exception hoist: the per-group aggregation below runs on
    // ThreadPool workers (a std::function on a std::thread), so a throw escaping it
    // would std::terminate. Every throw the group loop can raise depends ONLY on the
    // stat string / weighting (never on a group's data), so pre-validate each spec HERE,
    // on the caller, reproducing the SAME conditions & messages the inner branches emit.
    // The inner `else throw`/fweight-EngineError lines are then unreachable (kept as
    // defensive dead code). Guarded by G>0 so an empty input still returns an empty frame
    // WITHOUT throwing on a bad stat — byte-identical to the pre-parallel behavior.
    if (G > 0) {
        static const std::unordered_set<std::string> kAggKnown = {
            "sum", "count", "n", "max", "min", "mean", "sd",
            "first", "last", "firstnm", "lastnm"};
        for (const auto& s : specs) {
            const std::string& st = s.statistic;
            if (!wvar.empty()) {
                // weighted path: median/p50/p<NN> are pre-rejected wholesale (mirrors the
                // inner guard, which fires for ANY 'p…' token before parsing it).
                if (st == "median" || st == "p50" || (st.size() > 1 && st[0] == 'p'))
                    throw EngineError("bad_input",
                        "collapse: fweight not supported with median/percentile stats yet (stat '" +
                        st + "')", wvar);
                if (!kAggKnown.count(st)) throw std::runtime_error("collapse: unknown stat '" + st + "'");
            } else {
                if (kAggKnown.count(st) || st == "median" || st == "p50") continue;
                if (st.size() > 1 && st[0] == 'p') {
                    // strict-consume the percentile number (C1-F11 discipline), identical to
                    // the inner parse — `p2x5` must reject, not silently read as p2.
                    const std::string pstr = st.substr(1);
                    size_t consumed = 0;
                    double pp;
                    try { pp = std::stod(pstr, &consumed); (void)pp; }
                    catch (const std::exception&) { throw std::runtime_error("collapse: unknown stat '" + st + "'"); }
                    if (consumed != pstr.size()) throw std::runtime_error("collapse: unknown stat '" + st + "'");
                    continue;
                }
                throw std::runtime_error("collapse: unknown stat '" + st + "'");
            }
        }
    }

    // MAXPERF Stage 3 — morsel-parallel collapse, BY GROUP. Each group gi is aggregated
    // WHOLE by one worker, visiting its rows in the SAME order[a..b) sequence as the serial
    // path, and writes only its own disjoint output cells outcols[*][gi]. So every float
    // accumulation (sum/mean/sd) keeps its exact per-group association — the result is
    // BIT-IDENTICAL regardless of how ThreadPool splits [0,G) across workers (①-gate:
    // test_morsel_determinism). ThreadPool::run falls back to serial for G<2 / 1 core /
    // nested, so tiny/degenerate collapses are byte-unchanged.
    // BUG-394 (2026-09-02): a GROUP floor — under 16 groups the fan-out is pure
    // dispatch overhead (each group is one unit of work). Bit-identical either way.
    auto collapse_body = [&](int64_t glo, int64_t ghi, int) {
      for (int64_t gi = glo; gi < ghi; ++gi) {
        int64_t a = groups[gi].first, b = groups[gi].second;
        // K[bc][order[a]] is byte-identical to the old require_f64(by[bc])[order[a]] for
        // a numeric by-column (cols_as_double's non-factor branch is literally c.f64[i]);
        // for a factor by-column it's the ORIGINAL int32 code cast to double — rebuilt
        // into a fresh factor dictionary in the final assembly below.
        for (size_t bc = 0; bc < by.size(); ++bc) outcols[bc].second[gi] = K[bc][order[a]];
        for (size_t si = 0; si < specs.size(); ++si) {
            const std::string& st = specs[si].statistic;
            double agg = NAd;
            if (wvar.empty()) {
                // ---- unweighted path: BYTE-UNCHANGED from pre-fweight cobra ----
                std::vector<double> s;
                for (int64_t q = a; q < b; ++q) { double x = vp[si][order[q]]; if (!std::isnan(x)) s.push_back(x); }
                const int64_t cnt = static_cast<int64_t>(s.size());
                if (st == "sum") { double t = 0; for (double x : s) t += x; agg = t; }
                else if (st == "count" || st == "n") agg = static_cast<double>(cnt);
                else if (st == "max") { if (cnt) agg = *std::max_element(s.begin(), s.end()); }
                else if (st == "min") { if (cnt) agg = *std::min_element(s.begin(), s.end()); }
                else if (st == "mean") { if (cnt) { double t = 0; for (double x : s) t += x; agg = t / cnt; } }
                else if (st == "sd") {
                    if (cnt >= 2) { double t = 0; for (double x : s) t += x; double mn = t / cnt;
                        double ss = 0; for (double x : s) ss += (x - mn) * (x - mn); agg = std::sqrt(ss / (cnt - 1)); }
                } else if (st == "first") { agg = vp[si][order[a]]; }        // RAW: first row, missing included
                else if (st == "last") { agg = vp[si][order[b - 1]]; }        // RAW: last row, missing included
                else if (st == "firstnm") { if (cnt) agg = s.front(); }       // first NON-missing value
                else if (st == "lastnm") { if (cnt) agg = s.back(); }         // last  NON-missing value
                else if (st == "median" || st == "p50") { if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, 50.0); } }
                else if (st.size() > 1 && st[0] == 'p') {
                    // BUG-112(4): strict-consume the percentile number (matches the C1-F11
                    // literal discipline used in the expression evaluator) — `p2x5` must
                    // reject, not silently read as p2.
                    const std::string pstr = st.substr(1);
                    size_t consumed = 0;
                    double pp;
                    try { pp = std::stod(pstr, &consumed); }
                    catch (const std::exception&) { throw std::runtime_error("collapse: unknown stat '" + st + "'"); }
                    if (consumed != pstr.size()) throw std::runtime_error("collapse: unknown stat '" + st + "'");
                    if (cnt) { std::sort(s.begin(), s.end()); agg = stata_pctile(s, pp); }
                } else throw std::runtime_error("collapse: unknown stat '" + st + "'");
            } else {
                // ---- weighted path (Stata fweight), per group ----
                // count=Sum(w), sum=Sum(w*x), mean=Sum(w*x)/Sum(w), sd denom Sum(w)-1;
                // min/max/first/last stay plain unweighted row values (weight only gates
                // inclusion — a row with missing wvar is excluded here exactly like a row with
                // missing `var`). Percentile stats need a replicate algorithm — out of scope.
                if (st == "median" || st == "p50" || (st.size() > 1 && st[0] == 'p'))
                    throw EngineError("bad_input",
                        "collapse: fweight not supported with median/percentile stats yet (stat '" +
                        st + "')", wvar);
                std::vector<double> s, sw;  // parallel: s[i] is the value, sw[i] its weight
                for (int64_t q = a; q < b; ++q) {
                    double x = vp[si][order[q]];
                    double wi = wcol[order[q]];
                    // BUG-107: a zero-frequency (w==0) row is not in the dataset — exclude it
                    // from min/max (and everything). It already contributed 0 to sum/count/mean.
                    if (std::isnan(x) || std::isnan(wi) || wi == 0.0) continue;
                    s.push_back(x);
                    sw.push_back(wi);
                }
                const int64_t cnt = static_cast<int64_t>(s.size());
                double sumw = 0.0, sumwx = 0.0;
                for (int64_t i = 0; i < cnt; ++i) {
                    sumw += sw[static_cast<size_t>(i)];
                    sumwx += sw[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
                }
                if (st == "sum") agg = sumwx;
                else if (st == "count" || st == "n") agg = sumw;
                else if (st == "max") { if (cnt) agg = *std::max_element(s.begin(), s.end()); }
                else if (st == "min") { if (cnt) agg = *std::min_element(s.begin(), s.end()); }
                else if (st == "mean") { if (sumw > 0.0) agg = sumwx / sumw; }
                else if (st == "sd") {
                    if (sumw >= 2.0) {
                        double mn = sumwx / sumw, ss = 0;
                        for (int64_t i = 0; i < cnt; ++i) {
                            double d = s[static_cast<size_t>(i)] - mn;
                            ss += sw[static_cast<size_t>(i)] * d * d;
                        }
                        agg = std::sqrt(ss / (sumw - 1.0));
                    }
                } else if (st == "first" || st == "last") {
                    // RAW first/last row among fweight-INCLUDED rows (missing x kept; a
                    // missing-weight OR zero-weight row is dropped by fweight, as if absent
                    // from the data — BUG-107).
                    for (int64_t q = a; q < b; ++q) {
                        double wi = wcol[order[q]];
                        if (std::isnan(wi) || wi == 0.0) continue;
                        agg = vp[si][order[q]];
                        if (st == "first") break;
                    }
                }
                else if (st == "firstnm") { if (cnt) agg = s.front(); }
                else if (st == "lastnm") { if (cnt) agg = s.back(); }
                else throw std::runtime_error("collapse: unknown stat '" + st + "'");
            }
            outcols[by.size() + si].second[gi] = agg;
        }
      }
    };
    if (G < 16) collapse_body(0, G, 0);
    else ThreadPool::global().run(G, collapse_body);
    Frame out;
    out.n_ = G;
    out.cols_.reserve(outcols.size());
    for (size_t oc = 0; oc < outcols.size(); ++oc) {
        auto& nc = outcols[oc];
        if (oc < by.size() && by_is_factor[oc]) {
            // Rebuild a FRESH, self-consistent factor dictionary from just the codes
            // actually present among the G output rows, in FIRST-SEEN output-row order
            // (same convention `cobra convert --factor` produces) — NOT alphabetical
            // (contrast egen_group(), which intentionally re-sorts alphabetically).
            const FrameColumn& src_col = col(by[oc]);
            std::vector<int32_t> new_codes(static_cast<size_t>(G));
            std::vector<std::string> new_levels;
            std::unordered_map<std::string, int32_t> dict;
            for (int64_t gi = 0; gi < G; ++gi) {
                double raw = nc.second[static_cast<size_t>(gi)];
                int32_t orig_code = (raw < 0.0) ? -1 : static_cast<int32_t>(std::llround(raw));
                if (orig_code < 0) { new_codes[static_cast<size_t>(gi)] = -1; continue; }
                const std::string& label = src_col.levels[static_cast<size_t>(orig_code)];
                auto it = dict.find(label);
                if (it == dict.end()) {
                    int32_t code = static_cast<int32_t>(new_levels.size());
                    dict.emplace(label, code);
                    new_levels.push_back(label);
                    new_codes[static_cast<size_t>(gi)] = code;
                } else {
                    new_codes[static_cast<size_t>(gi)] = it->second;
                }
            }
            FrameColumn c;
            c.name = nc.first;
            c.is_factor = true;
            c.codes = std::move(new_codes);
            c.levels = std::move(new_levels);
            out.cols_.push_back(std::move(c));
        } else {
            FrameColumn c;
            c.name = nc.first;
            c.is_factor = false;
            c.f64 = std::move(nc.second);
            out.cols_.push_back(std::move(c));
        }
    }
    out.reindex();
    return out;
}


}  // namespace cecon
