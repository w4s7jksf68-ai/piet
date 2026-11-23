// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The [P] matrix subcommands that manage or decompose a stored matrix:
// `matrix dir`, `matrix rename`, `matrix roweq`/`coleq` (through
// runMatrixStripe), `matrix symeigen`, `matrix eigenvalues` and `matrix svd`.
// runMatrixDef (matrix_commands.cpp) hands every such word here before its
// "not implemented" fallback.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "data/missing.h"  // cecon::is_missing
#include "engine/command_support.h"
#include "matrix/matrix_expression_internal.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/eval/session.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/handlers/session_commands.h"  // runMatname
#include "piet/handlers/variables.h"  // resolveByKeyNames
#include "piet/inrange_filter.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::handlers {

// The if/in selection keeping each row's observation number (engine/inrange_filter.cpp).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState&, const std::string&,
                                                   const std::optional<std::string>&,
                                                   const std::optional<std::string>&, std::string*);

namespace command_detail {
namespace {

using Mat = std::vector<std::vector<double>>;

std::vector<std::string> words(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  for (std::string w; iss >> w;) out.push_back(w);
  return out;
}

eval::EvalMatrix* userMatrix(SessionState& session, const std::string& name) {
  for (auto& p : session.eval.matrices)
    if (p.first == name) return &p.second;
  return nullptr;
}

bool hasMissing(const eval::EvalMatrix& m) {
  for (const auto& row : m.data)
    for (double v : row)
      if (std::isnan(v) || v >= eval::kStataMissingSentinel) return true;
  return false;
}

// `NAME1 [NAME2 ...] = A`: the result names before the `=` and the source
// matrix after it, which must be a stored matrix (r(111) otherwise).
struct DecompArgs {
  std::vector<std::string> outs;
  eval::EvalMatrix src;
};

std::optional<ojson> parseDecomp(const std::string& args, size_t nOuts, SessionState& session,
                                 DecompArgs& out) {
  const size_t eq = args.find('=');
  if (eq == std::string::npos) return engineErrorBlockRc("invalid syntax", 198);
  out.outs = words(args.substr(0, eq));
  const std::vector<std::string> rhs = words(args.substr(eq + 1));
  if (out.outs.size() != nOuts || rhs.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
  for (const auto& n : out.outs)
    if (!strutil::isValidStataIdentifier(n)) return engineErrorBlockRc(n + " invalid name", 198);
  const eval::EvalMatrix* a = session.eval.findMatrix(rhs[0]);
  if (a == nullptr) return engineErrorBlockRc("matrix " + rhs[0] + " not found", 111);
  out.src = *a;
  if (hasMissing(out.src)) return engineErrorBlockRc("matrix has missing values", 504);
  return std::nullopt;
}

std::vector<std::string> seqNames(const char* stem, size_t n) {
  std::vector<std::string> v;
  for (size_t i = 1; i <= n; ++i) v.push_back(stem + std::to_string(i));
  return v;
}

double signOf(double a, double b) { return b >= 0.0 ? std::fabs(a) : -std::fabs(a); }

// sqrt(a^2 + b^2) without destructive underflow or overflow, by the EISPACK
// pythag iteration (Moler and Morrison 1983). Its last-bit rounding is what
// leaves a cancelled eigenvector element the sign StataBE prints (vs-stata
// bank ed2c6a3b); std::hypot rounds that element the other way.
double pythag(double a, double b) {
  double p = std::max(std::fabs(a), std::fabs(b));
  if (p == 0.0) return p;
  double r = std::min(std::fabs(a), std::fabs(b)) / p;
  r *= r;
  for (;;) {
    const double t = 4.0 + r;
    if (t == 4.0) return p;
    const double s = r / t;
    const double u = 1.0 + 2.0 * s;
    p = u * p;
    const double su = s / u;
    r = su * su * r;
  }
}

// Householder reduction of a real symmetric matrix to tridiagonal form,
// accumulating the transformations (the EISPACK tred2 algorithm; Wilkinson and
// Reinsch 1971), 1-based work arrays.
void tred2(size_t n, Mat& z, std::vector<double>& d, std::vector<double>& e) {
  for (size_t i = 1; i <= n; ++i) d[i] = z[n][i];
  if (n > 1) {
    for (size_t ii = 2; ii <= n; ++ii) {
      const size_t i = n + 2 - ii;
      const size_t l = i - 1;
      double h = 0.0, scale = 0.0;
      if (l >= 2)
        for (size_t k = 1; k <= l; ++k) scale += std::fabs(d[k]);
      if (l < 2 || scale == 0.0) {
        e[i] = d[l];
        for (size_t j = 1; j <= l; ++j) {
          d[j] = z[l][j];
          z[i][j] = 0.0;
          z[j][i] = 0.0;
        }
      } else {
        for (size_t k = 1; k <= l; ++k) {
          d[k] /= scale;
          h += d[k] * d[k];
        }
        double f = d[l];
        double g = -signOf(std::sqrt(h), f);
        e[i] = scale * g;
        h -= f * g;
        d[l] = f - g;
        for (size_t j = 1; j <= l; ++j) e[j] = 0.0;
        for (size_t j = 1; j <= l; ++j) {
          f = d[j];
          z[j][i] = f;
          g = e[j] + z[j][j] * f;
          for (size_t k = j + 1; k <= l; ++k) {
            g += z[k][j] * d[k];
            e[k] += z[k][j] * f;
          }
          e[j] = g;
        }
        f = 0.0;
        for (size_t j = 1; j <= l; ++j) {
          e[j] /= h;
          f += e[j] * d[j];
        }
        const double hh = f / (h + h);
        for (size_t j = 1; j <= l; ++j) e[j] -= hh * d[j];
        for (size_t j = 1; j <= l; ++j) {
          f = d[j];
          g = e[j];
          for (size_t k = j; k <= l; ++k) z[k][j] -= f * e[k] + g * d[k];
          d[j] = z[l][j];
          z[i][j] = 0.0;
        }
      }
      d[i] = h;
    }
    for (size_t i = 2; i <= n; ++i) {
      const size_t l = i - 1;
      z[n][l] = z[l][l];
      z[l][l] = 1.0;
      const double h = d[i];
      if (h != 0.0) {
        for (size_t k = 1; k <= l; ++k) d[k] = z[k][i] / h;
        for (size_t j = 1; j <= l; ++j) {
          double g = 0.0;
          for (size_t k = 1; k <= l; ++k) g += z[k][i] * z[k][j];
          for (size_t k = 1; k <= l; ++k) z[k][j] -= g * d[k];
        }
      }
      for (size_t k = 1; k <= l; ++k) z[k][i] = 0.0;
    }
  }
  for (size_t i = 1; i <= n; ++i) {
    d[i] = z[n][i];
    z[n][i] = 0.0;
  }
  z[n][n] = 1.0;
  e[1] = 0.0;
}

// The QL algorithm with implicit shifts on the tridiagonal form (EISPACK tql2),
// eigenvalues ascending. False when an eigenvalue takes more than 30 sweeps.
bool tql2(size_t n, Mat& z, std::vector<double>& d, std::vector<double>& e) {
  if (n == 1) return true;
  for (size_t i = 2; i <= n; ++i) e[i - 1] = e[i];
  double f = 0.0, tst1 = 0.0;
  e[n] = 0.0;
  for (size_t l = 1; l <= n; ++l) {
    int iter = 0;
    const double hl = std::fabs(d[l]) + std::fabs(e[l]);
    if (tst1 < hl) tst1 = hl;
    size_t m = l;
    for (; m <= n; ++m)
      if (tst1 + std::fabs(e[m]) == tst1) break;
    if (m != l) {
      for (;;) {
        if (iter == 30) return false;
        ++iter;
        const size_t l1 = l + 1, l2 = l1 + 1;
        double g = d[l];
        double p = (d[l1] - g) / (2.0 * e[l]);
        double r = pythag(p, 1.0);
        d[l] = e[l] / (p + signOf(r, p));
        d[l1] = e[l] * (p + signOf(r, p));
        const double dl1 = d[l1];
        double h = g - d[l];
        for (size_t i = l2; i <= n; ++i) d[i] -= h;
        f += h;
        p = d[m];
        double c = 1.0, c2 = c, c3 = c, s = 0.0, s2 = 0.0;
        const double el1 = e[l1];
        for (size_t ii = 1; ii <= m - l; ++ii) {
          c3 = c2;
          c2 = c;
          s2 = s;
          const size_t i = m - ii;
          g = c * e[i];
          h = c * p;
          r = pythag(p, e[i]);
          e[i + 1] = s * r;
          s = e[i] / r;
          c = p / r;
          p = c * d[i] - s * g;
          d[i + 1] = h + s * (c * g + s * d[i]);
          for (size_t k = 1; k <= n; ++k) {
            h = z[k][i + 1];
            z[k][i + 1] = s * z[k][i] + c * h;
            z[k][i] = c * z[k][i] - s * h;
          }
        }
        p = -s * s2 * c3 * el1 * e[l] / dl1;
        e[l] = s * p;
        d[l] = c * p;
        if (!(tst1 + std::fabs(e[l]) > tst1)) break;
      }
    }
    d[l] += f;
    e[l] = 0.0;
  }
  for (size_t ii = 2; ii <= n; ++ii) {
    const size_t i = ii - 1;
    size_t k = i;
    double p = d[i];
    for (size_t j = ii; j <= n; ++j)
      if (d[j] < p) {
        k = j;
        p = d[j];
      }
    if (k != i) {
      d[k] = d[i];
      d[i] = p;
      for (size_t j = 1; j <= n; ++j) std::swap(z[j][i], z[j][k]);
    }
  }
  return true;
}

// The Golub-Reinsch singular value decomposition A = U diag(w) V' of an m x n
// matrix, m >= n (Householder bidiagonalization, then implicit-shift QR on the
// bidiagonal form; Golub and Reinsch 1970). The singular values come out in the
// order the iteration leaves them, unsorted, each made nonnegative. 1-based.
bool svdGolubReinsch(size_t m, size_t n, Mat& a, std::vector<double>& w, Mat& v) {
  std::vector<double> rv1(n + 1, 0.0);
  double g = 0.0, scale = 0.0, anorm = 0.0;
  size_t l = 0;
  for (size_t i = 1; i <= n; ++i) {
    l = i + 1;
    rv1[i] = scale * g;
    g = 0.0;
    double s = 0.0;
    scale = 0.0;
    if (i <= m) {
      for (size_t k = i; k <= m; ++k) scale += std::fabs(a[k][i]);
      if (scale != 0.0) {
        for (size_t k = i; k <= m; ++k) {
          a[k][i] /= scale;
          s += a[k][i] * a[k][i];
        }
        double f = a[i][i];
        g = -signOf(std::sqrt(s), f);
        const double h = f * g - s;
        a[i][i] = f - g;
        for (size_t j = l; j <= n; ++j) {
          s = 0.0;
          for (size_t k = i; k <= m; ++k) s += a[k][i] * a[k][j];
          f = s / h;
          for (size_t k = i; k <= m; ++k) a[k][j] += f * a[k][i];
        }
        for (size_t k = i; k <= m; ++k) a[k][i] *= scale;
      }
    }
    w[i] = scale * g;
    g = 0.0;
    s = 0.0;
    scale = 0.0;
    if (i <= m && i != n) {
      for (size_t k = l; k <= n; ++k) scale += std::fabs(a[i][k]);
      if (scale != 0.0) {
        for (size_t k = l; k <= n; ++k) {
          a[i][k] /= scale;
          s += a[i][k] * a[i][k];
        }
        const double f = a[i][l];
        g = -signOf(std::sqrt(s), f);
        const double h = f * g - s;
        a[i][l] = f - g;
        for (size_t k = l; k <= n; ++k) rv1[k] = a[i][k] / h;
        for (size_t j = l; j <= m; ++j) {
          s = 0.0;
          for (size_t k = l; k <= n; ++k) s += a[j][k] * a[i][k];
          for (size_t k = l; k <= n; ++k) a[j][k] += s * rv1[k];
        }
        for (size_t k = l; k <= n; ++k) a[i][k] *= scale;
      }
    }
    anorm = std::max(anorm, std::fabs(w[i]) + std::fabs(rv1[i]));
  }
  for (size_t i = n; i >= 1; --i) {
    if (i < n) {
      if (g != 0.0) {
        for (size_t j = l; j <= n; ++j) v[j][i] = (a[i][j] / a[i][l]) / g;
        for (size_t j = l; j <= n; ++j) {
          double s = 0.0;
          for (size_t k = l; k <= n; ++k) s += a[i][k] * v[k][j];
          for (size_t k = l; k <= n; ++k) v[k][j] += s * v[k][i];
        }
      }
      for (size_t j = l; j <= n; ++j) v[i][j] = v[j][i] = 0.0;
    }
    v[i][i] = 1.0;
    g = rv1[i];
    l = i;
  }
  for (size_t i = std::min(m, n); i >= 1; --i) {
    l = i + 1;
    g = w[i];
    for (size_t j = l; j <= n; ++j) a[i][j] = 0.0;
    if (g != 0.0) {
      g = 1.0 / g;
      for (size_t j = l; j <= n; ++j) {
        double s = 0.0;
        for (size_t k = l; k <= m; ++k) s += a[k][i] * a[k][j];
        const double f = (s / a[i][i]) * g;
        for (size_t k = i; k <= m; ++k) a[k][j] += f * a[k][i];
      }
      for (size_t j = i; j <= m; ++j) a[j][i] *= g;
    } else {
      for (size_t j = i; j <= m; ++j) a[j][i] = 0.0;
    }
    a[i][i] += 1.0;
  }
  for (size_t k = n; k >= 1; --k) {
    for (int its = 1; its <= 30; ++its) {
      bool flag = true;
      size_t nm = 0;
      for (l = k; l >= 1; --l) {
        nm = l - 1;
        if (std::fabs(rv1[l]) + anorm == anorm) {
          flag = false;
          break;
        }
        if (std::fabs(w[nm]) + anorm == anorm) break;
      }
      if (flag) {
        double c = 0.0, s = 1.0;
        for (size_t i = l; i <= k; ++i) {
          const double f = s * rv1[i];
          rv1[i] = c * rv1[i];
          if (std::fabs(f) + anorm == anorm) break;
          g = w[i];
          double h = std::hypot(f, g);
          w[i] = h;
          h = 1.0 / h;
          c = g * h;
          s = -f * h;
          for (size_t j = 1; j <= m; ++j) {
            const double y = a[j][nm], z = a[j][i];
            a[j][nm] = y * c + z * s;
            a[j][i] = z * c - y * s;
          }
        }
      }
      double z = w[k];
      if (l == k) {
        if (z < 0.0) {
          w[k] = -z;
          for (size_t j = 1; j <= n; ++j) v[j][k] = -v[j][k];
        }
        break;
      }
      if (its == 30) return false;
      double x = w[l];
      nm = k - 1;
      double y = w[nm];
      g = rv1[nm];
      double h = rv1[k];
      double f = ((y - z) * (y + z) + (g - h) * (g + h)) / (2.0 * h * y);
      g = std::hypot(f, 1.0);
      f = ((x - z) * (x + z) + h * ((y / (f + signOf(g, f))) - h)) / x;
      double c = 1.0, s = 1.0;
      for (size_t j = l; j <= nm; ++j) {
        const size_t i = j + 1;
        g = rv1[i];
        y = w[i];
        h = s * g;
        g = c * g;
        z = std::hypot(f, h);
        rv1[j] = z;
        c = f / z;
        s = h / z;
        f = x * c + g * s;
        g = g * c - x * s;
        h = y * s;
        y *= c;
        for (size_t jj = 1; jj <= n; ++jj) {
          x = v[jj][j];
          z = v[jj][i];
          v[jj][j] = x * c + z * s;
          v[jj][i] = z * c - x * s;
        }
        z = std::hypot(f, h);
        w[j] = z;
        if (z != 0.0) {
          z = 1.0 / z;
          c = f * z;
          s = h * z;
        }
        f = c * g + s * y;
        x = c * y - s * g;
        for (size_t jj = 1; jj <= m; ++jj) {
          y = a[jj][j];
          z = a[jj][i];
          a[jj][j] = y * c + z * s;
          a[jj][i] = z * c - y * s;
        }
      }
      rv1[l] = 0.0;
      rv1[k] = f;
      w[k] = x;
    }
  }
  return true;
}

// Eigenvalues of a real general matrix: balancing, reduction to upper
// Hessenberg form by stabilized elementary similarity transformations, then
// the shifted QR algorithm on the Hessenberg form (the EISPACK balanc, elmhes
// and hqr algorithms). 1-based; wr/wi the real and imaginary parts.
void balance(size_t n, Mat& a) {
  const double radix = 2.0, sqrdx = radix * radix;
  bool done = false;
  while (!done) {
    done = true;
    for (size_t i = 1; i <= n; ++i) {
      double r = 0.0, c = 0.0;
      for (size_t j = 1; j <= n; ++j)
        if (j != i) {
          c += std::fabs(a[j][i]);
          r += std::fabs(a[i][j]);
        }
      if (c != 0.0 && r != 0.0) {
        double g = r / radix, f = 1.0;
        const double s = c + r;
        while (c < g) {
          f *= radix;
          c *= sqrdx;
        }
        g = r * radix;
        while (c > g) {
          f /= radix;
          c /= sqrdx;
        }
        if ((c + r) / f < 0.95 * s) {
          done = false;
          g = 1.0 / f;
          for (size_t j = 1; j <= n; ++j) a[i][j] *= g;
          for (size_t j = 1; j <= n; ++j) a[j][i] *= f;
        }
      }
    }
  }
}

void hessenberg(size_t n, Mat& a) {
  for (size_t m = 2; m < n; ++m) {
    double x = 0.0;
    size_t i = m;
    for (size_t j = m; j <= n; ++j)
      if (std::fabs(a[j][m - 1]) > std::fabs(x)) {
        x = a[j][m - 1];
        i = j;
      }
    if (i != m) {
      for (size_t j = m - 1; j <= n; ++j) std::swap(a[i][j], a[m][j]);
      for (size_t j = 1; j <= n; ++j) std::swap(a[j][i], a[j][m]);
    }
    if (x != 0.0) {
      for (i = m + 1; i <= n; ++i) {
        double y = a[i][m - 1];
        if (y != 0.0) {
          y /= x;
          a[i][m - 1] = y;
          for (size_t j = m; j <= n; ++j) a[i][j] -= y * a[m][j];
          for (size_t j = 1; j <= n; ++j) a[j][m] += y * a[j][i];
        }
      }
    }
  }
}

bool hqr(size_t n, Mat& a, std::vector<double>& wr, std::vector<double>& wi) {
  double anorm = 0.0;
  for (size_t i = 1; i <= n; ++i)
    for (size_t j = std::max<size_t>(i - 1, 1); j <= n; ++j) anorm += std::fabs(a[i][j]);
  long nn = static_cast<long>(n);
  double t = 0.0, p = 0.0, q = 0.0, r = 0.0, s = 0.0, w = 0.0, x = 0.0, y = 0.0, z = 0.0;
  while (nn >= 1) {
    int its = 0;
    long l = 0;
    do {
      for (l = nn; l >= 2; --l) {
        s = std::fabs(a[l - 1][l - 1]) + std::fabs(a[l][l]);
        if (s == 0.0) s = anorm;
        if (std::fabs(a[l][l - 1]) + s == s) {
          a[l][l - 1] = 0.0;
          break;
        }
      }
      x = a[nn][nn];
      if (l == nn) {
        wr[nn] = x + t;
        wi[nn--] = 0.0;
      } else {
        y = a[nn - 1][nn - 1];
        w = a[nn][nn - 1] * a[nn - 1][nn];
        if (l == nn - 1) {
          p = 0.5 * (y - x);
          q = p * p + w;
          z = std::sqrt(std::fabs(q));
          x += t;
          if (q >= 0.0) {
            z = p + signOf(z, p);
            wr[nn - 1] = wr[nn] = x + z;
            if (z != 0.0) wr[nn] = x - w / z;
            wi[nn - 1] = wi[nn] = 0.0;
          } else {
            wr[nn - 1] = wr[nn] = x + p;
            wi[nn - 1] = -(wi[nn] = z);
          }
          nn -= 2;
        } else {
          if (its == 60) return false;
          if (its == 10 || its == 20) {
            t += x;
            for (long i = 1; i <= nn; ++i) a[i][i] -= x;
            s = std::fabs(a[nn][nn - 1]) + std::fabs(a[nn - 1][nn - 2]);
            y = x = 0.75 * s;
            w = -0.4375 * s * s;
          }
          ++its;
          long m = nn - 2;
          for (; m >= l; --m) {
            z = a[m][m];
            r = x - z;
            s = y - z;
            p = (r * s - w) / a[m + 1][m] + a[m][m + 1];
            q = a[m + 1][m + 1] - z - r - s;
            r = a[m + 2][m + 1];
            s = std::fabs(p) + std::fabs(q) + std::fabs(r);
            p /= s;
            q /= s;
            r /= s;
            if (m == l) break;
            const double u = std::fabs(a[m][m - 1]) * (std::fabs(q) + std::fabs(r));
            const double v = std::fabs(p) * (std::fabs(a[m - 1][m - 1]) + std::fabs(z) +
                                             std::fabs(a[m + 1][m + 1]));
            if (u + v == v) break;
          }
          for (long i = m + 2; i <= nn; ++i) {
            a[i][i - 2] = 0.0;
            if (i != m + 2) a[i][i - 3] = 0.0;
          }
          for (long k = m; k <= nn - 1; ++k) {
            if (k != m) {
              p = a[k][k - 1];
              q = a[k + 1][k - 1];
              r = 0.0;
              if (k != nn - 1) r = a[k + 2][k - 1];
              if ((x = std::fabs(p) + std::fabs(q) + std::fabs(r)) != 0.0) {
                p /= x;
                q /= x;
                r /= x;
              }
            }
            if ((s = signOf(std::sqrt(p * p + q * q + r * r), p)) != 0.0) {
              if (k == m) {
                if (l != m) a[k][k - 1] = -a[k][k - 1];
              } else {
                a[k][k - 1] = -s * x;
              }
              p += s;
              x = p / s;
              y = q / s;
              z = r / s;
              q /= p;
              r /= p;
              for (long j = k; j <= nn; ++j) {
                p = a[k][j] + q * a[k + 1][j];
                if (k != nn - 1) {
                  p += r * a[k + 2][j];
                  a[k + 2][j] -= p * z;
                }
                a[k + 1][j] -= p * y;
                a[k][j] -= p * x;
              }
              const long mmin = nn < k + 3 ? nn : k + 3;
              for (long i = l; i <= mmin; ++i) {
                p = x * a[i][k] + y * a[i][k + 1];
                if (k != nn - 1) {
                  p += z * a[i][k + 2];
                  a[i][k + 2] -= p * r;
                }
                a[i][k + 1] -= p * q;
                a[i][k] -= p;
              }
            }
          }
        }
      }
    } while (l < nn - 1);
  }
  return true;
}

Mat oneBased(const eval::EvalMatrix& m) {
  const size_t r = m.data.size(), c = r ? m.data[0].size() : 0;
  Mat a(r + 1, std::vector<double>(std::max(r, c) + 1, 0.0));
  for (size_t i = 0; i < r; ++i)
    for (size_t j = 0; j < c; ++j) a[i + 1][j + 1] = m.data[i][j];
  return a;
}

// `matrix symeigen X v = A` ([P] matrix symeigen): the eigenvectors of the
// symmetric A in the columns of X (rows named as A's rows, columns e1..en) and
// the eigenvalues in v (1 x n, row r1), largest first. Each eigenvector is
// signed so that its elements sum to a nonnegative number: that reproduces
// every vector the vs-stata bank and the manual's example record (0968fb9c,
// ed2c6a3b, 4513d2da; [P] matrix symeigen example 1). A matrix that is not
// symmetric is r(505) (2222e846, d3c53f3c), one with missing values r(504).
ojson runSymeigen(const std::string& args, SessionState& session) {
  DecompArgs da;
  if (auto bad = parseDecomp(args, 2, session, da)) return *bad;
  const size_t n = da.src.data.size();
  if (n == 0 || da.src.data[0].size() != n) return engineErrorBlockRc("matrix not square", 505);
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < i; ++j)
      if (da.src.data[i][j] != da.src.data[j][i])
        return engineErrorBlockRc("matrix not symmetric", 505);
  Mat z = oneBased(da.src);
  std::vector<double> d(n + 1, 0.0), e(n + 1, 0.0);
  tred2(n, z, d, e);
  if (!tql2(n, z, d, e)) return engineErrorBlockRc("matrix symeigen: no convergence", 430);
  eval::EvalMatrix X, v;
  X.data.assign(n, std::vector<double>(n, 0.0));
  v.data.assign(1, std::vector<double>(n, 0.0));
  for (size_t c = 0; c < n; ++c) {
    const size_t src = n - c;  // tql2 leaves them ascending
    v.data[0][c] = d[src];
    double sum = 0.0;
    for (size_t r = 0; r < n; ++r) sum += z[r + 1][src];
    const double sgn = sum < 0.0 ? -1.0 : 1.0;
    for (size_t r = 0; r < n; ++r) X.data[r][c] = sgn * z[r + 1][src] + 0.0;
  }
  X.rownames = da.src.rownames;
  X.colnames = seqNames("e", n);
  v.rownames = {"r1"};
  v.colnames = seqNames("e", n);
  storeMatrixResult(session, da.outs[0], std::move(X));
  storeMatrixResult(session, da.outs[1], std::move(v));
  return ojson::array();
}

// `matrix eigenvalues r c = A` ([P] matrix eigenvalues): the real and the
// imaginary parts of the eigenvalues of the square A, ordered by modulus,
// largest first (a conjugate pair with its positive imaginary part first, as
// the manual's example lists it); rows `real` and `complex`. One result name
// is r(198) (vs-stata bank 5801a1ae).
ojson runEigenvalues(const std::string& args, SessionState& session) {
  DecompArgs da;
  if (auto bad = parseDecomp(args, 2, session, da)) return *bad;
  const size_t n = da.src.data.size();
  if (n == 0 || da.src.data[0].size() != n) return engineErrorBlockRc("matrix not square", 505);
  Mat a = oneBased(da.src);
  balance(n, a);
  hessenberg(n, a);
  std::vector<double> wr(n + 1, 0.0), wi(n + 1, 0.0);
  if (!hqr(n, a, wr, wi)) return engineErrorBlockRc("matrix eigenvalues: no convergence", 430);
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = i + 1;
  std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
    const double mx = std::hypot(wr[x], wi[x]), my = std::hypot(wr[y], wi[y]);
    if (mx != my) return mx > my;
    return wi[x] > wi[y];
  });
  eval::EvalMatrix re, im;
  re.data.assign(1, std::vector<double>(n, 0.0));
  im.data.assign(1, std::vector<double>(n, 0.0));
  for (size_t c = 0; c < n; ++c) {
    re.data[0][c] = wr[order[c]];
    im.data[0][c] = wi[order[c]] + 0.0;
  }
  re.rownames = {"real"};
  im.rownames = {"complex"};
  re.colnames = seqNames("c", n);
  im.colnames = seqNames("c", n);
  storeMatrixResult(session, da.outs[0], std::move(re));
  storeMatrixResult(session, da.outs[1], std::move(im));
  return ojson::array();
}

