/* ============================================================================
 *  loudness_actions.cpp — the two BS.1770 loudness actions.
 *
 *    ambiX: Normalize selected item(s) to target loudness (LUFS)...
 *    ambiX: Measure loudness of selected item(s)
 *
 *  Both measure each selected item's active take with the local BS.1770-4
 *  implementation in loudness.cpp. The first then scales the take volume so
 *  the integrated loudness lands on the user's target; the second only reports
 *  what it found and changes nothing, which is what you want when checking a
 *  delivery rather than fixing it.
 *
 *  REAPER has its own item loudness analysis, but no ambisonic mode: it
 *  measures every channel of a 36-channel fifth-order bed rather than W alone.
 *  Reporting the same number the normalize action drives to a target is the
 *  point of the measure-only action; it is not a general replacement for the
 *  host's analysis, which also gives peak and LRA.
 *
 *  Ambisonic sources (channel count a perfect square >= 4) are measured on the
 *  W channel alone — see AmbixLoudnessChannelSetup() — which is both the
 *  correct thing to do for an ambisonic bed and much cheaper than ingesting
 *  every HOA channel.
 *
 *  Measurement runs in chunks driven by a timer inside a modal progress
 *  dialog, so REAPER stays responsive and the run can be cancelled. Everything
 *  happens on the main thread — no worker threads, and therefore no REAPER API
 *  calls from a background thread.
 *
 *  No external dependencies: everything needed is in loudness.cpp and the
 *  REAPER API.
 * ==========================================================================*/

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>   /* PBM_SETRANGE / PBM_SETPOS and msctls_progress32 */
#else
#include "swell/swell.h"
#endif

#include "wdltypes.h"   /* WDL_DLGRET, GWLP_USERDATA / SetWindowLongPtr shims */
#include "reaper_plugin.h"
#include "resource.h"
#include "loudness.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>

class AudioAccessor;

/* imported by pcmsrc_ambix.cpp */
extern void (*ShowConsoleMsg)(const char *msg);
extern int  (*ShowMessageBox)(const char *msg, const char *title, int type);
extern REAPER_PLUGIN_HINSTANCE g_hInst;

/* ---------------------------------------------------------------------------
 * REAPER API imports (resolved in AmbixLoudnessActionsInit)
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
static HWND            (*GetMainHwnd)(void);
static bool            (*ValidatePtr2)(ReaProject *proj, void *pointer, const char *ctypename);

#define EXTSTATE_SECTION "reaper_ambix"
#define EXTSTATE_KEY     "normalize_target_lufs"

#define MEASURE_TITLE "ambiX: Measure item loudness"

static int g_normalizeCmd = 0;
static int g_measureCmd   = 0;

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

/* ---------------------------------------------------------------------------
 * measurement job
 *
 * The work is split into chunks so a timer inside the progress dialog can
 * drive it: each tick advances the analysis by a fixed slice of CPU time, then
 * hands control back to the event loop so the bar repaints and Cancel works.
 * -------------------------------------------------------------------------*/

struct NormalizeEntry
{
  MediaItem      *item;
  MediaItem_Take *take;
  char            name[256];
  const char     *skipReason;    /* non-NULL: nothing to measure, just report */

  /* measurement setup, resolved up front on the main thread */
  int             sourceChannels;
  bool            isAmbisonics;
  int             nch;           /* channels actually read (1 for ambisonics) */
  double          weights[AMBIX_LOUDNESS_MAX_CHANNELS];
  int             srate;
  double          fader;
  TrackEnvelope  *volEnv;
  double          itemPos;
  double          audioStart, audioEnd;

  double          measured;      /* result, in LUFS */
};

struct NormalizeJob
{
  std::vector<NormalizeEntry> entries;
  size_t                      cur;          /* entry being measured */

  /* state for the entry in progress */
  AudioAccessor              *accessor;
  AmbixLoudnessMeter         *meter;
  double                      pos;
  std::vector<double>         buf;

