/* ============================================================================
 *  test_adapter_matrices.cpp — properties of the EXTENDED-format adaptor
 *  matrices in src/adaptermatrices/ (2D circular, upper hemisphere).
 *
 *  When writing, libambix reduces the full set with R = pinv(A) (computed as
 *  (A^T A)^-1 A^T in float32), and a reader expands with A. For this to be
 *  lossless on repeated application, R A must equal I. Then reduce -> expand
 *  is a projector P = A R with P P = P: the first pass discards what the
 *  reduced format cannot hold, and every further pass changes nothing.
 *
 *  The shipped matrices have orthonormal columns, so R = A^T. These cases
 *  pin that down, together with the channel counts and the per-degree (m)
 *  block structure. scripts/generate_adapter_matrices.py --check compares
 *  the matrices with the construction they come from.
 *
 *  Needs neither REAPER nor libambix. ambix_matrix_t is re-declared with the
 *  same layout as in <ambix/ambix.h>, only so the headers compile.
 * ==========================================================================*/

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

typedef struct ambix_matrix_t
{
  uint32_t rows;
  uint32_t cols;
  float **data;
} ambix_matrix_t;

#include "adaptermatrices/adapter_circular.h"
#include "adaptermatrices/adapter_hemi.h"

static int g_failures = 0;

static void Check(bool ok, const char *what, int order)
{
  if (!ok)
  {
    printf("FAIL: %s (order %d)\n", what, order);
    ++g_failures;
  }
}

static int Acn(int n, int m) { return n * n + n + m; }
static int DegreeOfAcn(int q) { int n = (int)std::sqrt((double)q); return q - n * n - n; }

struct Matrix
{
  int rows, cols;
  std::vector<float> v;
  Matrix(int r, int c) : rows(r), cols(c), v((size_t)r * c, 0.0f) {}
  float &operator()(int r, int c) { return v[(size_t)r * cols + c]; }
  float operator()(int r, int c) const { return v[(size_t)r * cols + c]; }
};

static Matrix Load(const float *data, int rows, int cols)
{
  Matrix m(rows, cols);
  for (int i = 0; i < rows * cols; ++i) m.v[i] = data[i];
  return m;
}

static Matrix Mul(const Matrix &a, const Matrix &b)
{
  Matrix c(a.rows, b.cols);
  for (int i = 0; i < a.rows; ++i)
    for (int j = 0; j < b.cols; ++j)
    {
      float s = 0.0f;
      for (int k = 0; k < a.cols; ++k) s += a(i, k) * b(k, j);
      c(i, j) = s;
    }
  return c;
}

static Matrix Transpose(const Matrix &a)
{
  Matrix t(a.cols, a.rows);
  for (int i = 0; i < a.rows; ++i)
    for (int j = 0; j < a.cols; ++j) t(j, i) = a(i, j);
  return t;
}

/* float32 Gauss-Jordan inverse, SPD input (A^T A) */
static Matrix Inverse(Matrix a)
{
  const int n = a.rows;
  Matrix inv(n, n);
  for (int i = 0; i < n; ++i) inv(i, i) = 1.0f;
  for (int c = 0; c < n; ++c)
  {
    const float p = a(c, c);
    for (int j = 0; j < n; ++j) { a(c, j) /= p; inv(c, j) /= p; }
    for (int r = 0; r < n; ++r)
    {
      if (r == c) continue;
      const float f = a(r, c);
      for (int j = 0; j < n; ++j) { a(r, j) -= f * a(c, j); inv(r, j) -= f * inv(c, j); }
    }
  }
  return inv;
}

/* the reduction matrix as libambix derives it: (A^T A)^-1 A^T */
static Matrix Reduction(const Matrix &a)
{
  const Matrix at = Transpose(a);
  return Mul(Inverse(Mul(at, a)), at);
}

static float MaxAbsDiffFromIdentity(const Matrix &m)
{
  float e = 0.0f;
  for (int i = 0; i < m.rows; ++i)
    for (int j = 0; j < m.cols; ++j) e = std::fmax(e, std::fabs(m(i, j) - (i == j ? 1.0f : 0.0f)));
  return e;
}