// `matrix svd U w V = A` ([P] matrix svd), A m x n with m >= n: A = U
// diag(w) V'. w keeps the order the Golub-Reinsch iteration leaves (Stata's
// w is not sorted: vs-stata bank f595c9ff lists 0.51430 before 9.52552, the
// manual's example 21.83, 2.6e-16, 5.60), and each column of U, with the
// matching column of V, is signed so that the column of U sums to a
// nonnegative number (bank 457d73eb's U, the manual's U and V). U is named by
// A's rows and columns, w by r1 and A's columns, V by A's columns twice.
ojson runSvd(const std::string& args, SessionState& session) {
  DecompArgs da;
  if (auto bad = parseDecomp(args, 3, session, da)) return *bad;
  const size_t m = da.src.data.size();
  const size_t n = m ? da.src.data[0].size() : 0;
  if (n == 0 || m < n) return engineErrorBlockRc("conformability error", 503);
  Mat a(m + 1, std::vector<double>(n + 1, 0.0));
  for (size_t i = 0; i < m; ++i)
    for (size_t j = 0; j < n; ++j) a[i + 1][j + 1] = da.src.data[i][j];
  std::vector<double> w(n + 1, 0.0);
  Mat v(n + 1, std::vector<double>(n + 1, 0.0));
  if (!svdGolubReinsch(m, n, a, w, v)) return engineErrorBlockRc("matrix svd: no convergence", 430);
  eval::EvalMatrix U, W, V;
  U.data.assign(m, std::vector<double>(n, 0.0));
  W.data.assign(1, std::vector<double>(n, 0.0));
  V.data.assign(n, std::vector<double>(n, 0.0));
  for (size_t j = 1; j <= n; ++j) {
    double sum = 0.0;
    for (size_t i = 1; i <= m; ++i) sum += a[i][j];
    const double sgn = sum < 0.0 ? -1.0 : 1.0;
    for (size_t i = 1; i <= m; ++i) U.data[i - 1][j - 1] = sgn * a[i][j] + 0.0;
    for (size_t i = 1; i <= n; ++i) V.data[i - 1][j - 1] = sgn * v[i][j] + 0.0;
    W.data[0][j - 1] = w[j];
  }
  U.rownames = da.src.rownames;
  U.colnames = da.src.colnames;
  W.rownames = {"r1"};
  W.colnames = da.src.colnames;
  V.rownames = da.src.colnames;
  V.colnames = da.src.colnames;
  storeMatrixResult(session, da.outs[0], std::move(U));
  storeMatrixResult(session, da.outs[1], std::move(W));
  storeMatrixResult(session, da.outs[2], std::move(V));
  return ojson::array();
}