  double                      totalSeconds; /* for the progress fraction */
  double                      doneSeconds;
  bool                        cancelled;
  bool                        finished;

  NormalizeJob() : cur(0), accessor(NULL), meter(NULL), pos(0.0),
                   totalSeconds(0.0), doneSeconds(0.0),
                   cancelled(false), finished(false) {}
};

static const int kBlockFrames = 8192;

/* The progress dialog runs an event loop, so the project can change while a
 * measurement is in flight — an item deleted mid-run would leave us holding a
 * dangling pointer. Re-check before touching one. */
static bool EntryStillValid(const NormalizeEntry &e)
{
  if (!ValidatePtr2) return true;   /* very old REAPER: nothing we can do */
  return ValidatePtr2(NULL, (void *)e.item, "MediaItem*")
      && ValidatePtr2(NULL, (void *)e.take, "MediaItem_Take*");
}


/* Open the accessor and meter for entries[cur]. False if it cannot be
 * measured, in which case skipReason has been filled in. */
static bool JobOpenEntry(NormalizeJob &job)
{
  NormalizeEntry &e = job.entries[job.cur];

  if (!EntryStillValid(e))
  {
    e.skipReason = "item was removed during analysis";
    return false;
  }

  job.accessor = CreateTakeAudioAccessor(e.take);
  if (!job.accessor)
  {
    e.skipReason = "could not read audio";
    return false;
  }

  e.audioStart = GetAudioAccessorStartTime(job.accessor);
  e.audioEnd   = GetAudioAccessorEndTime(job.accessor);
  if (e.audioEnd - e.audioStart <= 0.0)
  {
    DestroyAudioAccessor(job.accessor);
    job.accessor = NULL;
    e.skipReason = "empty";
    return false;
  }

  job.meter = new AmbixLoudnessMeter(e.nch, (double)e.srate, e.weights);
  job.pos   = e.audioStart;
  job.buf.resize((size_t)kBlockFrames * e.nch);
  return true;
}

static void JobCloseEntry(NormalizeJob &job)
{
  NormalizeEntry &e = job.entries[job.cur];

  if (job.meter)
  {
    e.measured = job.meter->GetIntegrated();
    if (e.measured <= AMBIX_LOUDNESS_NEGATIVE_INF)
      e.skipReason = "silent or shorter than 400 ms";
    delete job.meter;
    job.meter = NULL;
  }
  if (job.accessor)
  {
    DestroyAudioAccessor(job.accessor);
    job.accessor = NULL;
  }
  ++job.cur;
}

/* Advance the analysis by roughly `budgetSeconds` of CPU time. Returns true
 * once every entry has been dealt with. */
static bool JobStep(NormalizeJob &job, double budgetSeconds)
{
  const clock_t deadline = clock() + (clock_t)(budgetSeconds * (double)CLOCKS_PER_SEC);

  while (job.cur < job.entries.size())
  {
    NormalizeEntry &e = job.entries[job.cur];

    /* entries rejected up front (no take, locked, MIDI) need no work */
    if (e.skipReason)
    {
      ++job.cur;
      continue;
    }

    if (!job.accessor && !job.meter)
    {
      if (!JobOpenEntry(job))
      {
        ++job.cur;
        continue;
      }
    }

    while (job.pos < e.audioEnd)
    {
      int frames = (int)((e.audioEnd - job.pos) * e.srate + 0.5);
      if (frames > kBlockFrames) frames = kBlockFrames;
      if (frames < 1) break;

      /* Asking for fewer channels than the source has yields the leading
       * `nch` channels interleaved, which is what makes the ambisonic path
       * cheap: a fifth-order bed is 36 channels but only W is requested.
       * GetAudioAccessorSamples() stops writing once past the item end, so
       * the buffer is zeroed rather than left holding stale samples. */
      memset(&job.buf[0], 0, (size_t)frames * e.nch * sizeof(double));
      GetAudioAccessorSamples(job.accessor, e.srate, e.nch, job.pos, frames, &job.buf[0]);

      double gain = e.fader;
      if (e.volEnv)
      {
        double envValue = 1.0;
        /* Evaluated once per block (~170 ms at 48 kHz); volume envelopes move
         * far slower than that, and loudness is a long-window measure. */
        Envelope_Evaluate(e.volEnv, job.pos + e.itemPos, (double)e.srate, 1,
                          &envValue, NULL, NULL, NULL);
        gain *= envValue;
      }

      if (gain != 1.0)
      {
        const size_t n = (size_t)frames * e.nch;
        for (size_t i = 0; i < n; ++i) job.buf[i] *= gain;
      }

      job.meter->AddFrames(&job.buf[0], frames);

      const double advanced = (double)frames / (double)e.srate;
      job.pos         += advanced;
      job.doneSeconds += advanced;

      if (clock() >= deadline) return false;  /* let the UI breathe */
    }

    JobCloseEntry(job);
  }

  job.finished = true;
  return true;
}

