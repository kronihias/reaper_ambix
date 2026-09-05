#include "fuma.h"

#include <cmath>
#include <cstring>

/* ---------------------------------------------------------------------------
 * FuMa channel order is W X Y Z R S T U V K L M N O P Q, i.e. per (n, m):
 *
 *   W (0, 0)
 *   X (1, 1)   Y (1,-1)   Z (1, 0)
 *   R (2, 0)   S (2, 1)   T (2,-1)   U (2, 2)   V (2,-2)
 *   K (3, 0)   L (3, 1)   M (3,-1)   N (3, 2)   O (3,-2)   P (3, 3)  Q (3,-3)
 *
 * ambiX numbers the same harmonics ACN = n^2 + n + m (paper eq. 2), which
 * gives the permutation below: entry r is the FuMa index that supplies ambiX
 * channel r.
 *
 *   ACN  0 = W     ACN  4 = V     ACN  9 = Q     ACN 13 = L
 *   ACN  1 = Y     ACN  5 = T     ACN 10 = O     ACN 14 = N
 *   ACN  2 = Z     ACN  6 = R     ACN 11 = M     ACN 15 = P
 *   ACN  3 = X     ACN  7 = S     ACN 12 = K
 *                  ACN  8 = U
 * -------------------------------------------------------------------------*/
static const int kAcnToFuMa[AMBIX_FUMA_MAX_CHANNELS] =
{
  0,              /* W */
  2, 3, 1,        /* Y Z X */
  8, 6, 4, 5, 7,  /* V T R S U */
  15, 13, 11, 9, 10, 12, 14   /* Q O M K L N P */
};

/* maxN -> SN3D gains, indexed by ACN.
 *
 * FuMa normalizes each harmonic to a peak of 1 (maxN) and additionally carries
 * W at 1/sqrt(2); SN3D does neither, so the gain is sqrt(2) for W and
 * 1 / max|Y_n^m| for the rest. The first-order harmonics already peak at 1, so
 * only orders 2 and 3 need scaling. Written out rather than derived because
 * these are the numbers every FuMa conversion table quotes, and a reader can
 * check them at a glance:
 *
 *   n=2, |m|=1,2 : sqrt(3)/2      ~ 0.86603
 *   n=3, |m|=1   : 4*sqrt(2/5)/3  ~ 0.84327
 *   n=3, |m|=2   : sqrt(5)/3      ~ 0.74536
 *   n=3, |m|=3   : sqrt(5/2)/2    ~ 0.79057
 *
 * Every gain is positive: the ambiX paper notes that the Ambisonics community
 * agreed not to use the Condon-Shortley phase, and its B-format conversion
 * matrix (section 4.2.1) has no negative entries. */
static void FuMaGains(double *g /*[16]*/)
{
  const double s3_2   = sqrt(3.0) / 2.0;
  const double s32_45 = 4.0 * sqrt(2.0 / 5.0) / 3.0;
  const double s5_9   = sqrt(5.0) / 3.0;
  const double s5_8   = sqrt(5.0 / 2.0) / 2.0;

  g[0]  = sqrt(2.0);                       /* ACN0  W       */
  g[1]  = g[2] = g[3] = 1.0;               /* ACN1-3  Y Z X */
  g[4]  = s3_2;                            /* ACN4  V  |m|=2 */
  g[5]  = s3_2;                            /* ACN5  T  |m|=1 */
  g[6]  = 1.0;                             /* ACN6  R   m=0  */
  g[7]  = s3_2;                            /* ACN7  S  |m|=1 */
  g[8]  = s3_2;                            /* ACN8  U  |m|=2 */
  g[9]  = s5_8;                            /* ACN9  Q  |m|=3 */
  g[10] = s5_9;                            /* ACN10 O  |m|=2 */
  g[11] = s32_45;                          /* ACN11 M  |m|=1 */
  g[12] = 1.0;                             /* ACN12 K   m=0  */
  g[13] = s32_45;                          /* ACN13 L  |m|=1 */
  g[14] = s5_9;                            /* ACN14 N  |m|=2 */
  g[15] = s5_8;                            /* ACN15 P  |m|=3 */
}

/* ---------------------------------------------------------------------------
 * Reduced FuMa sets.
 *
 * A FuMa file need not carry every component: horizontal-only and mixed-order
 * layouts drop the ones that stay silent. These are the sets FuMa defines, as
 * the FuMa indices present, in the order they appear in the file. Anything not
 * listed here is not a FuMa channel count.
 * -------------------------------------------------------------------------*/
struct FuMaSet
{
  int channels;
  int target;                              /* (N+1)^2 of the expanded set */
  int index[AMBIX_FUMA_MAX_CHANNELS];      /* FuMa index of each file channel */
};

static const FuMaSet kFuMaSets[] =
{
  {  1,  1, { 0 } },                                       /* W               */
  {  3,  4, { 0, 1, 2 } },                                 /* WXY             */
  {  4,  4, { 0, 1, 2, 3 } },                              /* WXYZ            */
  {  5,  9, { 0, 1, 2, 7, 8 } },                           /* WXYUV           */
  {  6,  9, { 0, 1, 2, 3, 7, 8 } },                        /* WXYZUV          */
  {  7, 16, { 0, 1, 2, 7, 8, 14, 15 } },                   /* WXYUVPQ         */
  {  8, 16, { 0, 1, 2, 3, 7, 8, 14, 15 } },                /* WXYZUVPQ        */
  {  9,  9, { 0, 1, 2, 3, 4, 5, 6, 7, 8 } },               /* WXYZRSTUV       */
  { 11, 16, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 14, 15 } },       /* WXYZRSTUVPQ     */
  { 16, 16, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } },
};

static const int kNumFuMaSets = (int)(sizeof(kFuMaSets) / sizeof(kFuMaSets[0]));

static const FuMaSet *FindSet(int fumaChannels)
{
  for (int i = 0; i < kNumFuMaSets; ++i)
    if (kFuMaSets[i].channels == fumaChannels) return &kFuMaSets[i];
  return NULL;
}

int AmbixFuMaTargetChannels(int fumaChannels)
{
  const FuMaSet *set = FindSet(fumaChannels);
  return set ? set->target : 0;
}

int AmbixFuMaToAmbixMatrix(int fumaChannels, std::vector<double> &matrixOut)
{
  matrixOut.clear();

  const FuMaSet *set = FindSet(fumaChannels);
  if (!set) return 0;

  double gains[AMBIX_FUMA_MAX_CHANNELS];
  FuMaGains(gains);

  matrixOut.assign((size_t)set->target * (size_t)fumaChannels, 0.0);

  /* One non-zero per row at most: ambiX channel r is fed by whichever file
   * channel carries FuMa component kAcnToFuMa[r]. A reduced set leaves the
   * rows it does not carry at zero, which is exactly the silent component. */
  for (int r = 0; r < set->target; ++r)
  {
    const int wanted = kAcnToFuMa[r];
    for (int c = 0; c < fumaChannels; ++c)
    {
      if (set->index[c] == wanted)
      {
        matrixOut[(size_t)r * (size_t)fumaChannels + (size_t)c] = gains[r];
        break;
      }
    }
  }

  return set->target;
}
