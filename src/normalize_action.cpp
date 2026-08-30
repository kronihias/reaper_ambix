/* ============================================================================
 *  normalize_action.cpp — "ambiX: Normalize selected item(s) ... " action.
 *
 *  Measures each selected item's active take with the local BS.1770-4
 *  implementation in loudness.cpp and scales the take volume so the integrated
 *  loudness lands on the user's target.
 *
 *  Ambisonic sources (channel count a perfect square >= 4) are measured on the
 *  W channel alone — see AmbixLoudnessChannelSetup() — which is both the
 *  correct thing to do for an ambisonic bed and much cheaper than ingesting
 *  every HOA channel.
 *
 *  No external dependencies: everything needed is in loudness.cpp and the
 *  REAPER API.
 * ==========================================================================*/

#ifdef _WIN32
#include <windows.h>
#else
#include "swell/swell.h"
#endif

#include "reaper_plugin.h"
#include "loudness.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

class AudioAccessor;

/* imported by pcmsrc_ambix.cpp */
extern void (*ShowConsoleMsg)(const char *msg);
extern int  (*ShowMessageBox)(const char *msg, const char *title, int type);

/* ---------------------------------------------------------------------------
 * REAPER API imports (resolved in AmbixNormalizeInit)
 * -------------------------------------------------------------------------*/
static int             (*CountSelectedMediaItems)(ReaProject *proj);
static MediaItem      *(*GetSelectedMediaItem)(ReaProject *proj, int selitem);
static MediaItem_Take *(*GetActiveTake)(MediaItem *item);
static PCM_source     *(*GetMediaItemTake_Source)(MediaItem_Take *take);
static int             (*GetMediaSourceNumChannels)(PCM_source *source);
static int             (*GetMediaSourceSampleRate)(PCM_source *source);
static double          (*GetMediaItemInfo_Value)(MediaItem *item, const char *parmname);
static double          (*GetMediaItemTakeInfo_Value)(MediaItem_Take *take, const char *parmname);
static bool            (*SetMediaItemTakeInfo_Value)(MediaItem_Take *take, const char *parmname, double newvalue);
static const char     *(*GetTakeName)(MediaItem_Take *take);
static AudioAccessor  *(*CreateTakeAudioAccessor)(MediaItem_Take *take);
static void            (*DestroyAudioAccessor)(AudioAccessor *accessor);
static double          (*GetAudioAccessorStartTime)(AudioAccessor *accessor);
static double          (*GetAudioAccessorEndTime)(AudioAccessor *accessor);
static int             (*GetAudioAccessorSamples)(AudioAccessor *accessor, int samplerate,
                                                  int numchannels, double starttime_sec,
                                                  int numsamplesperchannel, double *samplebuffer);
static TrackEnvelope  *(*GetTakeEnvelopeByName)(MediaItem_Take *take, const char *envname);
static bool            (*GetEnvelopeStateChunk)(TrackEnvelope *env, char *str, int str_sz, bool isundo);
static int             (*Envelope_Evaluate)(TrackEnvelope *envelope, double time, double samplerate,
                                            int samplesRequested, double *valueOutOptional,
                                            double *dVdSOutOptional, double *ddVdSOutOptional,
                                            double *dddVdSOutOptional);
static bool            (*GetUserInputs)(const char *title, int num_inputs, const char *captions_csv,
                                        char *retvals_csv, int retvals_csv_sz);
static void            (*SetExtState)(const char *section, const char *key, const char *value, bool persist);
static const char     *(*GetExtState)(const char *section, const char *key);
static void            (*Undo_BeginBlock)(void);
static void            (*Undo_EndBlock)(const char *descchange, int extraflags);
static void            (*UpdateArrange)(void);

#define EXTSTATE_SECTION "reaper_ambix"
#define EXTSTATE_KEY     "normalize_target_lufs"

static int g_normalizeCmd = 0;

/* ---------------------------------------------------------------------------
 * helpers
 * -------------------------------------------------------------------------*/

/* Take volume envelopes carry an "ACT <0|1>" line in their state chunk; an
 * inactive envelope does not affect playback and must not be compensated for. */
