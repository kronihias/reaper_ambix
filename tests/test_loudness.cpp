/* ============================================================================
 *  test_loudness.cpp — compliance + regression tests for src/loudness.cpp.
 *
 *  The integrated-loudness cases are the ones from EBU Tech 3341 ("Loudness
 *  Metering: EBU Mode metering to supplement EBU R 128"), which specify a
 *  tolerance of +-0.1 LU for an "EBU Mode" integrated reading. The channel
 *  weighting cases check BS.1770-5 Annex 3 Table 4 and the ambisonic
 *  (W-channel-only) path.
 *
 *  Build:  cmake -DREAPER_AMBIX_BUILD_TESTS=ON && ctest
 * ==========================================================================*/

#include "loudness.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures = 0;
static int g_checks   = 0;

static void CheckClose(const char *what, double got, double expected, double tol)
{
  ++g_checks;
  const bool ok = fabs(got - expected) <= tol;
  if (!ok) ++g_failures;
  printf("%s %-58s got %9.4f  expected %9.4f  (tol %.2f)\n",
         ok ? "  ok  " : "  FAIL", what, got, expected, tol);
}

static void CheckEqualInt(const char *what, int got, int expected)
{
  ++g_checks;
  const bool ok = (got == expected);
  if (!ok) ++g_failures;
  printf("%s %-58s got %9d  expected %9d\n", ok ? "  ok  " : "  FAIL", what, got, expected);
}

/* ---------------------------------------------------------------------------
 * signal generators
 * -------------------------------------------------------------------------*/

/* 1 kHz sine at `dbfs`, written into the channels listed in `chans`. */
static std::vector<double> MakeSine(double sr, double seconds, double dbfs,
                                    int nch, const std::vector<int> &chans,
                                    double freq = 1000.0)
{
  const int frames = (int)(sr * seconds);
  const double amp = pow(10.0, dbfs / 20.0);
  std::vector<double> buf((size_t)frames * nch, 0.0);
  for (int f = 0; f < frames; ++f)
  {
    const double s = amp * sin(2.0 * M_PI * freq * (double)f / sr);
    for (size_t c = 0; c < chans.size(); ++c)
      buf[(size_t)f * nch + (size_t)chans[c]] = s;
  }
  return buf;
}

static std::vector<double> MakeSilence(double sr, double seconds, int nch)
{
  return std::vector<double>((size_t)((int)(sr * seconds)) * nch, 0.0);
}

static double Measure(int nch, double sr, const double *weights,
                      const std::vector<const std::vector<double> *> &segments)
{
  AmbixLoudnessMeter meter(nch, sr, weights);
  for (size_t i = 0; i < segments.size(); ++i)
    meter.AddFrames(&(*segments[i])[0], (int)(segments[i]->size() / nch));
  return meter.GetIntegrated();
}

/* ---------------------------------------------------------------------------
 * EBU Tech 3341 integrated-loudness cases
 * -------------------------------------------------------------------------*/