static void JobAbort(NormalizeJob &job)
{
  if (job.meter)    { delete job.meter; job.meter = NULL; }
  if (job.accessor) { DestroyAudioAccessor(job.accessor); job.accessor = NULL; }
}

/* ---------------------------------------------------------------------------
 * progress dialog
 * -------------------------------------------------------------------------*/
#define AMBIX_PROGRESS_TIMER 1
#define AMBIX_PROGRESS_RANGE 1000

static WDL_DLGRET ProgressDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  NormalizeJob *job = (NormalizeJob *)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);

  switch (uMsg)
  {
    case WM_INITDIALOG:
      SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);
      SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETRANGE, 0,
                         MAKELPARAM(0, AMBIX_PROGRESS_RANGE));
      SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETPOS, 0, 0);
      SetDlgItemText(hwndDlg, IDC_PROGRESS_ITEM, "Preparing...");
      /* A 1 ms timer effectively means "as often as the event loop allows";
       * each tick does a bounded slice of work. */
      SetTimer(hwndDlg, AMBIX_PROGRESS_TIMER, 1, NULL);
    return 1;

    case WM_TIMER:
      if (wParam == AMBIX_PROGRESS_TIMER && job)
      {
        const bool done = JobStep(*job, 0.05);

        if (job->cur < job->entries.size())
        {
          const NormalizeEntry &e = job->entries[job->cur];
          char line[512];
          snprintf(line, sizeof(line), "Item %d of %d: %s",
                   (int)job->cur + 1, (int)job->entries.size(), e.name);
          SetDlgItemText(hwndDlg, IDC_PROGRESS_ITEM, line);

          if (e.isAmbisonics)
            snprintf(line, sizeof(line), "%d channels, ambisonic — measuring W only",
                     e.sourceChannels);
          else if (e.nch < e.sourceChannels)
            snprintf(line, sizeof(line), "%d channels — measuring the first %d",
                     e.sourceChannels, e.nch);
          else
            snprintf(line, sizeof(line), "%d channel%s", e.sourceChannels,
                     e.sourceChannels == 1 ? "" : "s");
          SetDlgItemText(hwndDlg, IDC_PROGRESS_STATUS, line);
        }

        const double frac = (job->totalSeconds > 0.0)
                          ? (job->doneSeconds / job->totalSeconds) : 1.0;
        SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETPOS,
                           (WPARAM)(int)(frac * AMBIX_PROGRESS_RANGE + 0.5), 0);

        if (done)
        {
          KillTimer(hwndDlg, AMBIX_PROGRESS_TIMER);
          EndDialog(hwndDlg, 1);
        }
      }
    return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == IDCANCEL)
      {
        KillTimer(hwndDlg, AMBIX_PROGRESS_TIMER);
        if (job)
        {
          job->cancelled = true;
          JobAbort(*job);
        }
        EndDialog(hwndDlg, 0);
      }
    return 0;

    case WM_DESTROY:
      KillTimer(hwndDlg, AMBIX_PROGRESS_TIMER);
    return 0;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * the action
 * -------------------------------------------------------------------------*/