// `matrix dir` ([P] matrix utility): one line per matrix in memory, the most
// recently created first, `NAME[r,c]` with the name right-aligned in 13
// columns (vs-stata bank 2a6d9f14, 57082f3e, d631eaa9: `            B[2,2]`).
// No matrix prints nothing (9cb03d26).
ojson runMatrixDir(const std::string& args, SessionState& session) {
  if (!words(args).empty()) return engineErrorBlockRc("invalid syntax", 198);
  std::string text;
  const auto& mats = session.eval.matrices;
  for (auto it = mats.rbegin(); it != mats.rend(); ++it) {
    const size_t r = it->second.data.size();
    const size_t c = r ? it->second.data[0].size() : 0;
    std::string name = it->first;
    if (name.size() < 13) name = std::string(13 - name.size(), ' ') + name;
    if (!text.empty()) text += "\n";
    text += name + "[" + std::to_string(r) + "," + std::to_string(c) + "]";
  }
  if (text.empty()) return ojson::array();
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

// `matrix rename old new` ([P] matrix utility): the matrix keeps its place in
// `matrix dir` (the manual's example lists the renamed z where a stood). An
// absent old is r(111) (vs-stata bank 0aee4581); a new name that is already a
// matrix is refused r(110) rather than silently replaced.
ojson runMatrixRename(const std::string& args, SessionState& session) {
  const std::vector<std::string> w = words(args);
  if (w.size() != 2) return engineErrorBlockRc("invalid syntax", 198);
  eval::EvalMatrix* m = userMatrix(session, w[0]);
  if (m == nullptr) return engineErrorBlockRc("matrix " + w[0] + " not found", 111);
  if (!strutil::isValidStataIdentifier(w[1])) return engineErrorBlockRc(w[1] + " invalid name", 198);
  if (w[0] == w[1]) return ojson::array();
  if (userMatrix(session, w[1]) != nullptr)
    return engineErrorBlockRc("matrix " + w[1] + " already exists", 110);
  session.eval.scalars.erase(w[1]);
  session.eval.stringScalars.erase(w[1]);
  for (auto& p : session.eval.matrices)
    if (p.first == w[0]) {
      p.first = w[1];
      break;
    }
  return ojson::array();
}


// ---------------------------------------------------------------------------
// The data-reading subcommands: score, vecaccum, opaccum, glsaccum.

bool optAbbrev(const std::string& key, const char* full, size_t shortest) {
  const std::string f(full);
  return key.size() >= shortest && key.size() <= f.size() && f.compare(0, key.size(), key) == 0;
}

// The rows a cross-product subcommand reads: the if/in selection, then every
// observation where a listed variable (and the weight) is not missing. A
// string variable is missing in every observation, so no row is left.
struct RowSet {
  std::vector<std::vector<double>> x;  // one row per kept observation, in `vars` order
  std::vector<double> w;
  std::vector<int64_t> obs;            // the kept rows' positions in the selected frame
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = nullptr;
};

std::optional<ojson> selectRows(const eval::ParsedCommand& parsed, const std::vector<std::string>& vars,
                                const std::string& wVar, const std::string& wType,
                                SessionState& session, const std::string& label, RowSet& out) {
  const std::optional<std::string> headPath = session.currentHbin();
  if (!headPath.has_value() || session.datastore() == nullptr)
    return engineErrorBlockRc("no variables defined", 111);
  out.ds = session.datastore();
  if (parsed.ifExpr.has_value() || parsed.inRange.has_value()) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *headPath, parsed.ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), label);
    out.filtered = std::make_unique<Datastore>(fr.dataPath);
    out.ds = out.filtered.get();
  }
  std::vector<const double*> cols;
  for (const auto& v : vars) {
    if (!out.ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
    cols.push_back(out.ds->dtype(v) == "f64" ? out.ds->col(v).f64() : nullptr);
  }
  const double* wcol = nullptr;
  if (!wVar.empty()) {
    if (!out.ds->has(wVar)) return engineErrorBlockRc("variable " + wVar + " not found", 111);
    if (out.ds->dtype(wVar) != "f64") return engineErrorBlockRc("type mismatch", 109);
    wcol = out.ds->col(wVar).f64();
  }
  const int64_t n = out.ds->nRows();
  for (int64_t r = 0; r < n; ++r) {
    bool keep = true;
    for (const double* c : cols)
      if (c == nullptr || cecon::is_missing(c[r])) {
        keep = false;
        break;
      }
    double w = 1.0;
    if (keep && wcol != nullptr) {
      w = wcol[r];
      keep = !cecon::is_missing(w) && w > 0.0;
      if (keep && wType == "fw" && w != std::floor(w))
        return engineErrorBlockRc("may not use noninteger frequency weights", 401);
    }
    if (!keep) continue;
    std::vector<double> row(cols.size());
    for (size_t c = 0; c < cols.size(); ++c) row[c] = cols[c][r];
    out.x.push_back(std::move(row));
    out.w.push_back(w);
    out.obs.push_back(r);
  }
  if (out.x.empty()) return engineErrorBlockRc("no observations", 2000);
  return std::nullopt;
}

