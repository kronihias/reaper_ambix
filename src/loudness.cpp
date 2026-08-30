#include "loudness.h"

#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * K-weighting (BS.1770-4)
 *
 * Stage 1: high-shelf, Stage 2: RLB high-pass. The recommendation tabulates
 * the coefficients for 48 kHz only; deriving them from the analog prototype
 * (same constants libebur128 uses) keeps the filter correct at 44.1/96/192 kHz.
 * The two biquads are convolved into a single order-4 section.
 * -------------------------------------------------------------------------*/
void AmbixLoudnessKWeighting(double samplerate, double *b /*[5]*/, double *a /*[5]*/)
{
  /* -- stage 1: high shelf -- */
  const double f0_s = 1681.974450955533;
  const double G_s  = 3.999843853973347;
  const double Q_s  = 0.7071752369554196;

  const double K_s  = tan(M_PI * f0_s / samplerate);
  const double Vh   = pow(10.0, G_s / 20.0);
  const double Vb   = pow(Vh, 0.4996667741545416);
  const double a0_s = 1.0 + K_s / Q_s + K_s * K_s;

  double pb[3], pa[3];
  pb[0] = (Vh + Vb * K_s / Q_s + K_s * K_s) / a0_s;
  pb[1] = 2.0 * (K_s * K_s - Vh) / a0_s;
  pb[2] = (Vh - Vb * K_s / Q_s + K_s * K_s) / a0_s;
  pa[0] = 1.0;
  pa[1] = 2.0 * (K_s * K_s - 1.0) / a0_s;
  pa[2] = (1.0 - K_s / Q_s + K_s * K_s) / a0_s;

  /* -- stage 2: RLB high pass -- */
  const double f0_h = 38.13547087602444;
  const double Q_h  = 0.5003270373238773;
  const double K_h  = tan(M_PI * f0_h / samplerate);
  const double a0_h = 1.0 + K_h / Q_h + K_h * K_h;

  double rb[3], ra[3];
  rb[0] =  1.0;
  rb[1] = -2.0;
  rb[2] =  1.0;
  ra[0] =  1.0;
  ra[1] = 2.0 * (K_h * K_h - 1.0) / a0_h;
  ra[2] = (1.0 - K_h / Q_h + K_h * K_h) / a0_h;

  /* -- cascade (polynomial multiply) -- */
  b[0] = pb[0] * rb[0];
  b[1] = pb[0] * rb[1] + pb[1] * rb[0];
  b[2] = pb[0] * rb[2] + pb[1] * rb[1] + pb[2] * rb[0];
  b[3] = pb[1] * rb[2] + pb[2] * rb[1];
  b[4] = pb[2] * rb[2];

  a[0] = pa[0] * ra[0];
  a[1] = pa[0] * ra[1] + pa[1] * ra[0];
  a[2] = pa[0] * ra[2] + pa[1] * ra[1] + pa[2] * ra[0];
  a[3] = pa[1] * ra[2] + pa[2] * ra[1];
  a[4] = pa[2] * ra[2];
}

AmbixLoudnessMeter::AmbixLoudnessMeter(int channels, double samplerate,
                                       const double *weights)
{
  if (channels < 1) channels = 1;
  if (channels > AMBIX_LOUDNESS_MAX_CHANNELS) channels = AMBIX_LOUDNESS_MAX_CHANNELS;
  m_channels = channels;

  for (int c = 0; c < AMBIX_LOUDNESS_MAX_CHANNELS; ++c)
    m_weights[c] = (weights && c < channels) ? weights[c] : 0.0;

  AmbixLoudnessKWeighting(samplerate, m_b, m_a);

  m_hist.assign((size_t)m_channels * 8, 0.0);

  /* 100 ms hop; 4 hops make one 400 ms gating block (75 % overlap). */
  m_subblockFrames = (int)(samplerate / 10.0 + 0.5);
  if (m_subblockFrames < 1) m_subblockFrames = 1;
  m_subblockFilled = 0;
  m_subblockSum.assign((size_t)m_channels, 0.0);
}

