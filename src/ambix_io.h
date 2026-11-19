/* Shared bits for reading and writing ambiX files through ambixfile. */
#ifndef REAPER_AMBIX_IO_H
#define REAPER_AMBIX_IO_H

#include <ambixfile/ambixfile.hpp>

#include <thread>

/* Values stored in the render sink's settings (and so in project files and
 * render presets). They must keep these numbers. */
enum
{
  AMBIX_CFG_FORMAT_NONE     = 0,
  AMBIX_CFG_FORMAT_BASIC    = 1,
  AMBIX_CFG_FORMAT_EXTENDED = 2
};
enum
{
  AMBIX_CFG_SAMPLEFORMAT_NONE    = 0,
  AMBIX_CFG_SAMPLEFORMAT_PCM16   = 1,
  AMBIX_CFG_SAMPLEFORMAT_PCM24   = 2,
  AMBIX_CFG_SAMPLEFORMAT_PCM32   = 3,
  AMBIX_CFG_SAMPLEFORMAT_FLOAT32 = 4,
  AMBIX_CFG_SAMPLEFORMAT_FLOAT64 = 5
};

static inline ambixfile::SampleFormat AmbixSampleFormatFromCfg(int cfg)
{
  switch (cfg)
  {
    case AMBIX_CFG_SAMPLEFORMAT_PCM16:   return ambixfile::SampleFormat::Int16;
    case AMBIX_CFG_SAMPLEFORMAT_PCM32:   return ambixfile::SampleFormat::Int32;
    case AMBIX_CFG_SAMPLEFORMAT_FLOAT32: return ambixfile::SampleFormat::Float32;
    case AMBIX_CFG_SAMPLEFORMAT_FLOAT64: return ambixfile::SampleFormat::Float64;
    case AMBIX_CFG_SAMPLEFORMAT_PCM24:
    default:                             return ambixfile::SampleFormat::Int24;
  }
}

/* Worker threads for the WavPack codec: none on a single core, otherwise the
 * core count up to 4. WavPack uses one per channel pair; taking every core
 * would starve the audio threads, and more than 4 gains little. */
static inline int AmbixWavPackThreads()
{
  const unsigned cores = std::thread::hardware_concurrency();
  if (cores <= 1) return 0;
  return cores > 4 ? 4 : (int)cores;
}

#endif /* REAPER_AMBIX_IO_H */
