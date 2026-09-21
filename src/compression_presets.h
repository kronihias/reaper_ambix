/* ============================================================================
 *  compression_presets.h — the container/compression choices offered by both
 *  the render dialog (pcmsink_ambix.cpp) and the "Convert item(s) to .ambix"
 *  dialog (convert_actions.cpp).
 *
 *  Shared so the two cannot drift: a rate that appears in one place and not
 *  the other is exactly the kind of difference nobody notices until a file
 *  comes out bigger, or noisier, than the same settings produced elsewhere.
 *
 *  Item data, as stored in the combobox:
 *    -1  uncompressed CAF
 *     0  WavPack lossless
 *    >0  WavPack hybrid (lossy) at data/100 bits per sample and channel
 *
 *  The lossy rates are WavPack's bits-per-sample form; the noise floor sits
 *  about 6 dB per bit below each channel's own level, which is what makes the
 *  mode usable for ambisonic beds where the higher orders are quiet.
 * ==========================================================================*/

#ifndef REAPER_AMBIX_COMPRESSION_PRESETS_H
#define REAPER_AMBIX_COMPRESSION_PRESETS_H

struct AmbixCompressionPreset { const char *label; int data; };

static const AmbixCompressionPreset kCompressionPresets[] = {
  { "CAF uncompressed",             -1  },
  { "WavPack lossless",              0  },
  { "WavPack lossy, 6 bit/sample",   600 },
  { "WavPack lossy, 4 bit/sample",   400 },
  { "WavPack lossy, 3 bit/sample",   300 },
};

static const int kNumCompressionPresets =
  (int)(sizeof(kCompressionPresets) / sizeof(kCompressionPresets[0]));

/* The two halves of the encoding, so callers do not open-code /100 and the
 * -1 sentinel. `bits` is WavPack's bits-per-sample, 0 for lossless. */
static inline bool AmbixCompressionUsesWavpack(int data) { return data >= 0; }
static inline float AmbixCompressionBits(int data)
{
  return data > 0 ? (float)data / 100.f : 0.f;
}
static inline int AmbixCompressionData(bool wavpack, float bits)
{
  if (!wavpack)  return -1;
  if (bits <= 0) return 0;
  return (int)(bits * 100.f + 0.5f);
}

#endif /* REAPER_AMBIX_COMPRESSION_PRESETS_H */