void AmbixLoudnessMeter::AddFrames(const double *interleaved, int frames)
{
  if (!interleaved || frames <= 0) return;
  FilterAndAccumulate(interleaved, frames);
}

void AmbixLoudnessMeter::FilterAndAccumulate(const double *interleaved, int frames)
{
  const int nch = m_channels;
  double *hist = &m_hist[0];

  for (int f = 0; f < frames; ++f)
  {
    const double *in = interleaved + (size_t)f * nch;

    for (int c = 0; c < nch; ++c)
    {
      double *h = hist + (size_t)c * 8;  /* x1 x2 x3 x4 y1 y2 y3 y4 */

      const double x = in[c];
      double y = m_b[0] * x
               + m_b[1] * h[0] + m_b[2] * h[1] + m_b[3] * h[2] + m_b[4] * h[3]
               - m_a[1] * h[4] - m_a[2] * h[5] - m_a[3] * h[6] - m_a[4] * h[7];

      /* Denormals cost more than the branch on long silent passages. */
      if (!(fabs(y) > 1e-30)) y = 0.0;

      h[3] = h[2]; h[2] = h[1]; h[1] = h[0]; h[0] = x;
      h[7] = h[6]; h[6] = h[5]; h[5] = h[4]; h[4] = y;

      m_subblockSum[c] += y * y;
    }

    if (++m_subblockFilled >= m_subblockFrames)
    {
      m_subblocks.insert(m_subblocks.end(), m_subblockSum.begin(), m_subblockSum.end());
      std::fill(m_subblockSum.begin(), m_subblockSum.end(), 0.0);
      m_subblockFilled = 0;
    }
  }
}

double AmbixLoudnessMeter::GetIntegrated() const
{
  const int nch     = m_channels;
  const size_t nsub = m_subblocks.size() / (size_t)nch;
  if (nsub < 4) return AMBIX_LOUDNESS_NEGATIVE_INF;  /* < 400 ms of audio */

  const size_t nblocks = nsub - 3;                   /* 400 ms, 100 ms hop */
  const double blockFrames = (double)m_subblockFrames * 4.0;

  /* Mean square per gating block per channel, and the block loudness. */
  std::vector<double> blockMS((size_t)nblocks * nch);
  std::vector<double> blockLoudness(nblocks);

  for (size_t j = 0; j < nblocks; ++j)
  {
    double weighted = 0.0;
    for (int c = 0; c < nch; ++c)
    {
      double sum = 0.0;
      for (size_t k = j; k < j + 4; ++k)
        sum += m_subblocks[k * (size_t)nch + (size_t)c];

      const double ms = sum / blockFrames;
      blockMS[j * (size_t)nch + (size_t)c] = ms;
      weighted += m_weights[c] * ms;
    }
    blockLoudness[j] = (weighted > 0.0)
                     ? (-0.691 + 10.0 * log10(weighted))
                     : AMBIX_LOUDNESS_NEGATIVE_INF;
  }

  /* --- gate 1: absolute, -70 LUFS --- */
  const double absoluteGate = -70.0;
  std::vector<double> meanMS((size_t)nch, 0.0);
  size_t counted = 0;

  for (size_t j = 0; j < nblocks; ++j)
  {
    if (blockLoudness[j] <= absoluteGate) continue;
    for (int c = 0; c < nch; ++c)
      meanMS[c] += blockMS[j * (size_t)nch + (size_t)c];
    ++counted;
  }
  if (!counted) return AMBIX_LOUDNESS_NEGATIVE_INF;

  double weighted = 0.0;
  for (int c = 0; c < nch; ++c)
    weighted += m_weights[c] * (meanMS[c] / (double)counted);
  if (!(weighted > 0.0)) return AMBIX_LOUDNESS_NEGATIVE_INF;

  /* --- gate 2: relative, gated mean - 10 LU --- */
  const double relativeGate = -0.691 + 10.0 * log10(weighted) - 10.0;

  std::fill(meanMS.begin(), meanMS.end(), 0.0);
  counted = 0;
  for (size_t j = 0; j < nblocks; ++j)
  {
    if (blockLoudness[j] <= absoluteGate || blockLoudness[j] <= relativeGate) continue;
    for (int c = 0; c < nch; ++c)
      meanMS[c] += blockMS[j * (size_t)nch + (size_t)c];
    ++counted;
  }
  if (!counted) return AMBIX_LOUDNESS_NEGATIVE_INF;

  weighted = 0.0;
  for (int c = 0; c < nch; ++c)
    weighted += m_weights[c] * (meanMS[c] / (double)counted);
  if (!(weighted > 0.0)) return AMBIX_LOUDNESS_NEGATIVE_INF;

  return -0.691 + 10.0 * log10(weighted);
}