/* Resolve everything the measurement needs, without reading any audio yet. */
static void BuildEntry(NormalizeEntry &e, MediaItem *item, int index)
{
  memset(&e, 0, sizeof(e));
  e.item     = item;
  e.take     = GetActiveTake(item);
  e.measured = AMBIX_LOUDNESS_NEGATIVE_INF;

  const char *name = (e.take && GetTakeName) ? GetTakeName(e.take) : NULL;
  if (name && *name) snprintf(e.name, sizeof(e.name), "%s", name);
  else               snprintf(e.name, sizeof(e.name), "item %d", index + 1);

  if (!e.take)
  {
    e.skipReason = "no active take";
    return;
  }

  /* Locked items are left alone, matching REAPER's own item actions.
   * C_LOCK is a bitmask; &1 is the lock bit. */
  if (((int)GetMediaItemInfo_Value(item, "C_LOCK")) & 1)
  {
    e.skipReason = "item locked";
    return;
  }

  PCM_source *source = GetMediaItemTake_Source(e.take);
  if (!source)
  {
    e.skipReason = "no source";
    return;
  }

  e.sourceChannels = GetMediaSourceNumChannels(source);
  if (e.sourceChannels < 1)
  {
    e.skipReason = "not audio";   /* MIDI take, video, ... */
    return;
  }

  e.nch = AmbixLoudnessChannelSetup(e.sourceChannels, e.weights, &e.isAmbisonics);

  e.srate = GetMediaSourceSampleRate(source);
  if (e.srate < 8000) e.srate = 48000;

  /* An audio accessor extracts samples immediately pre-FX, so take FX are not
   * part of the measurement, and neither are the gain stages below — fold
   * those in by hand, since they are exactly what the normalization adjusts. */
  e.fader = GetMediaItemTakeInfo_Value(e.take, "D_VOL")
          * GetMediaItemInfo_Value(item, "D_VOL");

  e.volEnv = GetTakeEnvelopeByName ? GetTakeEnvelopeByName(e.take, "Volume") : NULL;
  if (!IsEnvelopeActive(e.volEnv)) e.volEnv = NULL;

  /* A take accessor reports times relative to the item start, while take
   * envelopes are evaluated on the project timeline, so the item position has
   * to be added back when looking the envelope up.
   * https://github.com/reaper-oss/sws/issues/957 */
  e.itemPos = GetMediaItemInfo_Value(item, "D_POSITION");
}

/* Build a job from the current item selection and run it behind the progress
 * dialog. False means there is nothing to report and the caller should return:
 * either nothing was measurable (a message box has been shown) or the user
 * cancelled (job.cancelled says which). */