static bool IsEnvelopeActive(TrackEnvelope *env)
{
  if (!env || !GetEnvelopeStateChunk) return false;

  /* Envelope chunks start "<TAKEVOLENV\nACT <0|1> ...", so the flag is on the
   * second line — but GetEnvelopeStateChunk() fills nothing and returns false
   * unless the whole chunk fits, and a densely automated take can run long.
   * Retry once with room to spare rather than silently treating a large
   * envelope as inactive. */
  const int sizes[2] = { 64 * 1024, 1024 * 1024 };
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    std::vector<char> chunk((size_t)sizes[attempt]);
    chunk[0] = 0;
    if (!GetEnvelopeStateChunk(env, &chunk[0], sizes[attempt], false)) continue;

    const char *act = strstr(&chunk[0], "\nACT ");
    return act && atoi(act + 5) > 0;
  }
  return false;
}

/* Integrated loudness of a take, in LUFS, including the take/item volume
 * faders and (if present and active) the take volume envelope — the same set
 * of gain stages the normalization then adjusts.
 *
 * Returns AMBIX_LOUDNESS_NEGATIVE_INF for silence / unmeasurable material. */
static double MeasureTakeLoudness(MediaItem *item, MediaItem_Take *take, int *channelsOut,
                                  bool *isAmbisonicsOut)
{
  PCM_source *source = GetMediaItemTake_Source(take);
  if (!source) return AMBIX_LOUDNESS_NEGATIVE_INF;

  const int sourceChannels = GetMediaSourceNumChannels(source);
  if (sourceChannels < 1) return AMBIX_LOUDNESS_NEGATIVE_INF;  /* MIDI take etc. */

  double weights[AMBIX_LOUDNESS_MAX_CHANNELS];
  bool isAmbisonics = false;
  const int nch = AmbixLoudnessChannelSetup(sourceChannels, weights, &isAmbisonics);
  if (channelsOut)     *channelsOut     = sourceChannels;
  if (isAmbisonicsOut) *isAmbisonicsOut = isAmbisonics;

  int srate = GetMediaSourceSampleRate(source);
  if (srate < 8000) srate = 48000;

  AudioAccessor *accessor = CreateTakeAudioAccessor(take);
  if (!accessor) return AMBIX_LOUDNESS_NEGATIVE_INF;

  const double audioStart = GetAudioAccessorStartTime(accessor);
  const double audioEnd   = GetAudioAccessorEndTime(accessor);
  const double audioLen   = audioEnd - audioStart;
  if (audioLen <= 0.0)
  {
    DestroyAudioAccessor(accessor);
    return AMBIX_LOUDNESS_NEGATIVE_INF;
  }

  /* An audio accessor extracts samples immediately pre-FX, so take FX are not
   * part of the measurement, and neither are the gain stages below — fold those
   * in by hand, since they are exactly what the normalization then adjusts. */
  const double fader = GetMediaItemTakeInfo_Value(take, "D_VOL")
                     * GetMediaItemInfo_Value(item, "D_VOL");

  TrackEnvelope *volEnv = GetTakeEnvelopeByName ? GetTakeEnvelopeByName(take, "Volume") : NULL;
  if (!IsEnvelopeActive(volEnv)) volEnv = NULL;
  /* A take accessor reports times relative to the item start, while take
   * envelopes are evaluated on the project timeline, so the item position has
   * to be added back when looking the envelope up.
   * https://github.com/reaper-oss/sws/issues/957 */
  const double itemPos = GetMediaItemInfo_Value(item, "D_POSITION");

  AmbixLoudnessMeter meter(nch, (double)srate, weights);

  const int blockFrames = 8192;
  std::vector<double> buf((size_t)blockFrames * nch);

  double pos = audioStart;
  while (pos < audioEnd)
  {
    int frames = (int)((audioEnd - pos) * srate + 0.5);
    if (frames > blockFrames) frames = blockFrames;
    if (frames < 1) break;

    /* Asking for fewer channels than the source has yields the leading `nch`
     * channels interleaved, which is what makes the ambisonic path cheap: for
     * a third-order bed we request 1 channel and get W, instead of reading all
     * 16. GetAudioAccessorSamples() stops writing once it passes the item end,
     * so the buffer is zeroed first rather than left holding stale samples. */
    memset(&buf[0], 0, (size_t)frames * nch * sizeof(double));
    GetAudioAccessorSamples(accessor, srate, nch, pos, frames, &buf[0]);

    double gain = fader;
    if (volEnv)
    {
      double envValue = 1.0;
      /* Evaluated once per block (~170 ms at 48 kHz); volume envelopes move
       * far slower than that, and loudness is a long-window measure anyway. */
      Envelope_Evaluate(volEnv, pos + itemPos, (double)srate, 1, &envValue, NULL, NULL, NULL);
      gain *= envValue;
    }

    if (gain != 1.0)
    {
      const size_t n = (size_t)frames * nch;
      for (size_t i = 0; i < n; ++i) buf[i] *= gain;
    }

    meter.AddFrames(&buf[0], frames);
    pos += (double)frames / (double)srate;
  }

  DestroyAudioAccessor(accessor);
  return meter.GetIntegrated();
}

