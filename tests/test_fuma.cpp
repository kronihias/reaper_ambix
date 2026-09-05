/* ============================================================================
 *  test_fuma.cpp — FuMa (B-format) to ambiX channel conversion.
 *
 *  The reference is section 4.2.1 of the ambiX paper, which gives the
 *  first-order matrix explicitly, plus the standard maxN peak values for
 *  orders 2 and 3.
 *
 *  This suite exists because the obvious shortcut — libambix's
 *  AMBIX_MATRIX_FUMA — is wrong in two independent ways (inverted permutation,
 *  spurious Condon-Shortley signs) and its own round-trip test does not catch
 *  either. If someone later "simplifies" src/fuma.cpp back onto libambix,
 *  these cases fail.
 * ==========================================================================*/

#include "fuma.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
  if (!ok)
  {
    printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

static void CheckClose(double got, double want, double tol, const char *what)
{
  if (fabs(got - want) > tol)
  {
    printf("FAIL: %s (got %.6f, want %.6f)\n", what, got, want);
    ++g_failures;
  }
}

/* Convenience: run one FuMa frame through the matrix. */
static std::vector<double> Apply(const std::vector<double> &m, int rows,
                                 const std::vector<double> &in)
{
  const int cols = (int)in.size();
  std::vector<double> out((size_t)rows, 0.0);
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      out[(size_t)r] += m[(size_t)r * (size_t)cols + (size_t)c] * in[(size_t)c];
  return out;
}

/* --------------------------------------------------------------------------
 * The channel counts FuMa defines, and the ones it does not.
 * ------------------------------------------------------------------------*/
static void TestChannelCounts()
{
  Check(AmbixFuMaTargetChannels(1)  == 1,  "1 ch (W) -> 1");
  Check(AmbixFuMaTargetChannels(3)  == 4,  "3 ch (WXY) -> 4");
  Check(AmbixFuMaTargetChannels(4)  == 4,  "4 ch (WXYZ) -> 4");
  Check(AmbixFuMaTargetChannels(5)  == 9,  "5 ch (WXYUV) -> 9");
  Check(AmbixFuMaTargetChannels(6)  == 9,  "6 ch (WXYZUV) -> 9");
  Check(AmbixFuMaTargetChannels(7)  == 16, "7 ch (WXYUVPQ) -> 16");
  Check(AmbixFuMaTargetChannels(8)  == 16, "8 ch (WXYZUVPQ) -> 16");
  Check(AmbixFuMaTargetChannels(9)  == 9,  "9 ch (WXYZRSTUV) -> 9");
  Check(AmbixFuMaTargetChannels(11) == 16, "11 ch (WXYZRSTUVPQ) -> 16");
  Check(AmbixFuMaTargetChannels(16) == 16, "16 ch (full third order) -> 16");

  /* Not FuMa sets. 2 and 10 in particular are easy to get wrong. */
  Check(AmbixFuMaTargetChannels(0)  == 0, "0 ch rejected");
  Check(AmbixFuMaTargetChannels(2)  == 0, "2 ch rejected");
  Check(AmbixFuMaTargetChannels(10) == 0, "10 ch rejected");
  Check(AmbixFuMaTargetChannels(12) == 0, "12 ch rejected");
  Check(AmbixFuMaTargetChannels(25) == 0, "25 ch rejected (FuMa stops at 3rd)");
}

/* --------------------------------------------------------------------------
 * First order, straight out of the paper:
 *
 *   A = [sqrt2 0 0 0]   applied to [W X Y Z]
 *       [    0 0 1 0]
 *       [    0 0 0 1]
 *       [    0 1 0 0]
 * ------------------------------------------------------------------------*/
static void TestFirstOrderMatrix()
{
  std::vector<double> m;
  const int rows = AmbixFuMaToAmbixMatrix(4, m);
  Check(rows == 4, "WXYZ gives a 4-row matrix");
  if (rows != 4) return;

  const double want[16] =
  {
    1.41421356, 0, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
    0, 1, 0, 0
  };
  for (int i = 0; i < 16; ++i)
  {
    char what[64];
    snprintf(what, sizeof(what), "first-order matrix entry [%d][%d]", i / 4, i % 4);
    CheckClose(m[(size_t)i], want[i], 1e-6, what);
  }
}

/* Each FuMa component has to land in its own ambiX slot, at unit gain. This is
 * the check libambix fails: it routes X into ACN2 (Z) and flips signs. */
static void TestFirstOrderRouting()
{
  std::vector<double> m;
  const int rows = AmbixFuMaToAmbixMatrix(4, m);
  if (rows != 4) return;

  /* [W X Y Z] unit impulses -> [ACN0 ACN1 ACN2 ACN3] = [W Y Z X] */
  const int wantSlot[4] = { 0, 3, 1, 2 };   /* FuMa c lands in ACN wantSlot[c] */
  const char *label[4]  = { "W", "X", "Y", "Z" };

  for (int c = 0; c < 4; ++c)
  {
    std::vector<double> in(4, 0.0);
    in[(size_t)c] = 1.0;
    const std::vector<double> out = Apply(m, rows, in);

    for (int r = 0; r < 4; ++r)
    {
      char what[96];
      snprintf(what, sizeof(what), "FuMa %s=1 -> ACN%d", label[c], r);
      const double want = (r == wantSlot[c]) ? (c == 0 ? sqrt(2.0) : 1.0) : 0.0;
      CheckClose(out[(size_t)r], want, 1e-6, what);
    }
  }
}

/* --------------------------------------------------------------------------
 * Orders 2 and 3: ordering and the maxN gains.
 * ------------------------------------------------------------------------*/
static void TestHigherOrderRouting()
{
  std::vector<double> m;
  const int rows = AmbixFuMaToAmbixMatrix(16, m);
  Check(rows == 16, "full third order gives a 16-row matrix");
  if (rows != 16) return;

  /* FuMa index -> (ACN slot, gain). W X Y Z R S T U V K L M N O P Q */
  const int    slot[16] = { 0, 3, 1, 2, 6, 7, 5, 8, 4, 12, 13, 11, 14, 10, 15, 9 };
  const double s3_2   = sqrt(3.0) / 2.0;
  const double s32_45 = 4.0 * sqrt(2.0 / 5.0) / 3.0;
  const double s5_9   = sqrt(5.0) / 3.0;
  const double s5_8   = sqrt(5.0 / 2.0) / 2.0;
  const double gain[16] =
  {
    sqrt(2.0),                    /* W */
    1.0, 1.0, 1.0,                /* X Y Z */
    1.0, s3_2, s3_2, s3_2, s3_2,  /* R S T U V */
    1.0, s32_45, s32_45, s5_9, s5_9, s5_8, s5_8  /* K L M N O P Q */
  };
  const char *label[16] =
  { "W","X","Y","Z","R","S","T","U","V","K","L","M","N","O","P","Q" };

  for (int c = 0; c < 16; ++c)
  {
    std::vector<double> in(16, 0.0);
    in[(size_t)c] = 1.0;
    const std::vector<double> out = Apply(m, rows, in);

    for (int r = 0; r < 16; ++r)
    {
      char what[96];
      snprintf(what, sizeof(what), "FuMa %s=1 -> ACN%d", label[c], r);
      CheckClose(out[(size_t)r], (r == slot[c]) ? gain[c] : 0.0, 1e-6, what);
    }
  }
}

/* Every gain is positive — no Condon-Shortley phase. */
static void TestNoNegativeEntries()
{
  const int counts[10] = { 1, 3, 4, 5, 6, 7, 8, 9, 11, 16 };
  for (int i = 0; i < 10; ++i)
  {
    std::vector<double> m;
    const int rows = AmbixFuMaToAmbixMatrix(counts[i], m);
    if (!rows) { Check(false, "expected a matrix"); continue; }
    for (size_t k = 0; k < m.size(); ++k)
    {
      if (m[k] < 0.0)
      {
        char what[96];
        snprintf(what, sizeof(what), "%d-channel matrix has no negative entries",
                 counts[i]);
        Check(false, what);
        break;
      }
    }
  }
}

/* --------------------------------------------------------------------------
 * Reduced sets: the components that are present must route correctly, and the
 * ones that are absent must come out silent rather than picking up a
 * neighbour.
 * ------------------------------------------------------------------------*/
static void TestReducedSets()
{
  /* WXY (horizontal first order) -> 4 channels, Z (ACN2) silent. */
  {
    std::vector<double> m;
    const int rows = AmbixFuMaToAmbixMatrix(3, m);
    Check(rows == 4, "WXY expands to 4 channels");
    if (rows == 4)
    {
      std::vector<double> in(3, 0.0);
      in[1] = 1.0;                       /* X */
      std::vector<double> out = Apply(m, rows, in);
      CheckClose(out[3], 1.0, 1e-6, "WXY: X -> ACN3");
      CheckClose(out[2], 0.0, 1e-6, "WXY: nothing in ACN2");

      in.assign(3, 0.0);
      in[2] = 1.0;                       /* Y */
      out = Apply(m, rows, in);
      CheckClose(out[1], 1.0, 1e-6, "WXY: Y -> ACN1");

      /* ACN2 (Z) is not carried at all: its whole row is zero. */
      double rowSum = 0.0;
      for (int c = 0; c < 3; ++c) rowSum += fabs(m[(size_t)2 * 3 + (size_t)c]);
      CheckClose(rowSum, 0.0, 1e-12, "WXY: ACN2 row is empty");
    }
  }

  /* WXYUV (horizontal second order) -> 9 channels; U and V are FuMa 7 and 8,
   * which are ACN8 and ACN4. R, S, T stay silent. */
  {
    std::vector<double> m;
    const int rows = AmbixFuMaToAmbixMatrix(5, m);
    Check(rows == 9, "WXYUV expands to 9 channels");
    if (rows == 9)
    {
      const double s3_2 = sqrt(3.0) / 2.0;

      std::vector<double> in(5, 0.0);
      in[3] = 1.0;                       /* U */
      std::vector<double> out = Apply(m, rows, in);
      CheckClose(out[8], s3_2, 1e-6, "WXYUV: U -> ACN8 at sqrt(3)/2");

      in.assign(5, 0.0);
      in[4] = 1.0;                       /* V */
      out = Apply(m, rows, in);
      CheckClose(out[4], s3_2, 1e-6, "WXYUV: V -> ACN4 at sqrt(3)/2");

      for (int r = 5; r <= 7; ++r)       /* T, R, S not carried */
      {
        double rowSum = 0.0;
        for (int c = 0; c < 5; ++c) rowSum += fabs(m[(size_t)r * 5 + (size_t)c]);
        char what[64];
        snprintf(what, sizeof(what), "WXYUV: ACN%d row is empty", r);
        CheckClose(rowSum, 0.0, 1e-12, what);
      }
    }
  }

  /* A rejected count leaves the output empty rather than half-built. */
  {
    std::vector<double> m;
    m.push_back(1.0);
    Check(AmbixFuMaToAmbixMatrix(10, m) == 0, "10 channels rejected");
    Check(m.empty(), "rejected count clears the matrix");
  }
}

/* --------------------------------------------------------------------------
 * W alone must survive at sqrt(2): a mono FuMa file is still a valid set, and
 * the sqrt(2) is the one thing libambix does get right, so it is worth
 * pinning down separately.
 * ------------------------------------------------------------------------*/
static void TestMonoW()
{
  std::vector<double> m;
  const int rows = AmbixFuMaToAmbixMatrix(1, m);
  Check(rows == 1, "W alone gives a 1-row matrix");
  if (rows == 1) CheckClose(m[0], sqrt(2.0), 1e-6, "W -> ACN0 at sqrt(2)");
}

int main()
{
  TestChannelCounts();
  TestFirstOrderMatrix();
  TestFirstOrderRouting();
  TestHigherOrderRouting();
  TestNoNegativeEntries();
  TestReducedSets();
  TestMonoW();

  if (g_failures)
  {
    printf("%d FuMa check(s) failed\n", g_failures);
    return 1;
  }
  printf("all FuMa checks passed\n");
  return 0;
}