static bool GatherAndMeasure(NormalizeJob &job, int numSelected, const char *title)
{
  job.entries.resize((size_t)numSelected);

  size_t measurable = 0;
  for (int i = 0; i < numSelected; ++i)
  {
    MediaItem *item = GetSelectedMediaItem(NULL, i);
    NormalizeEntry &e = job.entries[(size_t)i];

    if (!item)
    {
      memset(&e, 0, sizeof(e));
      e.skipReason = "item disappeared";
      snprintf(e.name, sizeof(e.name), "item %d", i + 1);
      continue;
    }

    BuildEntry(e, item, i);
    if (!e.skipReason)
    {
      ++measurable;
      /* Item length drives the progress fraction. It is only an estimate of
       * the accessor's range, but a progress bar needs no better. */
      job.totalSeconds += GetMediaItemInfo_Value(item, "D_LENGTH");
    }
  }

  if (!measurable)
  {
    ShowMessageBox("None of the selected items could be measured "
                   "(no active take, locked, or not audio).", title, 0);
    return false;
  }

  const int completed = (int)DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_AMBIX_PROGRESS),
                                            GetMainHwnd ? GetMainHwnd() : NULL,
                                            ProgressDlgProc, (LPARAM)&job);

  return completed && !job.cancelled;
}

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

  NormalizeJob job;
  if (!GatherAndMeasure(job, numSelected, "ambiX: Normalize item loudness"))
  {
    /* Cancel means cancel: nothing is applied, so there is nothing to undo. */
    if (job.cancelled)
      ShowConsoleMsg("ambiX: normalization cancelled, no items changed\n\n");
    return;
  }

  /* --- apply --- */
  Undo_BeginBlock();

  char line[1024];
  snprintf(line, sizeof(line), "ambiX: normalizing %d item%s to %.2f LUFS\n",
           numSelected, numSelected == 1 ? "" : "s", targetLufs);
  ShowConsoleMsg(line);

  int normalized = 0, skipped = 0;

  for (size_t i = 0; i < job.entries.size(); ++i)
  {
    const NormalizeEntry &e = job.entries[i];

    if (e.skipReason)
    {
      snprintf(line, sizeof(line), "  %s: skipped (%s)\n", e.name, e.skipReason);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    if (!EntryStillValid(e))
    {
      snprintf(line, sizeof(line), "  %s: skipped (removed during analysis)\n", e.name);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    const double gainDb = targetLufs - e.measured;
    const double oldVol = GetMediaItemTakeInfo_Value(e.take, "D_VOL");
    /* Take volume may legitimately be negative (phase-inverted take); scale
     * the magnitude and keep the sign. */
    SetMediaItemTakeInfo_Value(e.take, "D_VOL", oldVol * pow(10.0, gainDb / 20.0));

    snprintf(line, sizeof(line),
             "  %s: %.1f LUFS -> %.1f LUFS (%+.2f dB, %d ch%s)\n",
             e.name, e.measured, targetLufs, gainDb, e.sourceChannels,
             e.isAmbisonics ? ", ambisonic - W measured" : "");
    ShowConsoleMsg(line);
    ++normalized;
  }

  snprintf(line, sizeof(line), "ambiX: %d normalized, %d skipped\n\n", normalized, skipped);
  ShowConsoleMsg(line);

  Undo_EndBlock("ambiX: Normalize item loudness", UNDO_STATE_ITEMS);
  UpdateArrange();
}

/* ---------------------------------------------------------------------------
 * measure only
 *
 * Same measurement as the normalize action, including the take fader and an
 * active take volume envelope, so the number reported is the loudness of the
 * item as it currently plays (pre track FX) rather than of the raw file. That
 * is deliberately the same quantity the normalize action drives to a target,
 * so running measure after normalize reads back what you asked for.
 * -------------------------------------------------------------------------*/

static void MeasureSelectedItems()
{
  const int numSelected = CountSelectedMediaItems(NULL);
  if (numSelected < 1)
  {
    ShowMessageBox("Select at least one media item first.",
                   MEASURE_TITLE, 0);
    return;
  }

  NormalizeJob job;
  if (!GatherAndMeasure(job, numSelected, MEASURE_TITLE))
  {
    if (job.cancelled) ShowConsoleMsg("ambiX: measurement cancelled\n\n");
    return;
  }

  char line[1024];
  snprintf(line, sizeof(line), "ambiX: integrated loudness of %d item%s (ITU-R BS.1770-4)\n",
           numSelected, numSelected == 1 ? "" : "s");
  ShowConsoleMsg(line);

  int measured = 0, skipped = 0;
  double sum = 0.0, quietest = 0.0, loudest = 0.0;

  for (size_t i = 0; i < job.entries.size(); ++i)
  {
    const NormalizeEntry &e = job.entries[i];

    if (e.skipReason)
    {
      snprintf(line, sizeof(line), "  %s: skipped (%s)\n", e.name, e.skipReason);
      ShowConsoleMsg(line);
      ++skipped;
      continue;
    }

    snprintf(line, sizeof(line), "  %s: %.1f LUFS (%d ch%s)\n",
             e.name, e.measured, e.sourceChannels,
             e.isAmbisonics ? ", ambisonic - W measured" : "");
    ShowConsoleMsg(line);

    if (!measured || e.measured < quietest) quietest = e.measured;
    if (!measured || e.measured > loudest)  loudest   = e.measured;
    sum += e.measured;
    ++measured;
  }

  if (measured > 1)
  {
    /* A plain mean of LUFS values, not an energy sum: this is a spread
     * indicator for a set of deliverables, not the loudness of the set played
     * together. The range is the number that matters when checking whether a
     * batch is consistent. */
    snprintf(line, sizeof(line),
             "ambiX: %d measured, %d skipped - quietest %.1f, loudest %.1f, "
             "spread %.1f LU, mean %.1f LUFS\n\n",
             measured, skipped, quietest, loudest, loudest - quietest,
             sum / (double)measured);
  }
  else
  {
    snprintf(line, sizeof(line), "ambiX: %d measured, %d skipped\n\n",
             measured, skipped);
  }
  ShowConsoleMsg(line);

  /* Nothing was changed, so no undo point and no arrange redraw. */
}

static bool hookCommandProc(int command, int flag)
{
  if (g_normalizeCmd && command == g_normalizeCmd) { NormalizeSelectedItems(); return true; }
  if (g_measureCmd   && command == g_measureCmd)   { MeasureSelectedItems();   return true; }
  return false;
}

static gaccel_register_t g_normalizeAccel =
{
  { 0, 0, 0 },
  "ambiX: Normalize selected item(s) to target loudness (LUFS)..."
};

/* No ellipsis: this one asks for nothing, it just measures and reports. */
static gaccel_register_t g_measureAccel =
{
  { 0, 0, 0 },
  "ambiX: Measure loudness of selected item(s) (LUFS)"
};

/* ---------------------------------------------------------------------------
 * registration
 * -------------------------------------------------------------------------*/
#define IMPAPI_OPT(x) if (!((*((void **)&(x)) = (void *)rec->GetFunc(#x)))) ++missing;

bool AmbixLoudnessActionsInit(reaper_plugin_info_t *rec)
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
  IMPAPI_OPT(GetMainHwnd);

  if (missing) return false;  /* leave the rest of the plugin working */

  /* Nice-to-haves: the action still works without them. */
  *((void **)&GetTakeName)           = (void *)rec->GetFunc("GetTakeName");
  *((void **)&GetTakeEnvelopeByName) = (void *)rec->GetFunc("GetTakeEnvelopeByName");
  *((void **)&GetEnvelopeStateChunk) = (void *)rec->GetFunc("GetEnvelopeStateChunk");
  *((void **)&Envelope_Evaluate)     = (void *)rec->GetFunc("Envelope_Evaluate");
  *((void **)&ValidatePtr2)          = (void *)rec->GetFunc("ValidatePtr2");
  if (!Envelope_Evaluate) GetTakeEnvelopeByName = NULL;

#ifdef _WIN32
  /* Register the msctls_progress32 window class. REAPER's own UI already pulls
   * comctl32 in, so this is belt and braces — but a progress bar that silently
   * fails to create would be an annoying thing to debug remotely. */
  InitCommonControls();
#endif

  g_normalizeCmd = rec->Register("command_id", (void *)"AMBIX_NORMALIZE_ITEM_LOUDNESS");
  if (!g_normalizeCmd) return false;

  g_normalizeAccel.accel.cmd = g_normalizeCmd;
  if (!rec->Register("gaccel", &g_normalizeAccel)) return false;

  /* The measure action shares every import above, so it either registers
   * alongside the normalize action or not at all. */
  g_measureCmd = rec->Register("command_id", (void *)"AMBIX_MEASURE_ITEM_LOUDNESS");
  if (g_measureCmd)
  {
    g_measureAccel.accel.cmd = g_measureCmd;
    if (!rec->Register("gaccel", &g_measureAccel)) g_measureCmd = 0;
  }

  /* One hook serves both command ids. */
  if (!rec->Register("hookcommand", (void *)hookCommandProc)) return false;

  return true;
}
