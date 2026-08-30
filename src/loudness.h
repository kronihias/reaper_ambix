/* ============================================================================
 *  loudness.h — self-contained ITU-R BS.1770-4 / EBU R128 integrated loudness.
 *
 *  Deliberately dependency-free (plain C++ + <cmath>/<vector>) so reaper_ambix
 *  keeps its "everything is vendored or local" property: no libebur128, no SWS,
 *  nothing to link against.
 *
 *  What is implemented:
 *    - K-weighting (BS.1770-4 Tables 1 & 2), coefficients derived analytically
 *      for the actual sample rate rather than the tabulated 48 kHz values
 *    - 400 ms blocks with 75 % overlap (100 ms hop)
 *    - two-stage gating: absolute -70 LUFS, then relative (gated mean - 10 LU)
 *    - per-channel weights G (BS.1770-4 Table 4)
 *
 *  Not implemented (not needed for volume normalization): momentary/short-term
 *  readouts, loudness range, true peak.
 * ==========================================================================*/

#ifndef REAPER_AMBIX_LOUDNESS_H
#define REAPER_AMBIX_LOUDNESS_H

#include <vector>

/* Returned by GetIntegrated() when no block passed the absolute gate, i.e. the
 * material is silence (or shorter than one 400 ms block). */
#define AMBIX_LOUDNESS_NEGATIVE_INF (-150.0)

/* Maximum number of channels we ever build a weight table for. Anything wider
 * is measured with the leading channels only (see AmbixLoudnessChannelSetup). */
#define AMBIX_LOUDNESS_MAX_CHANNELS 64

class AmbixLoudnessMeter
{
public:
  /* weights[] must hold `channels` entries (BS.1770-4 Table 4 G values;
   * 0.0 excludes a channel, e.g. LFE). */
  AmbixLoudnessMeter(int channels, double samplerate, const double *weights);

  /* `interleaved` holds frames * channels samples. May be called repeatedly. */
  void AddFrames(const double *interleaved, int frames);

  /* Integrated loudness in LUFS, or AMBIX_LOUDNESS_NEGATIVE_INF for silence. */
  double GetIntegrated() const;

private:
  void FilterAndAccumulate(const double *interleaved, int frames);

  int    m_channels;
  double m_weights[AMBIX_LOUDNESS_MAX_CHANNELS];

  /* K-weighting: two cascaded biquads flattened into one order-4 IIR. */
  double m_b[5];
  double m_a[5];               /* a[0] == 1 */
  std::vector<double> m_hist;  /* per channel: x[-1..-4] then y[-1..-4] */

  /* Sum of squares of the K-weighted signal over the current 100 ms sub-block,
   * one entry per channel. A 400 ms gating block is the sum of 4 sub-blocks. */
  int                 m_subblockFrames;   /* 100 ms worth of frames */
  int                 m_subblockFilled;
  std::vector<double> m_subblockSum;
  /* Completed sub-blocks, channel-major-per-subblock (size = n * m_channels) */
  std::vector<double> m_subblocks;
};

/* Pick the measurement channel layout for a source with `sourceChannels`
 * channels, mirroring the BS.1770-5 Annex 3 weighting table.
 *
 *   - ambisonics (channel count is a perfect square >= 4): only W (channel 0)
 *     is measured, with G = 1.0. Per Peters & Epain (AES 154th, 2023) applying
 *     BS.1770 to W alone matches a full loudspeaker rendering, and it avoids
 *     ingesting all HOA channels (16x less data for third order).
 *   - 14 channels: 9.1.4 in Dolby/SMPTE order.
 *   - everything else: Dolby/SMPTE order up to 7.1.4 (12 ch).
 *
 * Fills weightsOut (must hold at least AMBIX_LOUDNESS_MAX_CHANNELS entries) and
 * returns how many leading channels have to be requested from the audio
 * accessor: 1 for ambisonics, otherwise one past the last channel that carries
 * a non-zero weight. Trailing channels that BS.1770 does not weight are never
 * requested and never filtered — a 20-channel non-ambisonic source only costs
 * 12 channels of work, and a 36-channel fifth-order bed only costs one. */
int AmbixLoudnessChannelSetup(int sourceChannels, double *weightsOut,
                              bool *isAmbisonicsOut);

/* The K-weighting filter used internally, exposed so the test suite can check
 * the derived coefficients against the values tabulated in BS.1770-4 Tables 1
 * and 2. Both arrays hold 5 taps (the two biquads cascaded); a[0] is 1. */
void AmbixLoudnessKWeighting(double samplerate, double *b /*[5]*/, double *a /*[5]*/);

#endif /* REAPER_AMBIX_LOUDNESS_H */