// `matrix NAME = varlist ...` tokenized as Stata reads it; the variables
// resolve as every varlist does (abbreviations, wildcards, ranges).
std::optional<ojson> parseAccumLine(const std::string& sub, const std::string& args,
                                    SessionState& session, eval::ParsedCommand& parsed,
                                    std::string& name, std::vector<std::string>& vars) {
  try {
    parsed = eval::tokenize("matrix " + sub + " " + args);
  } catch (const eval::TokenizerError& te) {
    return ojson(te.block);
  }
  const auto eq = std::find(parsed.varlist.begin(), parsed.varlist.end(), "=");
  if (eq == parsed.varlist.end() || eq == parsed.varlist.begin() + 1 ||
      eq - parsed.varlist.begin() != 2)
    return engineErrorBlockRc("invalid syntax", 198);
  name = parsed.varlist[1];
  if (!strutil::isValidStataIdentifier(name)) return engineErrorBlockRc(name + " invalid name", 198);
  vars.assign(eq + 1, parsed.varlist.end());
  if (vars.empty()) return engineErrorBlockRc("varlist required", 100);
  for (const auto& v : vars)
    if (v.find_first_of(".#") != std::string::npos)
      return engineErrorBlockRc("matrix " + sub + ": factor-variable and time-series operators (" +
                                    v + ") are not supported in this version of PIET",
                                198);
  if (!session.hasData()) return engineErrorBlockRc("no variables defined", 111);
  if (auto bad = resolveByKeyNames(session, vars)) return *bad;
  return std::nullopt;
}

void postN(SessionState& session, const char* cmd, double n, std::optional<double> sumW) {
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", n);
  if (sumW.has_value()) {
    session.eval.rValues.set("sum_w", *sumW);
    eval::declareRPosted(session.eval, cmd, {"N", "sum_w"});
  } else {
    eval::declareRPosted(session.eval, cmd, {"N"});
  }
}

// `matrix vecaccum a = y x1 x2 ... [if] [in] [fw] [, noconstant]` ([P] matrix
// accum): the row vector a = y'X, X the other variables and a constant column
// last (dropped by noconstant), over the observations where none is missing;
// row y, columns the x's and _cons. r(N) is the observations (an fweight's
// sum), r(sum_w) the weights' sum (vs-stata bank 3ff4a0ea, 737d71b4, 51526fcd,
// c8f1e330). Other weight types are not implemented here and are refused.
ojson runVecaccum(const std::string& args, SessionState& session) {
  eval::ParsedCommand parsed;
  std::string name;
  std::vector<std::string> vars;
  if (auto bad = parseAccumLine("vecaccum", args, session, parsed, name, vars)) return *bad;
  bool noConstant = false;
  for (const auto& [key, val] : parsed.opts) {
    if (optAbbrev(key, "noconstant", 3) && val.isBool) noConstant = true;
    else return engineErrorBlockRc("option " + key + " not allowed", 198);
  }
  std::string wType;
  if (parsed.weightType.has_value()) {
    wType = parsed.weightType->substr(0, 2);
    for (auto& ch : wType) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (wType != "fw")
      return ojson(eval::unsupportedOption("[" + *parsed.weightType + "=" +
                                               parsed.weightVar.value_or("") + "]",
                                           "", std::nullopt, std::string("matrix vecaccum"),
                                           std::string("r(198)")));
    if (!strutil::isValidStataIdentifier(parsed.weightVar.value_or("")))
      return ojson(eval::unsupportedOption("[fw=" + parsed.weightVar.value_or("") + "]", "",
                                           std::nullopt, std::string("matrix vecaccum"),
                                           std::string("r(198)")));
  }
  std::string wVar = parsed.weightVar.value_or(std::string());
  if (!wVar.empty()) {
    std::vector<std::string> wv{wVar};
    if (auto bad = resolveByKeyNames(session, wv)) return *bad;
    wVar = wv[0];
  }
  RowSet rs;
  if (auto bad = selectRows(parsed, vars, wVar, wType, session, "matrix vecaccum", rs)) return *bad;
  const size_t k = vars.size() - 1;
  eval::EvalMatrix a;
  a.data.assign(1, std::vector<double>(noConstant ? k : k + 1, 0.0));
  double sumW = 0.0;
  for (size_t r = 0; r < rs.x.size(); ++r) {
    const double yw = rs.x[r][0] * rs.w[r];
    for (size_t j = 0; j < k; ++j) a.data[0][j] += yw * rs.x[r][j + 1];
    if (!noConstant) a.data[0][k] += yw;
    sumW += rs.w[r];
  }
  a.rownames = {vars[0]};
  a.colnames.assign(vars.begin() + 1, vars.end());
  if (!noConstant) a.colnames.push_back("_cons");
  storeMatrixResult(session, name, std::move(a));
  postN(session, "matrix vecaccum", wType.empty() ? static_cast<double>(rs.x.size()) : sumW, sumW);
  return ojson::array();
}

// Stata's sort marker: the frame's claimed sort order must lead with the
// group variable ([P] matrix accum: "The data must be sorted by groupvar"); a
// frame that merely happens to be in order is "not sorted", r(5) (vs-stata
// bank 8d5d9ec6, 803f383a: `generate g = ceil(_n/2)` with no `sort g`).
bool sortedByGroup(const Datastore& ds, const std::string& g) {
  std::istringstream in(ds.sortedBy());
  std::string first;
  in >> first;
  return first == g;
}

struct GroupOpts {
  std::string group, opvar, row, glsmat;
  bool noConstant = false;
};

std::optional<ojson> parseGroupOpts(const eval::ParsedCommand& parsed, bool gls, GroupOpts& o) {
  for (const auto& [key, val] : parsed.opts) {
    if (optAbbrev(key, "noconstant", 3) && val.isBool) o.noConstant = true;
    else if (optAbbrev(key, "group", 5) && !val.isBool) o.group = strutil::trim(val.strValue);
    else if (!gls && optAbbrev(key, "opvar", 5) && !val.isBool) o.opvar = strutil::trim(val.strValue);
    else if (gls && optAbbrev(key, "row", 3) && !val.isBool) o.row = strutil::trim(val.strValue);
    else if (gls && optAbbrev(key, "glsmat", 6) && !val.isBool) o.glsmat = strutil::trim(val.strValue);
    else return engineErrorBlockRc("option " + key + " not allowed", 198);
  }
  if (o.group.empty()) return engineErrorBlockRc("option group() required", 100);
  if (!gls && o.opvar.empty()) return engineErrorBlockRc("option opvar() required", 100);
  if (gls && o.row.empty()) return engineErrorBlockRc("option row() required", 100);
  if (gls && o.glsmat.empty()) return engineErrorBlockRc("option glsmat() required", 100);
  return std::nullopt;
}

eval::EvalMatrix crossNamed(size_t dim, const std::vector<std::string>& vars, bool noConstant) {
  eval::EvalMatrix m;
  m.data.assign(dim, std::vector<double>(dim, 0.0));
  m.rownames = vars;
  if (!noConstant) m.rownames.push_back("_cons");
  m.colnames = m.rownames;
  return m;
}