/* ---------------------------------------------------------------------------
 * Channel layout / weighting
 * -------------------------------------------------------------------------*/
int AmbixLoudnessChannelSetup(int sourceChannels, double *weightsOut,
                              bool *isAmbisonicsOut)
{
  for (int c = 0; c < AMBIX_LOUDNESS_MAX_CHANNELS; ++c)
    weightsOut[c] = 0.0;
  if (isAmbisonicsOut) *isAmbisonicsOut = false;

  if (sourceChannels < 1) sourceChannels = 1;

  /* Ambisonic channel counts are perfect squares: (N+1)^2 — FOA=4, SOA=9,
   * TOA=16, 4th=25 ... 10th=121. The standard surround counts (6, 8, 10, 12,
   * 14) are not perfect squares, so there is no collision. 16 ch is read as
   * third order rather than 9.1.6. lround() avoids a floating-point rounding
   * miss at large counts. */
  const int root = (int)lround(sqrt((double)sourceChannels));
  if (sourceChannels >= 4 && root * root == sourceChannels)
  {
    /* Measure W only, G = 1.00. */
    weightsOut[0] = 1.0;
    if (isAmbisonicsOut) *isAmbisonicsOut = true;
    return 1;
  }

  int nch = sourceChannels;
  if (nch > AMBIX_LOUDNESS_MAX_CHANNELS) nch = AMBIX_LOUDNESS_MAX_CHANNELS;

  if (nch == 14)
  {
    /* 9.1.4, Dolby/SMPTE order:
     *   L R C LFE Lss Rss Ls Rs Lb Rb Ltf Rtf Ltb Rtb
     * Lss/Rss (~+-60 deg) and Ls/Rs (~+-110 deg) are ear-level inside the
     * 60-120 deg azimuth window -> G = 1.41 (BS.1770-5 Annex 3 Table 4).
     * Lb/Rb sit beyond 120 deg -> G = 1.00, and every height channel
     * (elevation >= 30 deg) is G = 1.00 regardless of azimuth. */
    static const double w914[14] = {
      1.0, 1.0, 1.0, 0.0,      /* L R C LFE   */
      1.41, 1.41, 1.41, 1.41,  /* Lss Rss Ls Rs */
      1.0, 1.0,                /* Lb Rb       */
      1.0, 1.0, 1.0, 1.0       /* Ltf Rtf Ltb Rtb */
    };
    memcpy(weightsOut, w914, sizeof(w914));
    return nch;
  }

  /* Dolby/SMPTE order up to 7.1.4 (12 ch):
   *   L R C LFE Ls Rs Lb Rb Ltf Rtf Ltb Rtb
   * 10 ch is treated as 7.1.2; 9.1 has the same count and cannot be told
   * apart without file metadata. Channels past 12 stay unweighted. */
  static const double wSurround[12] = {
    1.0, 1.0, 1.0, 0.0,       /* L R C LFE   */
    1.41, 1.41,               /* Ls Rs       */
    1.0, 1.0,                 /* Lb Rb       */
    1.0, 1.0, 1.0, 1.0        /* Ltf Rtf Ltb Rtb */
  };
  for (int c = 0; c < nch && c < 12; ++c)
    weightsOut[c] = wSurround[c];

  return nch;
}