static void TestEbuIntegrated()
{
  printf("\nEBU Tech 3341 - integrated loudness (tolerance +-0.1 LU)\n");

  double w[AMBIX_LOUDNESS_MAX_CHANNELS];
  bool amb = false;
  const int nch = AmbixLoudnessChannelSetup(2, w, &amb);  /* stereo, G = 1/1 */

  /* Case 1: 1 kHz sine, -23 dBFS, both channels, 20 s -> -23.0 LUFS */
  {
    const std::vector<double> sig = MakeSine(48000, 20, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("case 1: stereo sine -23 dBFS", Measure(nch, 48000, w, segs), -23.0, 0.1);
  }

  /* Case 2: same at -33 dBFS -> -33.0 LUFS */
  {
    const std::vector<double> sig = MakeSine(48000, 20, -33.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("case 2: stereo sine -33 dBFS", Measure(nch, 48000, w, segs), -33.0, 0.1);
  }

  /* Case 3: 10 s of -36 dBFS, 60 s of -23 dBFS, 10 s of -36 dBFS -> -23.0.
   * The quiet head/tail sit 13 LU below the loud part, so the relative gate
   * (-10 LU) must discard them. This is the case that fails outright if the
   * second gating stage is missing. */
  {
    const std::vector<double> quiet = MakeSine(48000, 10, -36.0, 2, std::vector<int>{0, 1});
    const std::vector<double> loud  = MakeSine(48000, 60, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&quiet, &loud, &quiet};
    CheckClose("case 3: -36/-23/-36 dBFS (relative gate)", Measure(nch, 48000, w, segs), -23.0, 0.1);
  }

  /* Case 4: 10 s at -72 dBFS, 10 s at -36, 60 s at -23, 10 s at -36, 10 s at
   * -72 -> -23.0. The -72 dBFS segments fall below the absolute gate. */
  {
    const std::vector<double> vquiet = MakeSine(48000, 10, -72.0, 2, std::vector<int>{0, 1});
    const std::vector<double> quiet  = MakeSine(48000, 10, -36.0, 2, std::vector<int>{0, 1});
    const std::vector<double> loud   = MakeSine(48000, 60, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&vquiet, &quiet, &loud, &quiet, &vquiet};
    CheckClose("case 4: -72/-36/-23/-36/-72 dBFS (absolute gate)",
               Measure(nch, 48000, w, segs), -23.0, 0.1);
  }

  /* Case 5: 20 s at -26 dBFS, 20.1 s at -20, 20 s at -26 -> -23.0 LUFS. */
  {
    const std::vector<double> a = MakeSine(48000, 20.0,  -26.0, 2, std::vector<int>{0, 1});
    const std::vector<double> b = MakeSine(48000, 20.1,  -20.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&a, &b, &a};
    CheckClose("case 5: -26/-20/-26 dBFS", Measure(nch, 48000, w, segs), -23.0, 0.1);
  }
}

/* ---------------------------------------------------------------------------
 * BS.1770 channel weighting
 * -------------------------------------------------------------------------*/
static void TestChannelWeighting()
{
  printf("\nBS.1770-5 Annex 3 channel weighting\n");

  /* A single channel carrying a -23 dBFS 1 kHz sine reads -26.0 LUFS
   * (10*log10 of one G=1.0 channel instead of two). Every multi-channel case
   * below is that reference plus 10*log10(sum of the active G weights). */
  const double oneChannelRef = -26.0;

  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(1, w, &amb);
    const std::vector<double> sig = MakeSine(48000, 20, -23.0, 1, std::vector<int>{0});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("mono, G=1.0", Measure(nch, 48000, w, segs), oneChannelRef, 0.1);
  }

  /* 5.1 in Dolby/SMPTE order: L R C LFE Ls Rs. Feeding every channel except
   * LFE gives 1+1+1+1.41+1.41 = 5.82. If LFE were counted the reading would
   * be ~0.7 LU higher, and if Ls/Rs used G=1.0 it would be ~0.6 LU lower. */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(6, w, &amb);
    CheckEqualInt("5.1: measured channel count", nch, 6);
    CheckClose("5.1: LFE weight is 0", w[3], 0.0, 0.0);
    CheckClose("5.1: Ls weight is 1.41", w[4], 1.41, 0.0);

    const std::vector<double> sig = MakeSine(48000, 20, -23.0, 6, std::vector<int>{0, 1, 2, 3, 4, 5});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("5.1: sine in all channels (LFE excluded)",
               Measure(nch, 48000, w, segs), oneChannelRef + 10.0 * log10(5.82), 0.1);
  }

  /* 7.1.4 (12 ch): only Ls/Rs are the G=1.41 pair; the rear-backs and all four
   * height channels are G=1.0 -> 1+1+1 + 1.41+1.41 + 1+1 + 1+1+1+1 = 11.82. */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(12, w, &amb);
    std::vector<int> all;
    for (int c = 0; c < 12; ++c) all.push_back(c);
    const std::vector<double> sig = MakeSine(48000, 20, -23.0, 12, all);
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("7.1.4: sine in all channels",
               Measure(nch, 48000, w, segs), oneChannelRef + 10.0 * log10(11.82), 0.1);
  }

  /* Only the channels that carry weight are requested. A wide non-ambisonic
   * source is capped at the 12-channel layout, and a trailing unweighted
   * channel is not read at all — this is what keeps the measurement cheap. */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    CheckEqualInt("20 ch: only the 12 weighted channels are read",
                  AmbixLoudnessChannelSetup(20, w, &amb), 12);
    CheckEqualInt("13 ch: only the 12 weighted channels are read",
                  AmbixLoudnessChannelSetup(13, w, &amb), 12);
    CheckEqualInt("6 ch (5.1): all six read, LFE among them",
                  AmbixLoudnessChannelSetup(6, w, &amb), 6);
    CheckEqualInt("14 ch (9.1.4): all fourteen read",
                  AmbixLoudnessChannelSetup(14, w, &amb), 14);
  }

  /* An unweighted channel must not influence the reading no matter what it
   * carries — the meter skips filtering it entirely, so a loud LFE is inert. */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(6, w, &amb);

    const std::vector<double> quietLfe = MakeSine(48000, 20, -23.0, 6, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> a{&quietLfe};
    const double withoutLfe = Measure(nch, 48000, w, a);

    /* same L/R content, plus a full-scale LFE */
    std::vector<double> loudLfe = MakeSine(48000, 20, -23.0, 6, std::vector<int>{0, 1});
    const std::vector<double> lfe = MakeSine(48000, 20, 0.0, 6, std::vector<int>{3});
    for (size_t i = 0; i < loudLfe.size(); ++i) loudLfe[i] += lfe[i];
    std::vector<const std::vector<double> *> b{&loudLfe};
    const double withLfe = Measure(nch, 48000, w, b);

    CheckClose("full-scale LFE does not change the reading", withLfe, withoutLfe, 0.0);
  }

  /* 9.1.4 (14 ch): Lss/Rss AND Ls/Rs are all in the 60-120 deg window ->
   * four G=1.41 channels. 1+1+1 + 4*1.41 + 1+1 + 1+1+1+1 = 14.64. */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(14, w, &amb);
    CheckClose("9.1.4: Lss weight is 1.41", w[4], 1.41, 0.0);
    CheckClose("9.1.4: Rs weight is 1.41",  w[7], 1.41, 0.0);
    CheckClose("9.1.4: Lb weight is 1.0",   w[8], 1.0,  0.0);

    std::vector<int> all;
    for (int c = 0; c < 14; ++c) all.push_back(c);
    const std::vector<double> sig = MakeSine(48000, 20, -23.0, 14, all);
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("9.1.4: sine in all channels",
               Measure(nch, 48000, w, segs), oneChannelRef + 10.0 * log10(14.64), 0.1);
  }
}