// `matrix opaccum A = varlist [if] [in], group(g) opvar(e) [noconstant]` and
// `matrix glsaccum A = varlist [if] [in], group(g) glsmat(W) row(r)
// [noconstant]` ([P] matrix accum): A = sum over the groups k of X_k' W_k
// X_k, X_k the group's rows of the variables and a constant, W_k = e_k e_k'
// for opaccum and, for glsaccum, the rows and columns of W named by the
// group's row() values. The groups are the runs of g in the data, which must
// carry Stata's sort marker for g. r(N) is the observations used (bank
// ab22dccf, 13c8c3f8, ecd49505, 1b05e2d7: 74). A glsmat() naming a string
// variable (one matrix per group) and weights are not implemented here.
ojson runGroupAccum(bool gls, const std::string& args, SessionState& session) {
  const std::string sub = gls ? "glsaccum" : "opaccum";
  eval::ParsedCommand parsed;
  std::string name;
  std::vector<std::string> vars;
  if (auto bad = parseAccumLine(sub, args, session, parsed, name, vars)) return *bad;
  if (parsed.weightType.has_value())
    return ojson(eval::unsupportedOption("[" + *parsed.weightType + "=" +
                                             parsed.weightVar.value_or("") + "]",
                                         "", std::nullopt, "matrix " + sub, std::string("r(198)")));
  GroupOpts o;
  if (auto bad = parseGroupOpts(parsed, gls, o)) return *bad;
  std::vector<std::string> extra{o.group, gls ? o.row : o.opvar};
  if (auto bad = resolveByKeyNames(session, extra)) return *bad;
  o.group = extra[0];
  const std::string second = extra[1];
  const eval::EvalMatrix* W = nullptr;
  eval::EvalMatrix wCopy;
  if (gls) {
    W = session.eval.findMatrix(o.glsmat);
    if (W == nullptr) {
      Datastore* hds = session.datastore();
      if (hds != nullptr && hds->has(o.glsmat))
        return ojson(eval::unsupportedOption("glsmat(" + o.glsmat + ")", "", std::nullopt,
                                             std::string("matrix glsaccum"), std::string("r(198)")));
      return engineErrorBlockRc("matrix " + o.glsmat + " not found", 111);
    }
    wCopy = *W;
  }
  {
    Datastore* hds = session.datastore();
    if (hds == nullptr || !hds->has(o.group))
      return engineErrorBlockRc("variable " + o.group + " not found", 111);
    if (!sortedByGroup(*hds, o.group)) return engineErrorBlockRc("not sorted", 5);
  }
  std::vector<std::string> all = vars;
  all.push_back(second);
  RowSet rs;
  if (auto bad = selectRows(parsed, all, "", "", session, "matrix " + sub, rs)) return *bad;
  if (!rs.ds->has(o.group)) return engineErrorBlockRc("variable " + o.group + " not found", 111);
  if (rs.ds->dtype(o.group) != "f64")
    return ojson(eval::unsupportedOption("group(" + o.group + ")", "", std::nullopt,
                                         "matrix " + sub, std::string("r(198)")));
  const double* gcol = rs.ds->col(o.group).f64();
  const size_t k = vars.size();
  const size_t dim = o.noConstant ? k : k + 1;
  eval::EvalMatrix A = crossNamed(dim, vars, o.noConstant);
  const size_t wn = wCopy.data.size();
  size_t start = 0;
  const auto xval = [&](size_t r, size_t j) { return j < k ? rs.x[r][j] : 1.0; };
  while (start < rs.x.size()) {
    size_t end = start + 1;
    const double gv = gcol[rs.obs[start]];
    while (end < rs.x.size()) {
      const double hv = gcol[rs.obs[end]];
      if (!(hv == gv || (std::isnan(hv) && std::isnan(gv)))) break;
      ++end;
    }
    if (!gls) {
      std::vector<double> v(dim, 0.0);
      for (size_t r = start; r < end; ++r)
        for (size_t j = 0; j < dim; ++j) v[j] += rs.x[r][k] * xval(r, j);
      for (size_t i = 0; i < dim; ++i)
        for (size_t j = 0; j < dim; ++j) A.data[i][j] += v[i] * v[j];
    } else {
      for (size_t r = start; r < end; ++r) {
        const double ri = rs.x[r][k];
        if (ri != std::floor(ri) || ri < 1.0 || ri > static_cast<double>(wn) ||
            wCopy.data[0].size() != wn)
          return engineErrorBlockRc("conformability error", 503);
        for (size_t q = start; q < end; ++q) {
          const double rq = rs.x[q][k];
          if (rq != std::floor(rq) || rq < 1.0 || rq > static_cast<double>(wn))
            return engineErrorBlockRc("conformability error", 503);
          const double wij =
              wCopy.data[static_cast<size_t>(ri) - 1][static_cast<size_t>(rq) - 1];
          if (wij == 0.0) continue;
          for (size_t i = 0; i < dim; ++i) {
            const double xi = xval(r, i) * wij;
            for (size_t j = 0; j < dim; ++j) A.data[i][j] += xi * xval(q, j);
          }
        }
      }
    }
    start = end;
  }
  storeMatrixResult(session, name, std::move(A));
  postN(session, ("matrix " + sub).c_str(), static_cast<double>(rs.x.size()), std::nullopt);
  return ojson::array();
}

std::string numLiteral(double v) {
  if (std::isnan(v) || v >= eval::kStataMissingSentinel) return "(.)";
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string("(") + buf + ")";
}

// `matrix score [type] newvar = b [if] [in] [, equation(eqspec) missval(#)
// replace forcezero]` ([P] matrix score): newvar = sum_j b_j x_j over the
// columns of the row vector b (rows of a column vector), x_j the variable the
// column names and _cons the constant 1, summed in column order. Only the
// columns of one equation are read: eq(#1) (the first equation the vector
// lists) unless equation() names another. A missing x_j makes the score missing
// unless missval(#) stands in for it (vs-stata bank ba965174: missval(-1) over
// rep78); forcezero reads an absent variable as 0, which is otherwise r(111).
// replace writes into an existing variable inside the if/in selection and
// leaves the rest (and creates it when absent: bank 63176820); a storage type
// with replace is r(101) (8b9fc1ea); an existing newvar without it r(110).
// The score is a generate/replace of that sum, so the storage type is the
// default type or the one given.
ojson runMatrixScore(const std::string& args, SessionState& session) {
  std::string head = args, opts;
  {
    int depth = 0;
    bool inQuote = false;
    for (size_t i = 0; i < args.size(); ++i) {
      const char c = args[i];
      if (c == '"') inQuote = !inQuote;
      if (inQuote) continue;
      if (c == '(') ++depth;
      else if (c == ')') --depth;
      else if (c == ',' && depth == 0) {
        head = args.substr(0, i);
        opts = args.substr(i + 1);
        break;
      }
    }
  }
  const size_t eqPos = head.find('=');
  if (eqPos == std::string::npos) return engineErrorBlockRc("invalid syntax", 198);
  std::vector<std::string> lhs = words(head.substr(0, eqPos));
  std::string type;
  if (lhs.size() == 2) {
    static const std::vector<std::string> kTypes = {"byte", "int", "long", "float", "double"};
    if (std::find(kTypes.begin(), kTypes.end(), lhs[0]) == kTypes.end())
      return engineErrorBlockRc("invalid syntax", 198);
    type = lhs[0];
    lhs.erase(lhs.begin());
  }
  if (lhs.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
  const std::string newvar = lhs[0];
  if (!strutil::isValidStataIdentifier(newvar)) return engineErrorBlockRc(newvar + " invalid name", 198);
  std::string rest = strutil::trim(head.substr(eqPos + 1));
  const size_t sp = rest.find_first_of(" \t");
  const std::string bname = rest.substr(0, sp);
  const std::string qualifiers = sp == std::string::npos ? std::string() : strutil::trim(rest.substr(sp));
  if (!qualifiers.empty() && !(qualifiers.rfind("if ", 0) == 0 || qualifiers.rfind("in ", 0) == 0))
    return engineErrorBlockRc("invalid syntax", 198);
  bool replace = false, forcezero = false;
  std::string eqSpec, missval;
  {
    eval::ParsedCommand po;
    try {
      po = eval::tokenize("matrix , " + opts);
    } catch (const eval::TokenizerError& te) {
      return ojson(te.block);
    }
    for (const auto& [key, val] : po.opts) {
      if (key == "replace" && val.isBool) replace = true;
      else if (optAbbrev(key, "forcezero", 9) && val.isBool) forcezero = true;
      else if (optAbbrev(key, "equation", 2) && !val.isBool) eqSpec = strutil::trim(val.strValue);
      else if (optAbbrev(key, "missval", 7) && !val.isBool) missval = strutil::trim(val.strValue);
      else return engineErrorBlockRc("option " + key + " not allowed", 198);
    }
  }
  if (replace && !type.empty())
    return engineErrorBlockRc("may not set storage type of variable when using replace option", 101);
  const eval::EvalMatrix* bp = session.eval.findMatrix(bname);
  if (bp == nullptr) return engineErrorBlockRc("matrix " + bname + " not found", 111);
  const eval::EvalMatrix b = *bp;
  const size_t nr = b.data.size(), nc = nr ? b.data[0].size() : 0;
  if (nr != 1 && nc != 1) return engineErrorBlockRc("conformability error", 503);
  std::vector<double> coef;
  std::vector<std::string> names;
  if (nr == 1) {
    coef = b.data[0];
    names = b.colnames;
  } else {
    for (const auto& r : b.data) coef.push_back(r[0]);
    names = b.rownames;
  }
  names.resize(coef.size());
  // The equation each column belongs to, in order of first appearance.
  std::vector<std::string> eqOf(coef.size()), eqOrder;
  for (size_t j = 0; j < coef.size(); ++j) {
    const size_t c = names[j].find(':');
    eqOf[j] = c == std::string::npos ? std::string() : names[j].substr(0, c);
    if (std::find(eqOrder.begin(), eqOrder.end(), eqOf[j]) == eqOrder.end()) eqOrder.push_back(eqOf[j]);
  }
  std::string useEq = eqOrder.empty() ? std::string() : eqOrder[0];
  if (!eqSpec.empty()) {
    if (eqSpec[0] == '#') {
      const std::string num = eqSpec.substr(1);
      char* endp = nullptr;
      const long k = std::strtol(num.c_str(), &endp, 10);
      if (num.empty() || *endp != '\0' || k < 1 || static_cast<size_t>(k) > eqOrder.size())
        return engineErrorBlockRc("equation " + eqSpec + " not found", 303);
      useEq = eqOrder[static_cast<size_t>(k) - 1];
    } else {
      if (std::find(eqOrder.begin(), eqOrder.end(), eqSpec) == eqOrder.end())
        return engineErrorBlockRc("equation " + eqSpec + " not found", 303);
      useEq = eqSpec;
    }
  }
  if (!session.hasData() || session.datastore() == nullptr)
    return engineErrorBlockRc("no variables defined", 111);
  Datastore* ds = session.datastore();
  const bool exists = ds->has(newvar);
  if (exists && !replace) return engineErrorBlockRc("variable " + newvar + " already defined", 110);
  std::string mv;
  if (!missval.empty()) {
    char* endp = nullptr;
    const double m = std::strtod(missval.c_str(), &endp);
    if (endp == missval.c_str() || *endp != '\0') return engineErrorBlockRc("invalid syntax", 198);
    mv = numLiteral(m);
  }
  std::string expr;
  for (size_t j = 0; j < coef.size(); ++j) {
    if (eqOf[j] != useEq) continue;
    const size_t c = names[j].find(':');
    const std::string var = c == std::string::npos ? names[j] : names[j].substr(c + 1);
    std::string term;
    if (var == "_cons") {
      term = numLiteral(coef[j]);
    } else {
      if (var.find_first_of(".#") != std::string::npos)
        return engineErrorBlockRc("matrix score: factor-variable and time-series operators (" + var +
                                      ") are not supported in this version of PIET",
                                  198);
      if (!ds->has(var) || ds->dtype(var) != "f64") {
        if (!ds->has(var) && forcezero) continue;
        if (!ds->has(var)) return engineErrorBlockRc("variable " + var + " not found", 111);
        return engineErrorBlockRc("type mismatch", 109);
      }
      const std::string x = mv.empty() ? var : "cond(missing(" + var + ")," + mv + "," + var + ")";
      term = numLiteral(coef[j]) + "*" + x;
    }
    expr += expr.empty() ? term : " + " + term;
  }
  if (expr.empty()) expr = "0";
  const std::string sel = qualifiers.empty() ? std::string() : " " + qualifiers;
  const std::string cmd = exists ? "quietly replace " + newvar + " = " + expr + sel
                                 : "quietly generate " + (type.empty() ? std::string() : type + " ") +
                                       newvar + " = " + expr + sel;
  ojson blocks = dispatch(cmd, session);
  for (const auto& blk : blocks)
    if (blk.is_object() && blk.value("type", std::string()) == "error") return blk;
  return ojson::array();
}

// ---------------------------------------------------------------------------
// matrix rowjoinbyname / coljoinbyname ([P] matrix rowjoinbyname)

// One operand of a join: a stored matrix, or e(name) / r(name).
std::optional<ojson> joinOperand(const std::string& tok, SessionState& session,
                                 eval::EvalMatrix& out) {
  static const std::regex reClass(R"(^([er])\(\s*([A-Za-z_]\w*)\s*\)$)");
  std::smatch m;
  if (std::regex_match(tok, m, reClass)) {
    std::string err;
    int rc = 111;
    std::optional<eval::EvalMatrix> r =
        resolveClassMatrixRef(m.str(1)[0], m.str(2), session, err, &rc);
    if (!r.has_value()) return engineErrorBlockRc("matrix " + tok + " not found", rc ? rc : 111);
    out = std::move(*r);
    return std::nullopt;
  }
  const eval::EvalMatrix* a = session.eval.findMatrix(tok);
  if (a == nullptr) return engineErrorBlockRc("matrix " + tok + " not found", 111);
  out = *a;
  return std::nullopt;
}

// `eq:name` split into its equation ("_" for none) and name.
std::pair<std::string, std::string> stripeParts(const std::string& full) {
  const size_t colon = full.find(':');
  if (colon == std::string::npos) return {"_", full};
  std::string eq = full.substr(0, colon);
  if (eq.empty()) eq = "_";
  return {eq, full.substr(colon + 1)};
}

// Split `text` at the first comma outside parentheses: the operands and the
// options.
std::pair<std::string, std::string> splitOptions(const std::string& text) {
  int depth = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    else if (text[i] == ')') --depth;
    else if (text[i] == ',' && depth == 0) return {text.substr(0, i), text.substr(i + 1)};
  }
  return {text, std::string()};
}