static void CheckMatrix(const Matrix &a, int order, int wantCols, bool binary)
{
  const int L = (order + 1) * (order + 1);
  Check(a.rows == L, "rows = (N+1)^2", order);
  Check(a.cols == wantCols, "column count", order);

  /* orthonormal columns: stored with 10 decimals, so ~1e-7 */
  Check(MaxAbsDiffFromIdentity(Mul(Transpose(a), a)) < 1e-6f, "A^T A = I", order);

  /* R A = I with libambix's formula in float32, and R equals A^T */
  const Matrix r = Reduction(a);
  Check(MaxAbsDiffFromIdentity(Mul(r, a)) < 1e-5f, "pinv(A) A = I", order);
  const Matrix at = Transpose(a);
  float d = 0.0f;
  for (size_t i = 0; i < r.v.size(); ++i) d = std::fmax(d, std::fabs(r.v[i] - at.v[i]));
  Check(d < 1e-5f, "pinv(A) = A^T", order);

  /* P = A R is idempotent */
  const Matrix p = Mul(a, r);
  const Matrix pp = Mul(p, p);
  float e = 0.0f;
  for (size_t i = 0; i < p.v.size(); ++i) e = std::fmax(e, std::fabs(pp.v[i] - p.v[i]));
  Check(e < 1e-5f, "P P = P", order);

  /* no column mixes azimuthal degrees: sin/cos(m phi) never couple */
  bool blockDiag = true;
  for (int c = 0; c < a.cols; ++c)
  {
    int m = 0x7fff;
    for (int q = 0; q < L; ++q)
    {
      if (a(q, c) == 0.0f) continue;
      if (m == 0x7fff) m = DegreeOfAcn(q);
      else if (DegreeOfAcn(q) != m) blockDiag = false;
    }
  }
  Check(blockDiag, "each column lives in a single degree m", order);

  if (binary)
  {
    bool ok = true;
    for (size_t i = 0; i < a.v.size(); ++i) ok &= (a.v[i] == 0.0f || a.v[i] == 1.0f);
    Check(ok, "entries are exactly 0 or 1", order);
  }

  /* repeated reduce -> expand in float32, as a chain of renders would do it:
   * the first pass projects, every further pass must leave the signal alone */
  std::vector<float> x(L), c(a.cols), first(L);
  uint32_t seed = 12345u + (uint32_t)order;
  for (int q = 0; q < L; ++q)
  {
    seed = seed * 1664525u + 1013904223u;
    x[q] = (float)((seed >> 8) / 16777216.0 * 2.0 - 1.0);
  }
  const std::vector<float> input = x;
  for (int pass = 0; pass < 100; ++pass)
  {
    for (int i = 0; i < a.cols; ++i) { float s = 0.0f; for (int q = 0; q < L; ++q) s += r(i, q) * x[q]; c[i] = s; }
    for (int q = 0; q < L; ++q) { float s = 0.0f; for (int i = 0; i < a.cols; ++i) s += a(q, i) * c[i]; x[q] = s; }
    if (pass == 0) first = x;
  }
  double drift = 0.0, norm = 0.0, lost = 0.0, in = 0.0;
  for (int q = 0; q < L; ++q)
  {
    drift += (double)(x[q] - first[q]) * (x[q] - first[q]);
    norm += (double)first[q] * first[q];
    lost += (double)(input[q] - first[q]) * (input[q] - first[q]);
    in += (double)input[q] * input[q];
  }
  Check(std::sqrt(drift / norm) < 1e-4, "100 reduce/expand passes add no loss after the first (< -80 dB)", order);
  Check(std::sqrt(lost / in) > 0.1, "first pass does discard out-of-subspace content", order);
}

static void TestCircular()
{
  for (int order = 1; order <= 7; ++order)
  {
    const int L = (order + 1) * (order + 1), C = 2 * order + 1;
    const Matrix a = Load(adapter_circular[order - 1], L, C);
    CheckMatrix(a, order, C, true);

    /* the kept channels are W and the sectoral pairs |m| = n, in ACN order */
    bool ok = a(0, 0) == 1.0f;
    for (int n = 1; n <= order; ++n)
      ok &= a(Acn(n, -n), 2 * n - 1) == 1.0f && a(Acn(n, n), 2 * n) == 1.0f;
    Check(ok, "circular keeps W and the sectoral harmonics", order);
  }
}

static void TestHemi()
{
  for (int order = 1; order <= 7; ++order)
  {
    const ambix_matrix_t &dims = mtx_adapter_hemi[order - 1];
    const int C = (order + 1) * (order + 2) / 2;
    Check((int)dims.rows == (order + 1) * (order + 1) && (int)dims.cols == C, "mtx_adapter_hemi dimensions", order);
    const Matrix a = Load(adapter_hemi[order - 1], dims.rows, dims.cols);
    CheckMatrix(a, order, C, false);

    /* sectoral harmonics are untouched: they are z-symmetric with nothing to mix with */
    const int col = C - 1; /* last column: ACN (N, +N) */
    Check(a(Acn(order, order), col) == 1.0f, "sectoral (N,+N) passes through", order);
  }
}

int main()
{
  TestCircular();
  TestHemi();

  if (g_failures)
  {
    printf("%d adaptor-matrix check(s) failed\n", g_failures);
    return 1;
  }
  printf("all adaptor-matrix checks passed\n");
  return 0;
}