/* ---------------------------------------------------------------------------
 * ambisonic detection / W-only measurement
 * -------------------------------------------------------------------------*/
static void TestAmbisonics()
{
  printf("\nAmbisonic layout detection and W-only measurement\n");

  const int ambisonicCounts[]    = { 4, 9, 16, 25, 36, 49, 64 };
  const int nonAmbisonicCounts[] = { 1, 2, 3, 5, 6, 7, 8, 10, 12, 14 };

  for (size_t i = 0; i < sizeof(ambisonicCounts) / sizeof(ambisonicCounts[0]); ++i)
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(ambisonicCounts[i], w, &amb);
    char label[128];
    snprintf(label, sizeof(label), "%d ch -> ambisonic, 1 channel measured", ambisonicCounts[i]);
    CheckEqualInt(label, (amb && nch == 1 && w[0] == 1.0) ? 1 : 0, 1);
  }

  for (size_t i = 0; i < sizeof(nonAmbisonicCounts) / sizeof(nonAmbisonicCounts[0]); ++i)
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(nonAmbisonicCounts[i], w, &amb);
    char label[128];
    snprintf(label, sizeof(label), "%d ch -> not ambisonic", nonAmbisonicCounts[i]);
    CheckEqualInt(label, (!amb && nch == nonAmbisonicCounts[i]) ? 1 : 0, 1);
  }

  /* Third order: only W is measured, so the reading must match a plain mono
   * measurement of W and must NOT change when the other 15 channels carry
   * signal (the caller only ever hands us channel 0). */
  {
    double w[AMBIX_LOUDNESS_MAX_CHANNELS];
    bool amb = false;
    const int nch = AmbixLoudnessChannelSetup(16, w, &amb);
    const std::vector<double> sig = MakeSine(48000, 20, -23.0, nch, std::vector<int>{0});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("third order: W at -23 dBFS reads as mono", Measure(nch, 48000, w, segs), -26.0, 0.1);
  }
}

/* ---------------------------------------------------------------------------
 * sample rate independence, edge cases
 * -------------------------------------------------------------------------*/