/* ---------------------------------------------------------------------------
 * the action
 * -------------------------------------------------------------------------*/
static void NormalizeSelectedItems()
{
  const int numSelected = CountSelectedMediaItems(NULL);
  if (numSelected < 1)
  {
    ShowMessageBox("Select at least one media item first.",
                   "ambiX: Normalize item loudness", 0);
    return;
  }

  /* Target level: remembered across sessions in reaper-extstate.ini. */
  char target[64];
  const char *stored = GetExtState(EXTSTATE_SECTION, EXTSTATE_KEY);
  if (stored && *stored)
    snprintf(target, sizeof(target), "%s", stored);
  else
    snprintf(target, sizeof(target), "-23");

  if (!GetUserInputs("ambiX: Normalize item loudness", 1,
                     "Target integrated loudness (LUFS):,extrawidth=60",
                     target, sizeof(target)))
    return;  /* cancelled */

  char *parseEnd = NULL;
  const double targetLufs = strtod(target, &parseEnd);
  /* strtod() reports junk as 0.0, so check that it actually consumed digits. */
  if (parseEnd == target || targetLufs > 0.0 || targetLufs < -70.0)
  {
    ShowMessageBox("Enter a target loudness between -70 and 0 LUFS (for example -23).",
                   "ambiX: Normalize item loudness", 0);
    return;
  }
  SetExtState(EXTSTATE_SECTION, EXTSTATE_KEY, target, true);

  Undo_BeginBlock();

  char line[1024];
  snprintf(line, sizeof(line),
           "ambiX: normalizing %d item%s to %.2f LUFS\n",
           numSelected, numSelected == 1 ? "" : "s", targetLufs);
  ShowConsoleMsg(line);

  int normalized = 0, skipped = 0;

  for (int i = 0; i < numSelected; ++i)
  {
    MediaItem *item = GetSelectedMediaItem(NULL, i);
    if (!item) continue;

    MediaItem_Take *take = GetActiveTake(item);

    char takeName[256];
    const char *name = (take && GetTakeName) ? GetTakeName(take) : NULL;
    if (name && *name) snprintf(takeName, sizeof(takeName), "%s", name);
    else               snprintf(takeName, sizeof(takeName), "item %d", i + 1);

    if (!take)
    {
      snprintf(line, sizeof(line), "  %s: skipped (no active take)\n", takeName);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    /* Locked items are left alone, matching REAPER's own item actions.
     * C_LOCK is a bitmask; &1 is the lock bit. */
    if (((int)GetMediaItemInfo_Value(item, "C_LOCK")) & 1)
    {
      snprintf(line, sizeof(line), "  %s: skipped (item locked)\n", takeName);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    int channels = 0;
    bool isAmbisonics = false;
    const double measured = MeasureTakeLoudness(item, take, &channels, &isAmbisonics);

    if (measured <= AMBIX_LOUDNESS_NEGATIVE_INF)
    {
      snprintf(line, sizeof(line),
               "  %s: skipped (silent, too short, or not audio)\n", takeName);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    const double gainDb = targetLufs - measured;
    const double oldVol = GetMediaItemTakeInfo_Value(take, "D_VOL");
    /* Take volume may legitimately be negative (phase-inverted take); scale the
     * magnitude and keep the sign. */
    const double newVol = oldVol * pow(10.0, gainDb / 20.0);
    SetMediaItemTakeInfo_Value(take, "D_VOL", newVol);

    snprintf(line, sizeof(line),
             "  %s: %.1f LUFS -> %.1f LUFS (%+.2f dB, %d ch%s)\n",
             takeName, measured, targetLufs, gainDb, channels,
             isAmbisonics ? ", ambisonic - W measured" : "");
    ShowConsoleMsg(line);
    ++normalized;
  }

  snprintf(line, sizeof(line), "ambiX: %d normalized, %d skipped\n\n", normalized, skipped);
  ShowConsoleMsg(line);

  Undo_EndBlock("ambiX: Normalize item loudness", UNDO_STATE_ITEMS);
  UpdateArrange();
}

static bool hookCommandProc(int command, int flag)
{
  if (!g_normalizeCmd || command != g_normalizeCmd) return false;
  NormalizeSelectedItems();
  return true;
}

static gaccel_register_t g_normalizeAccel =
{
  { 0, 0, 0 },
  "ambiX: Normalize selected item(s) to target loudness (LUFS)..."
};

/* ---------------------------------------------------------------------------
 * registration
 * -------------------------------------------------------------------------*/
#define IMPAPI_OPT(x) if (!((*((void **)&(x)) = (void *)rec->GetFunc(#x)))) ++missing;

bool AmbixNormalizeInit(reaper_plugin_info_t *rec)
{
  int missing = 0;

  IMPAPI_OPT(CountSelectedMediaItems);
  IMPAPI_OPT(GetSelectedMediaItem);
  IMPAPI_OPT(GetActiveTake);
  IMPAPI_OPT(GetMediaItemTake_Source);
  IMPAPI_OPT(GetMediaSourceNumChannels);
  IMPAPI_OPT(GetMediaSourceSampleRate);
  IMPAPI_OPT(GetMediaItemInfo_Value);
  IMPAPI_OPT(GetMediaItemTakeInfo_Value);
  IMPAPI_OPT(SetMediaItemTakeInfo_Value);
  IMPAPI_OPT(CreateTakeAudioAccessor);
  IMPAPI_OPT(DestroyAudioAccessor);
  IMPAPI_OPT(GetAudioAccessorStartTime);
  IMPAPI_OPT(GetAudioAccessorEndTime);
  IMPAPI_OPT(GetAudioAccessorSamples);
  IMPAPI_OPT(GetUserInputs);
  IMPAPI_OPT(SetExtState);
  IMPAPI_OPT(GetExtState);
  IMPAPI_OPT(Undo_BeginBlock);
  IMPAPI_OPT(Undo_EndBlock);
  IMPAPI_OPT(UpdateArrange);

  if (missing) return false;  /* leave the rest of the plugin working */

  /* Nice-to-haves: the action still works without them. */
  *((void **)&GetTakeName)           = (void *)rec->GetFunc("GetTakeName");
  *((void **)&GetTakeEnvelopeByName) = (void *)rec->GetFunc("GetTakeEnvelopeByName");
  *((void **)&GetEnvelopeStateChunk) = (void *)rec->GetFunc("GetEnvelopeStateChunk");
  *((void **)&Envelope_Evaluate)     = (void *)rec->GetFunc("Envelope_Evaluate");
  if (!Envelope_Evaluate) GetTakeEnvelopeByName = NULL;

  g_normalizeCmd = rec->Register("command_id", (void *)"AMBIX_NORMALIZE_ITEM_LOUDNESS");
  if (!g_normalizeCmd) return false;

  g_normalizeAccel.accel.cmd = g_normalizeCmd;
  if (!rec->Register("gaccel", &g_normalizeAccel)) return false;
  if (!rec->Register("hookcommand", (void *)hookCommandProc)) return false;

  return true;
}