// `name` or `name(arg)` items of an option list.
std::optional<ojson> optionItems(const std::string& text,
                                 std::vector<std::pair<std::string, std::string>>& out,
                                 std::vector<bool>& hasArg) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) break;
    size_t j = i;
    while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != '(') ++j;
    std::string key = text.substr(i, j - i);
    if (j < text.size() && text[j] == '(') {
      int depth = 0;
      size_t k = j;
      for (; k < text.size(); ++k) {
        if (text[k] == '(') ++depth;
        else if (text[k] == ')' && --depth == 0) break;
      }
      if (k >= text.size()) return engineErrorBlockRc("invalid syntax", 198);
      out.emplace_back(key, strutil::trim(text.substr(j + 1, k - j - 1)));
      hasArg.push_back(true);
      i = k + 1;
    } else {
      out.emplace_back(key, std::string());
      hasArg.push_back(false);
      i = j;
    }
  }
  return std::nullopt;
}

// A missing(#) value: a number, `.` or `.a`-`.z`.
std::optional<double> missingCode(const std::string& t) {
  if (t == ".") return std::numeric_limits<double>::quiet_NaN();
  if (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z')
    return eval::stataMissingWithKind(t[1] - 'a' + 1);
  try {
    size_t pos = 0;
    const double v = std::stod(t, &pos);
    if (pos == t.size()) return v;
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

// `matrix rowjoinbyname A = B C ... [, missing(#) noconsolidate]` stacks the
// rows of B, C, ... and matches their columns by name: the result's columns
// are every name met, a matrix without one gets missing(#) (default `.`)
// there. By default the names are consolidated, the columns of one equation
// kept together in the order the equations are first met; noconsolidate keeps
// the order the names are first met. coljoinbyname is the same with rows and
// columns exchanged (vs-stata bank 87499ecb, 9b4f1ea8, bdf1fd4a, fd39a462; the
// manual's examples). Names carrying factor-variable or time-series operators
// are consolidated by term in Stata, which is not implemented here.
ojson runJoinByName(bool rowJoin, const std::string& args, SessionState& session) {
  const char* sub = rowJoin ? "rowjoinbyname" : "coljoinbyname";
  const auto [lhsAll, optText] = splitOptions(args);
  const size_t eq = lhsAll.find('=');
  if (eq == std::string::npos) return engineErrorBlockRc("invalid syntax", 198);
  const std::vector<std::string> target = words(lhsAll.substr(0, eq));
  const std::vector<std::string> ops = words(lhsAll.substr(eq + 1));
  if (target.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
  if (!strutil::isValidStataIdentifier(target[0]))
    return engineErrorBlockRc(target[0] + " invalid name", 198);
  if (ops.empty()) return engineErrorBlockRc("invalid syntax", 198);
  double fillValue = std::numeric_limits<double>::quiet_NaN();
  bool consolidate = true;
  std::vector<std::pair<std::string, std::string>> items;
  std::vector<bool> hasArg;
  if (auto bad = optionItems(optText, items, hasArg)) return *bad;
  for (size_t k = 0; k < items.size(); ++k) {
    const std::string& key = items[k].first;
    if (optAbbrev(key, "missing", 3) && hasArg[k]) {
      const std::optional<double> v = missingCode(items[k].second);
      if (!v.has_value()) return engineErrorBlockRc("option missing() incorrectly specified", 198);
      fillValue = *v;
    } else if (optAbbrev(key, "noconsolidate", 3) && !hasArg[k]) {
      consolidate = false;
    } else {
      return engineErrorBlockRc("option " + key + " not allowed", 198);
    }
  }
  std::vector<eval::EvalMatrix> mats(ops.size());
  for (size_t k = 0; k < ops.size(); ++k) {
    if (auto bad = joinOperand(ops[k], session, mats[k])) return *bad;
    if (!rowJoin) mats[k] = transposeMatrix(mats[k]);
    const size_t nr = mats[k].data.size();
    const size_t nc = nr > 0 ? mats[k].data[0].size() : 0;
    if (mats[k].rownames.size() != nr || mats[k].colnames.size() != nc) {
      eval::EvalMatrix named = mats[k];
      applyDefaultStripe(named);
      if (mats[k].rownames.size() != nr) mats[k].rownames = named.rownames;
      if (mats[k].colnames.size() != nc) mats[k].colnames = named.colnames;
    }
    for (size_t j = 0; j < nc; ++j)
      if (mats[k].colnames[j].empty())
        mats[k].colnames[j] = (rowJoin ? "c" : "r") + std::to_string(j + 1);
    for (size_t i = 0; i < nr; ++i)
      if (mats[k].rownames[i].empty())
        mats[k].rownames[i] = (rowJoin ? "r" : "c") + std::to_string(i + 1);
  }
  // The matched names, as (equation, name) in first-met order.
  std::vector<std::pair<std::string, std::string>> names;
  for (const auto& m : mats) {
    std::set<std::pair<std::string, std::string>> seenHere;
    for (const auto& full : m.colnames) {
      const auto p = stripeParts(full);
      if (!seenHere.insert(p).second)
        return engineErrorBlockRc(std::string("matrix ") + sub +
                                      ": a matrix with a repeated name is not implemented in "
                                      "this version of PIET",
                                  198);
      if (consolidate && p.second.find_first_of(".#") != std::string::npos)
        return engineErrorBlockRc(std::string("matrix ") + sub +
                                      ": consolidating factor-variable or time-series names is "
                                      "not implemented in this version of PIET; specify "
                                      "noconsolidate",
                                  198);
      if (std::find(names.begin(), names.end(), p) == names.end()) names.push_back(p);
    }
  }
  if (consolidate) {
    std::vector<std::string> eqOrder;
    for (const auto& p : names)
      if (std::find(eqOrder.begin(), eqOrder.end(), p.first) == eqOrder.end())
        eqOrder.push_back(p.first);
    std::vector<std::pair<std::string, std::string>> grouped;
    for (const auto& e : eqOrder)
      for (const auto& p : names)
        if (p.first == e) grouped.push_back(p);
    names = std::move(grouped);
  }
  eval::EvalMatrix out;
  for (const auto& p : names) out.colnames.push_back(p.first == "_" ? p.second : p.first + ":" + p.second);
  for (const auto& m : mats) {
    std::vector<int> where(names.size(), -1);
    for (size_t j = 0; j < m.colnames.size(); ++j) {
      const auto p = stripeParts(m.colnames[j]);
      const size_t at = static_cast<size_t>(std::find(names.begin(), names.end(), p) - names.begin());
      where[at] = static_cast<int>(j);
    }
    for (size_t i = 0; i < m.data.size(); ++i) {
      std::vector<double> row(names.size(), fillValue);
      for (size_t c = 0; c < names.size(); ++c)
        if (where[c] >= 0) row[c] = m.data[i][static_cast<size_t>(where[c])];
      out.data.push_back(std::move(row));
      out.rownames.push_back(m.rownames[i]);
    }
  }
  if (!rowJoin) out = transposeMatrix(out);
  storeMatrixResult(session, target[0], std::move(out));
  return ojson::array();
}

// ---------------------------------------------------------------------------
// matrix dissimilarity ([P] matrix dissimilarity, [MV] measure_option)

enum class Measure {
  kL2, kL2sq, kL1, kLinf, kLp, kLpower, kCanberra, kCorrelation, kAngular,
  kMatching, kJaccard, kRussell, kHamann, kDice, kAntiDice, kSneath, kRogers,
  kOchiai, kYule, kAnderberg, kKulczynski, kPearson, kGower2
};

bool isBinaryMeasure(Measure m) { return m >= Measure::kMatching; }
bool isSimilarity(Measure m) {
  return m == Measure::kCorrelation || m == Measure::kAngular || isBinaryMeasure(m);
}

// The measure an option names (not case sensitive), or nullopt.
std::optional<Measure> measureNamed(const std::string& keyIn, bool hasArg) {
  std::string k = keyIn;
  for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (hasArg) {
    if (k == "l") return Measure::kLp;
    if (optAbbrev(k, "lpower", 4)) return Measure::kLpower;
    return std::nullopt;
  }
  if (k == "l2" || optAbbrev(k, "euclidean", 3)) return Measure::kL2;
  if (k == "l2squared") return Measure::kL2sq;
  if (k == "l1" || optAbbrev(k, "absolute", 3) || optAbbrev(k, "cityblock", 5) ||
      optAbbrev(k, "manhattan", 6))
    return Measure::kL1;
  if (optAbbrev(k, "linfinity", 4) || optAbbrev(k, "maximum", 3)) return Measure::kLinf;
  if (optAbbrev(k, "canberra", 4)) return Measure::kCanberra;
  if (optAbbrev(k, "correlation", 4)) return Measure::kCorrelation;
  if (optAbbrev(k, "angular", 3) || optAbbrev(k, "angle", 3)) return Measure::kAngular;
  if (optAbbrev(k, "matching", 5)) return Measure::kMatching;
  if (optAbbrev(k, "jaccard", 3)) return Measure::kJaccard;
  if (optAbbrev(k, "russell", 4)) return Measure::kRussell;
  if (k == "hamann") return Measure::kHamann;
  if (k == "dice") return Measure::kDice;
  if (k == "antidice") return Measure::kAntiDice;
  if (k == "sneath") return Measure::kSneath;
  if (k == "rogers") return Measure::kRogers;
  if (k == "ochiai") return Measure::kOchiai;
  if (k == "yule") return Measure::kYule;
  if (optAbbrev(k, "anderberg", 5)) return Measure::kAnderberg;
  if (optAbbrev(k, "kulczynski", 4)) return Measure::kKulczynski;
  if (k == "pearson") return Measure::kPearson;
  if (k == "gower2") return Measure::kGower2;
  return std::nullopt;
}

// The measure between two vectors (the two observations' values of the
// variables, or the two variables' values over the observations).
double measureOf(Measure m, double p, const std::vector<double>& x, const std::vector<double>& y,
                 bool proportions) {
  const size_t n = x.size();
  if (!isBinaryMeasure(m)) {
    double s = 0.0;
    switch (m) {
      case Measure::kL2:
      case Measure::kL2sq:
        for (size_t k = 0; k < n; ++k) s += (x[k] - y[k]) * (x[k] - y[k]);
        return m == Measure::kL2 ? std::sqrt(s) : s;
      case Measure::kL1:
        for (size_t k = 0; k < n; ++k) s += std::fabs(x[k] - y[k]);
        return s;
      case Measure::kLinf:
        for (size_t k = 0; k < n; ++k) s = std::max(s, std::fabs(x[k] - y[k]));
        return s;
      case Measure::kLp:
      case Measure::kLpower:
        for (size_t k = 0; k < n; ++k) s += std::pow(std::fabs(x[k] - y[k]), p);
        return m == Measure::kLp ? std::pow(s, 1.0 / p) : s;
      case Measure::kCanberra:
        for (size_t k = 0; k < n; ++k) {
          const double den = std::fabs(x[k]) + std::fabs(y[k]);
          if (den > 0.0) s += std::fabs(x[k] - y[k]) / den;
        }
        return s;
      case Measure::kCorrelation: {
        double mx = 0.0, my = 0.0;
        for (size_t k = 0; k < n; ++k) {
          mx += x[k];
          my += y[k];
        }
        mx /= static_cast<double>(n);
        my /= static_cast<double>(n);
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (size_t k = 0; k < n; ++k) {
          sxy += (x[k] - mx) * (y[k] - my);
          sxx += (x[k] - mx) * (x[k] - mx);
          syy += (y[k] - my) * (y[k] - my);
        }
        return sxy / std::sqrt(sxx * syy);
      }
      case Measure::kAngular: {
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (size_t k = 0; k < n; ++k) {
          sxy += x[k] * y[k];
          sxx += x[k] * x[k];
          syy += y[k] * y[k];
        }
        return sxy / std::sqrt(sxx * syy);
      }
      default:
        break;
    }
  }
  // The 2x2 counts: a both one, b x one y zero, c x zero y one, d both zero;
  // under proportions the sums of the products of the proportions.
  double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
  for (size_t k = 0; k < n; ++k) {
    const double u = proportions ? x[k] : (x[k] != 0.0 ? 1.0 : 0.0);
    const double v = proportions ? y[k] : (y[k] != 0.0 ? 1.0 : 0.0);
    a += u * v;
    b += u * (1.0 - v);
    c += (1.0 - u) * v;
    d += (1.0 - u) * (1.0 - v);
  }
  const double t = a + b + c + d;
  const bool xZero = a + b == 0.0, yZero = a + c == 0.0;
  const bool xOne = c + d == 0.0, yOne = b + d == 0.0;
  switch (m) {
    case Measure::kMatching: return (a + d) / t;
    case Measure::kJaccard: return a + b + c == 0.0 ? 1.0 : a / (a + b + c);
    case Measure::kRussell: return a / t;
    case Measure::kHamann: return ((a + d) - (b + c)) / t;
    case Measure::kDice: return a + b + c == 0.0 ? 1.0 : 2.0 * a / (2.0 * a + b + c);
    case Measure::kAntiDice: return a + b + c == 0.0 ? 1.0 : a / (a + 2.0 * (b + c));
    case Measure::kSneath: return 2.0 * (a + d) / (2.0 * (a + d) + (b + c));
    case Measure::kRogers: return (a + d) / ((a + d) + 2.0 * (b + c));
    case Measure::kOchiai:
      if (xZero && yZero) return 1.0;
      if (xZero || yZero) return 0.0;
      return a / std::sqrt((a + b) * (a + c));
    case Measure::kYule:
    case Measure::kPearson:
      if (b + c == 0.0) return 1.0;
      if (a + d == 0.0) return -1.0;
      if (a * d - b * c == 0.0) return 0.0;
      return m == Measure::kYule ? (a * d - b * c) / (a * d + b * c)
                                 : (a * d - b * c) / std::sqrt((a + b) * (a + c) * (d + b) * (d + c));
    case Measure::kAnderberg:
      if ((xOne && yOne) || (xZero && yZero)) return 1.0;
      if (a + b == 0.0 || a + c == 0.0 || c + d == 0.0 || b + d == 0.0) return 0.0;
      return (a / (a + b) + a / (a + c) + d / (c + d) + d / (b + d)) / 4.0;
    case Measure::kKulczynski:
      if (xZero && yZero) return 1.0;
      if (xZero || yZero) return 0.0;
      return (a / (a + b) + a / (a + c)) / 2.0;
    case Measure::kGower2:
      if ((xOne && yOne) || (xZero && yZero)) return 1.0;
      if (a * d == 0.0) return 0.0;
      return a * d / std::sqrt((a + b) * (a + c) * (d + b) * (d + c));
    default:
      return std::numeric_limits<double>::quiet_NaN();
  }
}

// `matrix dissimilarity D = [varlist] [if] [in] [, measure observations
// variables names(var) allbinary proportions dissim(oneminus|standard)]`: the
// matrix of the measure between every pair of observations (default; rows
// obs# by observation number, or the values of names()) or of variables,
// over the observations with no missing value in the variables. L2 is the
// default measure; binary measures read a nonzero value as one unless
// proportions. dissim() turns a similarity s into 1-s or
// sqrt(s_ii+s_jj-2s_ij) and leaves a dissimilarity alone (vs-stata bank
// 0251eee2, 05574139, 700c9e82, 86c185bd, bbc7fba3, c99b2bb4, d74a9208,
// f98a3a8a). The Gower measure, which keeps observations with missing
// values, is not implemented here.
ojson runDissimilarity(const std::string& args, SessionState& session) {
  eval::ParsedCommand parsed;
  try {
    parsed = eval::tokenize("matrix dissimilarity " + args);
  } catch (const eval::TokenizerError& te) {
    return ojson(te.block);
  }
  const auto eqIt = std::find(parsed.varlist.begin(), parsed.varlist.end(), "=");
  if (eqIt == parsed.varlist.end() || eqIt - parsed.varlist.begin() != 2)
    return engineErrorBlockRc("invalid syntax", 198);
  const std::string name = parsed.varlist[1];
  if (!strutil::isValidStataIdentifier(name)) return engineErrorBlockRc(name + " invalid name", 198);
  if (parsed.weightType.has_value()) return engineErrorBlockRc("weights not allowed", 101);
  std::optional<Measure> measure;
  double power = 2.0;
  bool variables = false, observations = false, allbinary = false, proportions = false;
  std::string namesVar, dissim;
  const auto setMeasure = [&](Measure m) -> std::optional<ojson> {
    if (measure.has_value())
      return engineErrorBlockRc("matrix dissimilarity: more than one measure is not implemented "
                                "in this version of PIET",
                                198);
    measure = m;
    return std::nullopt;
  };
  for (const auto& [key, val] : parsed.opts) {
    std::string lk = key;
    for (char& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (optAbbrev(lk, "observations", 3) && val.isBool) observations = true;
    else if (optAbbrev(lk, "variables", 3) && val.isBool) variables = true;
    else if (optAbbrev(lk, "names", 4) && !val.isBool) namesVar = strutil::trim(val.strValue);
    else if (optAbbrev(lk, "allbinary", 4) && val.isBool) allbinary = true;
    else if (optAbbrev(lk, "proportions", 4) && val.isBool) proportions = true;
    else if (lk == "dissim" && !val.isBool) dissim = strutil::trim(val.strValue);
    else if (optAbbrev(lk, "measure", 3) && !val.isBool) {
      eval::ParsedCommand inner;
      try {
        inner = eval::tokenize("x, " + strutil::trim(val.strValue));
      } catch (const eval::TokenizerError& te) {
        return ojson(te.block);
      }
      if (inner.opts.size() != 1) return engineErrorBlockRc("invalid measure()", 198);
      const auto& [ik, iv] = inner.opts[0];
      const std::optional<Measure> m = measureNamed(ik, !iv.isBool);
      if (!m.has_value()) return engineErrorBlockRc(ik + " invalid measure", 198);
      if (!iv.isBool) power = std::atof(iv.strValue.c_str());
      if (auto bad = setMeasure(*m)) return *bad;
    } else if (const std::optional<Measure> m = measureNamed(key, !val.isBool)) {
      if (!val.isBool) {
        const std::optional<double> pv = missingCode(strutil::trim(val.strValue));
        if (!pv.has_value() || std::isnan(*pv)) return engineErrorBlockRc("invalid syntax", 198);
        power = *pv;
      }
      if (auto bad = setMeasure(*m)) return *bad;
    } else if (lk == "gower") {
      return engineErrorBlockRc("matrix dissimilarity: the Gower measure is not implemented in "
                                "this version of PIET",
                                198);
    } else {
      return engineErrorBlockRc("option " + key + " not allowed", 198);
    }
  }
  const Measure m = measure.value_or(Measure::kL2);
  if ((m == Measure::kLp || m == Measure::kLpower) && !(power >= 1.0))
    return engineErrorBlockRc("matrix dissimilarity: L(#) and Lpower(#) require # >= 1", 198);
  if (variables && observations)
    return engineErrorBlockRc("options observations and variables are exclusive", 198);
  if (variables && !namesVar.empty())
    return engineErrorBlockRc("option names() not allowed with variables", 198);
  if (allbinary && proportions)
    return engineErrorBlockRc("options allbinary and proportions are exclusive", 198);
  if (!dissim.empty() && dissim != "oneminus" && !optAbbrev(dissim, "standard", 2))
    return engineErrorBlockRc("dissim(" + dissim + ") invalid", 198);
  if (!session.hasData() || session.datastore() == nullptr)
    return engineErrorBlockRc("no variables defined", 111);
  std::vector<std::string> vars(eqIt + 1, parsed.varlist.end());
  if (vars.empty()) {
    for (const auto& v : session.datastore()->names())
      if (!isHiddenColumn(v)) vars.push_back(v);
  } else {
    for (const auto& v : vars)
      if (v.find_first_of(".#") != std::string::npos)
        return engineErrorBlockRc("matrix dissimilarity: factor-variable and time-series "
                                  "operators (" + v + ") are not supported in this version of PIET",
                                  198);
    if (auto bad = resolveByKeyNames(session, vars)) return *bad;
  }
  // The selection, keeping each row's observation number.
  std::string indexVar;
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilterKeepingIndex(session, *session.currentHbin(), parsed.ifExpr,
                                        parsed.inRange, &indexVar);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "matrix dissimilarity");
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = session.datastore();
  if (fr.dataPath != *session.currentHbin()) {
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  std::vector<const double*> cols;
  for (const auto& v : vars) {
    if (!ds->has(v)) return engineErrorBlockRc("variable " + v + " not found", 111);
    if (ds->dtype(v) != "f64")
      return engineErrorBlockRc("matrix dissimilarity: the string variable " + v +
                                    " is not implemented in this version of PIET",
                                198);
    cols.push_back(ds->col(v).f64());
  }
  if (!namesVar.empty()) {
    std::vector<std::string> one = {namesVar};
    if (auto bad = resolveByKeyNames(session, one)) return *bad;
    namesVar = one[0];
    if (!ds->has(namesVar)) return engineErrorBlockRc("variable " + namesVar + " not found", 111);
    if (ds->dtype(namesVar) != "factor") return engineErrorBlockRc("type mismatch", 109);
  }
  const double* obsIndex =
      !indexVar.empty() && ds->has(indexVar) ? ds->col(indexVar).f64() : nullptr;
  std::vector<std::vector<double>> rows;  // kept observations x variables
  std::vector<std::string> rowNames;
  for (int64_t r = 0; r < ds->nRows(); ++r) {
    std::vector<double> row(cols.size());
    bool keep = true;
    for (size_t c = 0; c < cols.size() && keep; ++c) {
      row[c] = cols[c][r];
      keep = !cecon::is_missing(row[c]);
    }
    if (!keep) continue;
    for (double v : row) {
      if (allbinary && v != 0.0 && v != 1.0)
        return engineErrorBlockRc("matrix dissimilarity: allbinary found a value other than 0 "
                                  "and 1; this refusal is not implemented with Stata's message",
                                  198);
      if (proportions && (v < 0.0 || v > 1.0))
        return engineErrorBlockRc("matrix dissimilarity: proportions found a value outside "
                                  "[0,1]; this refusal is not implemented with Stata's message",
                                  198);
    }
    rows.push_back(std::move(row));
    if (!namesVar.empty()) {
      const cecon::Column& nc = ds->col(namesVar);
      const int32_t code = nc.codes()[r];
      std::string label =
          code >= 0 && static_cast<size_t>(code) < nc.levels.size() ? nc.levels[static_cast<size_t>(code)] : "";
      for (char& ch : label)
        if (ch == ' ') ch = '_';
      rowNames.push_back(label);
    } else {
      const int64_t obs = obsIndex != nullptr ? static_cast<int64_t>(std::llround(obsIndex[r])) : r + 1;
      rowNames.push_back("obs" + std::to_string(obs));
    }
  }
  if (rows.empty()) return engineErrorBlockRc("no observations", 2000);
  // The vectors compared: the observations, or the variables over them.
  std::vector<std::vector<double>> vec;
  if (variables) {
    vec.assign(cols.size(), std::vector<double>(rows.size()));
    for (size_t i = 0; i < rows.size(); ++i)
      for (size_t c = 0; c < cols.size(); ++c) vec[c][i] = rows[i][c];
    rowNames = vars;
  } else {
    vec = rows;
  }
  const size_t n = vec.size();
  eval::EvalMatrix out;
  out.data.assign(n, std::vector<double>(n, 0.0));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j <= i; ++j) {
      const double s = measureOf(m, power, vec[i], vec[j], proportions);
      out.data[i][j] = out.data[j][i] = s;
    }
  if (!dissim.empty() && isSimilarity(m)) {
    std::vector<double> diag(n);
    for (size_t i = 0; i < n; ++i) diag[i] = out.data[i][i];
    for (size_t i = 0; i < n; ++i)
      for (size_t j = 0; j < n; ++j) {
        const double s = out.data[i][j];
        out.data[i][j] = dissim == "oneminus" ? 1.0 - s : std::sqrt(diag[i] + diag[j] - 2.0 * s);
      }
  }
  out.rownames = rowNames;
  out.colnames = rowNames;
  storeMatrixResult(session, name, std::move(out));
  return ojson::array();
}

}  // namespace

std::optional<ojson> runMatrixSubcommand(const std::string& full, const std::string& args,
                                         SessionState& session) {
  if (full == "dir") return runMatrixDir(args, session);
  if (full == "rename") return runMatrixRename(args, session);
  if (full == "symeigen") return runSymeigen(args, session);
  if (full == "eigenvalues") return runEigenvalues(args, session);
  if (full == "svd") return runSvd(args, session);
  if (full == "score") return runMatrixScore(args, session);
  if (full == "vecaccum") return runVecaccum(args, session);
  if (full == "opaccum") return runGroupAccum(false, args, session);
  if (full == "glsaccum") return runGroupAccum(true, args, session);
  if (full == "rowjoinbyname") return runJoinByName(true, args, session);
  if (full == "coljoinbyname") return runJoinByName(false, args, session);
  if (full == "dissimilarity") return runDissimilarity(args, session);
  if (full == "roweq" || full == "coleq") {
    const size_t eq = args.find('=');
    if (eq == std::string::npos) return engineErrorBlockRc("invalid syntax", 198);
    const std::vector<std::string> target = words(args.substr(0, eq));
    if (target.size() != 1) return engineErrorBlockRc("invalid syntax", 198);
    return runMatrixStripe(full == "roweq", target[0], args.substr(eq + 1), session,
                           /*equations=*/true);
  }
  return std::nullopt;
}

}  // namespace command_detail

// ---------------------------------------------------------------------------
// matname ([P] matrix rownames): `matname A namelist [, rows(range)
// columns(range) explicit]` puts the names on the rows and/or columns of A a
// range selects (`.` all, `#`, `#...` to the last, `#..#`); with neither
// option, on both. Without explicit the names are a varlist of the data in
// memory, unabbreviated, `_cons` kept as written; an empty namelist is every
// variable. A is checked first (r(111) "matrix A not found"); a name that is
// no variable is r(111); a range that is malformed is r(198), outside the
// matrix or of the wrong length r(503) (vs-stata bank 026b6826, 152fcdbd,
// 805457fe, fd8a6741, ffe5e861).
ojson runMatname(const eval::ParsedCommand& parsed, SessionState& session) {
  using namespace command_detail;
  if (parsed.varlist.empty()) return engineErrorBlockRc("matrix  not found", 111);
  const std::string matName = parsed.varlist[0];
  const eval::EvalMatrix* mat = session.eval.findMatrix(matName);
  if (mat == nullptr) return engineErrorBlockRc("matrix " + matName + " not found", 111);
  std::optional<std::string> rowsSpec, colsSpec;
  bool explicitNames = false;
  for (const auto& [key, val] : parsed.opts) {
    if (optAbbrev(key, "rows", 1) && !val.isBool) rowsSpec = strutil::trim(val.strValue);
    else if (optAbbrev(key, "columns", 1) && !val.isBool) colsSpec = strutil::trim(val.strValue);
    else if (optAbbrev(key, "explicit", 1) && val.isBool) explicitNames = true;
    else return engineErrorBlockRc("option " + key + " not allowed", 198);
  }
  std::vector<std::string> names(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (!explicitNames) {
    Datastore* ds = session.hasData() ? session.datastore() : nullptr;
    const auto expand = [&](std::vector<std::string>& group,
                            std::vector<std::string>& into) -> std::optional<ojson> {
      if (group.empty()) return std::nullopt;
      if (ds == nullptr) return engineErrorBlockRc("no variables defined", 111);
      if (auto bad = resolveByKeyNames(session, group)) return *bad;
      for (const auto& v : group) {
        if (!ds->has(v) || isHiddenColumn(v))
          return engineErrorBlockRc("variable " + v + " not found", 111);
        into.push_back(v);
      }
      group.clear();
      return std::nullopt;
    };
    std::vector<std::string> out, pending;
    if (names.empty()) {
      if (ds != nullptr)
        for (const auto& v : ds->names())
          if (!isHiddenColumn(v)) out.push_back(v);
    } else {
      for (const auto& n : names) {
        if (n == "_cons") {
          if (auto bad = expand(pending, out)) return *bad;
          out.push_back(n);
        } else {
          pending.push_back(n);
        }
      }
      if (auto bad = expand(pending, out)) return *bad;
    }
    names = std::move(out);
  }
  if (!rowsSpec.has_value() && !colsSpec.has_value()) rowsSpec = colsSpec = std::string(".");
  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  // The range [first, last] (1-based) a rows()/columns() text selects.
  const auto place = [&](const std::string& spec, size_t total, size_t& first,
                         size_t& last) -> std::optional<ojson> {
    static const std::regex reRange(R"(^\s*(?:(\.)|(\d+)(?:\s*(\.\.\.|\.\.\s*(\d+)))?)\s*$)");
    std::smatch m;
    if (!std::regex_match(spec, m, reRange))
      return engineErrorBlockRc("invalid syntax in rows() or columns() options", 198);
    if (m[1].matched) {
      first = 1;
      last = total;
    } else {
      first = static_cast<size_t>(std::stoull(m.str(2)));
      last = !m[3].matched ? first : m[4].matched ? static_cast<size_t>(std::stoull(m.str(4))) : total;
    }
    if (!(1 <= first && first <= last && last <= total))
      return engineErrorBlockRc("invalid rows() or columns() ranges", 503);
    if (last - first + 1 != names.size())
      return engineErrorBlockRc("number of names and rows() or columns() ranges do not match", 503);
    return std::nullopt;
  };
  const auto rename = [&](bool rows, const std::string& spec) -> std::optional<ojson> {
    const eval::EvalMatrix* cur = session.eval.findMatrix(matName);
    const size_t total = rows ? nrows : ncols;
    size_t first = 0, last = 0;
    if (auto bad = place(spec, total, first, last)) return *bad;
    const std::vector<std::string>& stripe = rows ? cur->rownames : cur->colnames;
    std::string list;
    for (size_t i = 1; i <= total; ++i) {
      std::string nm;
      if (i >= first && i <= last) {
        nm = names[i - first];
      } else {
        nm = i - 1 < stripe.size() ? stripe[i - 1] : std::string();
        const size_t colon = nm.find(':');
        if (colon != std::string::npos) nm = nm.substr(colon + 1);
        if (nm.empty()) nm = (rows ? "r" : "c") + std::to_string(i);
      }
      list += " " + nm;
    }
    ojson blocks =
        dispatch(std::string("matrix ") + (rows ? "rownames " : "colnames ") + matName + " =" + list,
                 session);
    for (const auto& blk : blocks)
      if (blk.is_object() && blk.value("type", std::string()) == "error") return blk;
    return std::nullopt;
  };
  if (rowsSpec.has_value())
    if (auto bad = rename(true, *rowsSpec)) return *bad;
  if (colsSpec.has_value())
    if (auto bad = rename(false, *colsSpec)) return *bad;
  return ojson::array();
}

}  // namespace piet::handlers