static void TestSampleRates()
{
  printf("\nSample-rate independence of the K-weighting filter\n");

  const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
  double w[AMBIX_LOUDNESS_MAX_CHANNELS];
  bool amb = false;
  const int nch = AmbixLoudnessChannelSetup(2, w, &amb);

  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
  {
    const std::vector<double> sig = MakeSine(rates[i], 20, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    char label[128];
    snprintf(label, sizeof(label), "%.0f Hz: stereo sine -23 dBFS", rates[i]);
    CheckClose(label, Measure(nch, rates[i], w, segs), -23.0, 0.1);
  }
}

static void TestEdgeCases()
{
  printf("\nEdge cases\n");

  double w[AMBIX_LOUDNESS_MAX_CHANNELS];
  bool amb = false;
  const int nch = AmbixLoudnessChannelSetup(2, w, &amb);

  {
    const std::vector<double> sig = MakeSilence(48000, 5, 2);
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("digital silence -> -inf", Measure(nch, 48000, w, segs),
               AMBIX_LOUDNESS_NEGATIVE_INF, 0.0);
  }

  {
    /* Shorter than one 400 ms gating block: nothing to report. */
    const std::vector<double> sig = MakeSine(48000, 0.2, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("200 ms of audio -> -inf", Measure(nch, 48000, w, segs),
               AMBIX_LOUDNESS_NEGATIVE_INF, 0.0);
  }

  {
    /* Everything below the -70 LUFS absolute gate must be discarded. */
    const std::vector<double> sig = MakeSine(48000, 20, -85.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    CheckClose("-85 dBFS sine (below absolute gate) -> -inf",
               Measure(nch, 48000, w, segs), AMBIX_LOUDNESS_NEGATIVE_INF, 0.0);
  }

  {
    /* Feeding the same signal in many small chunks must give the same answer
     * as one big call — i.e. the filter state and sub-block accumulator carry
     * across AddFrames() boundaries. */
    const std::vector<double> sig = MakeSine(48000, 10, -23.0, 2, std::vector<int>{0, 1});
    std::vector<const std::vector<double> *> segs{&sig};
    const double whole = Measure(nch, 48000, w, segs);

    AmbixLoudnessMeter meter(nch, 48000, w);
    const int totalFrames = (int)(sig.size() / nch);
    int done = 0;
    int chunk = 1;
    while (done < totalFrames)
    {
      int n = chunk;
      if (done + n > totalFrames) n = totalFrames - done;
      meter.AddFrames(&sig[(size_t)done * nch], n);
      done += n;
      chunk = (chunk * 3) % 977 + 1;  /* irregular, never block-aligned */
    }
    CheckClose("chunked input matches single-call input", meter.GetIntegrated(), whole, 0.001);
  }
}

/* ---------------------------------------------------------------------------
 * K-weighting: coefficients and frequency response
 * -------------------------------------------------------------------------*/

/* BS.1770-4 Table 1 — stage 1 (high shelf), specified at 48 kHz. */
static const double kTable1_b[3] = {  1.53512485958697, -2.69169618940638,  1.19839281085285 };
static const double kTable1_a[3] = {  1.0,              -1.69065929318241,  0.73248077421585 };
/* BS.1770-4 Table 2 — stage 2 (RLB high pass), specified at 48 kHz. */
static const double kTable2_b[3] = {  1.0,              -2.0,               1.0              };
static const double kTable2_a[3] = {  1.0,              -1.99004745483398,  0.99007225036621 };

/* loudness.cpp derives the coefficients from the analog prototype so the filter
 * stays correct at 44.1/96/192 kHz. At 48 kHz that derivation must reproduce
 * the values the recommendation tabulates — this is the check that the filter
 * really is BS.1770 K-weighting and not something that merely looks like it. */
static void TestKWeightingCoefficients()
{
  printf("\nK-weighting coefficients vs BS.1770-4 Tables 1 & 2 (48 kHz)\n");

  /* Cascade the two published biquads into the order-4 form loudness.cpp uses. */
  double expB[5], expA[5];
  expB[0] = kTable1_b[0] * kTable2_b[0];
  expB[1] = kTable1_b[0] * kTable2_b[1] + kTable1_b[1] * kTable2_b[0];
  expB[2] = kTable1_b[0] * kTable2_b[2] + kTable1_b[1] * kTable2_b[1] + kTable1_b[2] * kTable2_b[0];
  expB[3] = kTable1_b[1] * kTable2_b[2] + kTable1_b[2] * kTable2_b[1];
  expB[4] = kTable1_b[2] * kTable2_b[2];
  expA[0] = kTable1_a[0] * kTable2_a[0];
  expA[1] = kTable1_a[0] * kTable2_a[1] + kTable1_a[1] * kTable2_a[0];
  expA[2] = kTable1_a[0] * kTable2_a[2] + kTable1_a[1] * kTable2_a[1] + kTable1_a[2] * kTable2_a[0];
  expA[3] = kTable1_a[1] * kTable2_a[2] + kTable1_a[2] * kTable2_a[1];
  expA[4] = kTable1_a[2] * kTable2_a[2];

  double b[5], a[5];
  AmbixLoudnessKWeighting(48000.0, b, a);

  for (int i = 0; i < 5; ++i)
  {
    char label[128];
    snprintf(label, sizeof(label), "b[%d]", i);
    CheckClose(label, b[i], expB[i], 1e-9);
  }
  for (int i = 0; i < 5; ++i)
  {
    char label[128];
    snprintf(label, sizeof(label), "a[%d]", i);
    CheckClose(label, a[i], expA[i], 1e-9);
  }
}

/* |H(e^jw)| of the order-4 K-weighting filter, in dB. */
static double ResponseDb(const double *b, const double *a, double freq, double sr)
{
  double numRe = 0.0, numIm = 0.0, denRe = 0.0, denIm = 0.0;
  for (int k = 0; k < 5; ++k)
  {
    const double phase = -2.0 * M_PI * freq * (double)k / sr;
    numRe += b[k] * cos(phase);  numIm += b[k] * sin(phase);
    denRe += a[k] * cos(phase);  denIm += a[k] * sin(phase);
  }
  const double num = sqrt(numRe * numRe + numIm * numIm);
  const double den = sqrt(denRe * denRe + denIm * denIm);
  return 20.0 * log10(num / den);
}

/* End-to-end check: run sines through the whole meter and confirm the level
 * differences follow the K-weighting curve computed from the published 48 kHz
 * coefficients. This catches mistakes in the filter *implementation* (state
 * handling, sign of the feedback terms) that a coefficient check cannot. */
static void TestKWeightingResponse()
{
  printf("\nK-weighting response through the meter, relative to 1 kHz\n");

  double expB[5], expA[5];
  {
    /* same cascade as above, from the published tables */
    expB[0] = kTable1_b[0] * kTable2_b[0];
    expB[1] = kTable1_b[0] * kTable2_b[1] + kTable1_b[1] * kTable2_b[0];
    expB[2] = kTable1_b[0] * kTable2_b[2] + kTable1_b[1] * kTable2_b[1] + kTable1_b[2] * kTable2_b[0];
    expB[3] = kTable1_b[1] * kTable2_b[2] + kTable1_b[2] * kTable2_b[1];
    expB[4] = kTable1_b[2] * kTable2_b[2];
    expA[0] = kTable1_a[0] * kTable2_a[0];
    expA[1] = kTable1_a[0] * kTable2_a[1] + kTable1_a[1] * kTable2_a[0];
    expA[2] = kTable1_a[0] * kTable2_a[2] + kTable1_a[1] * kTable2_a[1] + kTable1_a[2] * kTable2_a[0];
    expA[3] = kTable1_a[1] * kTable2_a[2] + kTable1_a[2] * kTable2_a[1];
    expA[4] = kTable1_a[2] * kTable2_a[2];
  }

  double w[AMBIX_LOUDNESS_MAX_CHANNELS];
  bool amb = false;
  const int nch = AmbixLoudnessChannelSetup(1, w, &amb);

  const std::vector<double> ref = MakeSine(48000, 10, -20.0, 1, std::vector<int>{0}, 1000.0);
  std::vector<const std::vector<double> *> refSegs{&ref};
  const double meterRef1k = Measure(nch, 48000, w, refSegs);
  const double theoryRef1k = ResponseDb(expB, expA, 1000.0, 48000.0);

  const double freqs[] = { 20.0, 50.0, 100.0, 200.0, 500.0, 2000.0, 5000.0, 10000.0 };
  for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); ++i)
  {
    const std::vector<double> sig = MakeSine(48000, 10, -20.0, 1, std::vector<int>{0}, freqs[i]);
    std::vector<const std::vector<double> *> segs{&sig};

    const double measured = Measure(nch, 48000, w, segs) - meterRef1k;
    const double expected = ResponseDb(expB, expA, freqs[i], 48000.0) - theoryRef1k;

    char label[128];
    snprintf(label, sizeof(label), "gain at %5.0f Hz", freqs[i]);
    CheckClose(label, measured, expected, 0.05);
  }
}

int main()
{
  printf("reaper_ambix — loudness (ITU-R BS.1770 / EBU R128) test suite\n");

  TestEbuIntegrated();
  TestChannelWeighting();
  TestAmbisonics();
  TestSampleRates();
  TestEdgeCases();
  TestKWeightingCoefficients();
  TestKWeightingResponse();

  printf("\n%d checks, %d failure%s\n", g_checks, g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
