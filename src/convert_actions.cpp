/* ============================================================================
 *  convert_actions.cpp — the two file-conversion actions.
 *
 *    ambiX: Convert selected item(s) to .ambix file(s)...
 *    ambiX: Convert selected item(s) from FuMa to ambiX...
 *
 *  Both walk the same pipeline: read each selected item through a take audio
 *  accessor, optionally run the block through a channel-conversion matrix, and
 *  write an .ambix file with libambix. The render dialog can also write .ambix
 *  per item ("Selected media items"), and it is the tool to use when FX must be
 *  baked in, since it runs the take FX chain. These two are for material that
 *  is already ambiX: a pre-FX copy per item, next to its source, with no render
 *  settings to touch — a dozen deliverables, or a folder of FuMa material that
 *  needs bringing into the ambiX convention.
 *
 *  What gets written is the ITEM, not the whole source file: the accessor
 *  delivers the item's own extent with its take gain applied, pre track FX.
 *  That makes "normalize, then convert" compose the way you would expect, and
 *  it means two items trimmed out of one long recording convert to two files.
 *
 *  The FuMa conversion deliberately does NOT use libambix's AMBIX_MATRIX_FUMA,
 *  which routes first-order X into the ambiX Z slot and adds Condon-Shortley
 *  signs the ambiX paper rejects. src/fuma.cpp implements the paper's matrix
 *  instead, with tests/test_fuma.cpp pinning it down. FuMa is only defined to
 *  third order, so 16 channels is the ceiling.
 *
 *  Measurement-style chunking: the work is driven by a timer inside the shared
 *  progress dialog, so REAPER stays responsive and a long batch can be
 *  cancelled. Everything runs on the main thread.
 * ==========================================================================*/

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>   /* PBM_SETRANGE / PBM_SETPOS */
#else
#include "swell/swell.h"
#endif

#include "wdltypes.h"   /* WDL_DLGRET, GWLP_USERDATA / SetWindowLongPtr shims */
#include "reaper_plugin.h"
#include "resource.h"

#include <ambix/ambix.h>

#include "fuma.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

class AudioAccessor;

/* imported by pcmsrc_ambix.cpp */
extern void (*ShowConsoleMsg)(const char *msg);
extern int  (*ShowMessageBox)(const char *msg, const char *title, int type);
extern REAPER_PLUGIN_HINSTANCE g_hInst;

/* ---------------------------------------------------------------------------
 * REAPER API imports (resolved in AmbixConvertActionsInit)
 * -------------------------------------------------------------------------*/
static int             (*CountSelectedMediaItems)(ReaProject *proj);
static MediaItem      *(*GetSelectedMediaItem)(ReaProject *proj, int selitem);
static MediaItem_Take *(*GetActiveTake)(MediaItem *item);
static PCM_source     *(*GetMediaItemTake_Source)(MediaItem_Take *take);
static int             (*GetMediaSourceNumChannels)(PCM_source *source);
static int             (*GetMediaSourceSampleRate)(PCM_source *source);
static void            (*GetMediaSourceFileName)(PCM_source *source, char *buf, int buf_sz);
static double          (*GetMediaItemInfo_Value)(MediaItem *item, const char *parmname);
static const char     *(*GetTakeName)(MediaItem_Take *take);
static AudioAccessor  *(*CreateTakeAudioAccessor)(MediaItem_Take *take);
static void            (*DestroyAudioAccessor)(AudioAccessor *accessor);
static double          (*GetAudioAccessorStartTime)(AudioAccessor *accessor);
static double          (*GetAudioAccessorEndTime)(AudioAccessor *accessor);
static int             (*GetAudioAccessorSamples)(AudioAccessor *accessor, int samplerate,
                                                  int numchannels, double starttime_sec,
                                                  int numsamplesperchannel, double *samplebuffer);
static bool            (*GetUserInputs)(const char *title, int num_inputs, const char *captions_csv,
                                        char *retvals_csv, int retvals_csv_sz);
static void            (*SetExtState)(const char *section, const char *key, const char *value, bool persist);
static const char     *(*GetExtState)(const char *section, const char *key);
static void            (*GetProjectPath)(char *buf, int buf_sz);
static HWND            (*GetMainHwnd)(void);
static bool            (*ValidatePtr2)(ReaProject *proj, void *pointer, const char *ctypename);

/* only needed by the FuMa action's "add as a new take" option */
static MediaItem_Take *(*AddTakeToMediaItem)(MediaItem *item);
static PCM_source     *(*PCM_Source_CreateFromFile)(const char *filename);
static bool            (*SetMediaItemTake_Source)(MediaItem_Take *take, PCM_source *source);
static bool            (*GetSetMediaItemTakeInfo_String)(MediaItem_Take *tk, const char *parmname,
                                                         char *stringNeedBig, bool setNewValue);
static void            (*Undo_BeginBlock)(void);
static void            (*Undo_EndBlock)(const char *descchange, int extraflags);
static void            (*UpdateArrange)(void);

#define EXTSTATE_SECTION "reaper_ambix"
#define KEY_WAVPACK      "convert_wavpack"
#define KEY_WAVPACK_BITS "convert_wavpack_bits"
#define KEY_OVERWRITE    "convert_overwrite"
#define KEY_ADDTAKE      "convert_fuma_addtake"

#define CONVERT_TITLE "ambiX: Convert item(s) to .ambix"
#define FUMA_TITLE    "ambiX: Convert item(s) from FuMa"

static int g_convertCmd = 0;
static int g_fumaCmd    = 0;

/* ---------------------------------------------------------------------------
 * channel-count helpers
 * -------------------------------------------------------------------------*/

/* Ambisonic channel counts are the perfect squares (N+1)^2 — the same test
 * loudness.cpp and channelcount_action.cpp use. -1 if not an ambisonic set. */
static int AmbisonicOrder(int nch)
{
  const int root = (int)lround(sqrt((double)nch));
  if (nch >= 1 && root * root == nch) return root - 1;
  return -1;
}

/* ---------------------------------------------------------------------------
 * output paths
 * -------------------------------------------------------------------------*/

#ifdef _WIN32
#define AMBIX_PATH_SEP '\\'
#else
#define AMBIX_PATH_SEP '/'
#endif

static bool FileExists(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

/* Strip anything a filesystem may object to. Conservative on purpose — this
 * runs on take names, which users put all sorts of things in. */
static void SanitizeBaseName(std::string &s)
{
  for (size_t i = 0; i < s.size(); ++i)
  {
    const char c = s[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20)
      s[i] = '_';
  }
  /* Trailing dots and spaces are trouble on Windows. */
  while (!s.empty() && (s[s.size() - 1] == '.' || s[s.size() - 1] == ' '))
    s.erase(s.size() - 1);
  if (s.empty()) s = "item";
}

static std::string DirectoryOf(const std::string &path)
{
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
}

static std::string BaseNameNoExt(const std::string &path)
{
  const size_t slash = path.find_last_of("/\\");
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
  return base;
}

/* Where the converted file goes: alongside the source media file, named after
 * the take. Two items trimmed out of one recording therefore land next to it
 * as separate files, and the -1/-2 suffix keeps them from colliding when the
 * take names are identical too. */
static bool BuildOutputPath(const std::string &dir, const std::string &base,
                            bool overwrite, std::string &out)
{
  char buf[2600];
  snprintf(buf, sizeof(buf), "%s%c%s.ambix", dir.c_str(), AMBIX_PATH_SEP, base.c_str());
  out = buf;
  if (overwrite || !FileExists(out.c_str())) return true;

  for (int n = 1; n < 1000; ++n)
  {
    snprintf(buf, sizeof(buf), "%s%c%s-%d.ambix", dir.c_str(), AMBIX_PATH_SEP,
             base.c_str(), n);
    out = buf;
    if (!FileExists(out.c_str())) return true;
  }
  return false;
}

/* ---------------------------------------------------------------------------
 * the job
 * -------------------------------------------------------------------------*/

struct ConvertEntry
{
  MediaItem      *item;
  MediaItem_Take *take;
  char            name[256];
  const char     *skipReason;

  int             sourceChannels;   /* channels read from the accessor */
  int             outChannels;      /* channels written (differs for FuMa) */
  int             srate;
  double          audioStart, audioEnd;
  std::string     outPath;

  bool            written;
  int64_t         framesWritten;

  ConvertEntry() : item(NULL), take(NULL), skipReason(NULL), sourceChannels(0),
                   outChannels(0), srate(0), audioStart(0.0), audioEnd(0.0),
                   written(false), framesWritten(0) { name[0] = 0; }
};

struct ConvertJob
{
  std::vector<ConvertEntry> entries;
  size_t                    cur;

  bool                      fuma;        /* run blocks through the FuMa matrix */
  bool                      wavpack;
  float                     wavpackBits; /* > 0: WavPack hybrid (lossy) bits per
                                            sample and channel; 0 = lossless */
  bool                      overwrite;

  /* state for the entry in progress */
  AudioAccessor            *accessor;
  ambix_t                  *fh;
  std::vector<double>       matrix;    /* [outChannels][sourceChannels] */
  double                    pos;
  std::vector<double>       inBuf;
  std::vector<double>       outBuf;      /* only used when fuma */

  double                    totalSeconds;
  double                    doneSeconds;
  bool                      cancelled;
  bool                      finished;

  ConvertJob() : cur(0), fuma(false), wavpack(true), wavpackBits(0.f), overwrite(false),
                 accessor(NULL), fh(NULL), pos(0.0),
                 totalSeconds(0.0), doneSeconds(0.0),
                 cancelled(false), finished(false) {}
};

static const int kBlockFrames = 8192;

static bool EntryStillValid(const ConvertEntry &e)
{
  if (!ValidatePtr2) return true;
  return ValidatePtr2(NULL, (void *)e.item, "MediaItem*")
      && ValidatePtr2(NULL, (void *)e.take, "MediaItem_Take*");
}

/* Close whatever the current entry has open. `keep` false means the output is
 * being abandoned — remove the half-written file rather than leaving a
 * truncated .ambix behind that looks like a successful conversion. */
static void JobCloseEntry(ConvertJob &job, bool keep)
{
  ConvertEntry &e = job.entries[job.cur];

  if (job.fh)
  {
    ambix_close(job.fh);
    job.fh = NULL;
    if (!keep && !e.outPath.empty()) remove(e.outPath.c_str());
  }
  job.matrix.clear();
  if (job.accessor) { DestroyAudioAccessor(job.accessor); job.accessor = NULL; }
}

static bool JobOpenEntry(ConvertJob &job)
{
  ConvertEntry &e = job.entries[job.cur];

  if (!EntryStillValid(e))
  {
    e.skipReason = "item was removed during conversion";
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
    e.skipReason = "empty";
    JobCloseEntry(job, false);
    return false;
  }

  if (job.fuma)
  {
    if (AmbixFuMaToAmbixMatrix(e.sourceChannels, job.matrix) != e.outChannels)
    {
      e.skipReason = "no FuMa matrix for this channel count";
      JobCloseEntry(job, false);
      return false;
    }
  }

  ambix_info_t ainfo;
  memset(&ainfo, 0, sizeof(ainfo));
  ainfo.fileformat    = AMBIX_BASIC;
  ainfo.ambichannels  = (uint32_t)e.outChannels;
  ainfo.extrachannels = 0;
  ainfo.samplerate    = (double)e.srate;
  /* Float32 keeps the conversion lossless for anything REAPER hands us and
   * avoids a clipping decision on material that may legitimately exceed 0 dBFS
   * (an ambisonic W channel routinely does). */
  ainfo.sampleformat  = AMBIX_SAMPLEFORMAT_FLOAT32;

  ambix_filemode_t mode = AMBIX_WRITE;
  if (job.wavpack) mode = (ambix_filemode_t)(mode | AMBIX_USE_WAVPACK);

  job.fh = ambix_open(e.outPath.c_str(), mode, &ainfo);
  if (!job.fh)
  {
    e.skipReason = "libambix could not create the output file";
    JobCloseEntry(job, false);
    return false;
  }

  /* Lossy mode is configured before the first write; the value was already
   * clamped into WavPack's range when the options were read. */
  if (job.wavpack && job.wavpackBits > 0.f &&
      ambix_set_wavpack_bitrate(job.fh, job.wavpackBits) != AMBIX_ERR_SUCCESS)
  {
    e.skipReason = "libambix refused the WavPack bit rate";
    JobCloseEntry(job, false);
    return false;
  }

  job.pos = e.audioStart;
  job.inBuf.resize((size_t)kBlockFrames * e.sourceChannels);
  if (job.fuma) job.outBuf.resize((size_t)kBlockFrames * e.outChannels);
  return true;
}

/* Advance by roughly `budgetSeconds` of CPU time. True once everything is
 * dealt with. */
static bool JobStep(ConvertJob &job, double budgetSeconds)
{
  const clock_t deadline = clock() + (clock_t)(budgetSeconds * (double)CLOCKS_PER_SEC);

  while (job.cur < job.entries.size())
  {
    ConvertEntry &e = job.entries[job.cur];

    if (e.skipReason) { ++job.cur; continue; }

    if (!job.accessor)
    {
      if (!JobOpenEntry(job)) { ++job.cur; continue; }
    }

    while (job.pos < e.audioEnd)
    {
      int frames = (int)((e.audioEnd - job.pos) * e.srate + 0.5);
      if (frames > kBlockFrames) frames = kBlockFrames;
      if (frames < 1) break;

      /* GetAudioAccessorSamples() stops writing past the item end, so clear
       * first rather than write stale samples into the file. */
      memset(&job.inBuf[0], 0, (size_t)frames * e.sourceChannels * sizeof(double));
      GetAudioAccessorSamples(job.accessor, e.srate, e.sourceChannels,
                              job.pos, frames, &job.inBuf[0]);

      const double *toWrite = &job.inBuf[0];
      if (job.fuma)
      {
        /* Both buffers are interleaved. The matrix has at most one non-zero
         * per row, so this is a permutation with gains rather than a real
         * matrix multiply, but writing it out in full keeps it obvious. */
        const int rows = e.outChannels, cols = e.sourceChannels;
        for (int f = 0; f < frames; ++f)
        {
          const double *in  = &job.inBuf[(size_t)f * cols];
          double       *out = &job.outBuf[(size_t)f * rows];
          for (int r = 0; r < rows; ++r)
          {
            double acc = 0.0;
            const double *row = &job.matrix[(size_t)r * cols];
            for (int c = 0; c < cols; ++c) acc += row[c] * in[c];
            out[r] = acc;
          }
        }
        toWrite = &job.outBuf[0];
      }

      if (ambix_writef_float64(job.fh, toWrite, NULL, frames) != frames)
      {
        e.skipReason = "write failed (disk full?)";
        JobCloseEntry(job, false);
        break;
      }
      e.framesWritten += frames;

      const double advanced = (double)frames / (double)e.srate;
      job.pos         += advanced;
      job.doneSeconds += advanced;

      if (clock() >= deadline) return false;   /* let the UI breathe */
    }

    if (!e.skipReason)
    {
      e.written = true;
      JobCloseEntry(job, true);
    }
    ++job.cur;
  }

  job.finished = true;
  return true;
}

static void JobAbort(ConvertJob &job)
{
  if (job.cur < job.entries.size()) JobCloseEntry(job, false);
}

/* ---------------------------------------------------------------------------
 * progress dialog (shares IDD_AMBIX_PROGRESS with the loudness actions)
 * -------------------------------------------------------------------------*/
#define AMBIX_CONVERT_TIMER 1
#define AMBIX_CONVERT_RANGE 1000

static WDL_DLGRET ConvertDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  ConvertJob *job = (ConvertJob *)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);

  switch (uMsg)
  {
    case WM_INITDIALOG:
      SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);
      SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETRANGE, 0,
                         MAKELPARAM(0, AMBIX_CONVERT_RANGE));
      SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETPOS, 0, 0);
      SetDlgItemText(hwndDlg, IDC_PROGRESS_ITEM, "Preparing...");
      SetTimer(hwndDlg, AMBIX_CONVERT_TIMER, 1, NULL);
    return 1;

    case WM_TIMER:
      if (wParam == AMBIX_CONVERT_TIMER && job)
      {
        const bool done = JobStep(*job, 0.05);

        if (job->cur < job->entries.size())
        {
          const ConvertEntry &e = job->entries[job->cur];
          char line[512];
          snprintf(line, sizeof(line), "Item %d of %d: %s",
                   (int)job->cur + 1, (int)job->entries.size(), e.name);
          SetDlgItemText(hwndDlg, IDC_PROGRESS_ITEM, line);

          if (job->fuma)
            snprintf(line, sizeof(line), "FuMa %d ch -> ambiX %d ch",
                     e.sourceChannels, e.outChannels);
          else
            snprintf(line, sizeof(line), "%d channels, ambisonic order %d",
                     e.sourceChannels, AmbisonicOrder(e.sourceChannels));
          SetDlgItemText(hwndDlg, IDC_PROGRESS_STATUS, line);
        }

        const double frac = (job->totalSeconds > 0.0)
                          ? (job->doneSeconds / job->totalSeconds) : 1.0;
        SendDlgItemMessage(hwndDlg, IDC_PROGRESS_BAR, PBM_SETPOS,
                           (WPARAM)(int)(frac * AMBIX_CONVERT_RANGE + 0.5), 0);

        if (done)
        {
          KillTimer(hwndDlg, AMBIX_CONVERT_TIMER);
          EndDialog(hwndDlg, 1);
        }
      }
    return 0;

    case WM_COMMAND:
      if (LOWORD(wParam) == IDCANCEL)
      {
        KillTimer(hwndDlg, AMBIX_CONVERT_TIMER);
        if (job)
        {
          job->cancelled = true;
          JobAbort(*job);
        }
        EndDialog(hwndDlg, 0);
      }
    return 0;

    case WM_DESTROY:
      KillTimer(hwndDlg, AMBIX_CONVERT_TIMER);
    return 0;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * shared setup
 * -------------------------------------------------------------------------*/

/* Read a y/n answer, defaulting to `fallback` on anything unrecognised. */
static bool ParseYesNo(const char *s, bool fallback)
{
  while (*s == ' ') ++s;
  if (*s == 'y' || *s == 'Y' || *s == '1') return true;
  if (*s == 'n' || *s == 'N' || *s == '0') return false;
  return fallback;
}

/* WavPack's hybrid bit rate in bits per sample. Anything at or below zero
 * means lossless; a positive value is clamped into the range the encoder
 * accepts (the wavpack CLI's 2.0 .. 23.9). */
static float ParseBitsPerSample(const char *s)
{
  const double v = atof(s);
  if (v <= 0.0) return 0.f;
  if (v < 2.0)  return 2.f;
  if (v > 23.9) return 23.9f;
  return (float)v;
}

/* "uncompressed CAF", "WavPack lossless" or "WavPack lossy 4.0 bit/sample". */
static void DescribeCompression(const ConvertJob &job, char *buf, size_t n)
{
  if (!job.wavpack)             snprintf(buf, n, "uncompressed CAF");
  else if (job.wavpackBits > 0) snprintf(buf, n, "WavPack lossy %.1f bit/sample", job.wavpackBits);
  else                          snprintf(buf, n, "WavPack lossless");
}

/* GetUserInputs() hands back the fields comma-separated. Splits in place. */
static void SplitCsv(char *csv, const char **fields, int count)
{
  for (int i = 0; i < count; ++i) fields[i] = "";
  int i = 0;
  fields[0] = csv;
  for (char *p = csv; *p && i < count - 1; ++p)
  {
    if (*p == ',') { *p = 0; fields[++i] = p + 1; }
  }
}

/* Fill in everything the conversion of one item needs, without reading audio.
 * `fuma` selects which channel-count rule applies. */
static void BuildEntry(ConvertEntry &e, MediaItem *item, int index, bool fuma,
                       bool overwrite, const std::string &projectDir)
{
  e.item     = item;
  e.take     = GetActiveTake(item);
  e.skipReason = NULL;

  const char *name = (e.take && GetTakeName) ? GetTakeName(e.take) : NULL;
  if (name && *name) snprintf(e.name, sizeof(e.name), "%s", name);
  else               snprintf(e.name, sizeof(e.name), "item %d", index + 1);

  if (!e.take)                                              { e.skipReason = "no active take";  return; }
  if (((int)GetMediaItemInfo_Value(item, "C_LOCK")) & 1)    { e.skipReason = "item locked";     return; }

  PCM_source *source = GetMediaItemTake_Source(e.take);
  if (!source)                                              { e.skipReason = "no source";       return; }

  e.sourceChannels = GetMediaSourceNumChannels(source);
  if (e.sourceChannels < 1)                                 { e.skipReason = "not audio";       return; }

  if (fuma)
  {
    e.outChannels = AmbixFuMaTargetChannels(e.sourceChannels);
    if (!e.outChannels)
    {
      /* FuMa stops at third order, and only certain reduced sets exist. */
      e.skipReason = "not a FuMa channel count (1, 3, 4, 5, 6, 7, 8, 9, 11 or 16)";
      return;
    }
  }
  else
  {
    if (AmbisonicOrder(e.sourceChannels) < 0)
    {
      /* ambiX BASIC stores a complete (N+1)^2 set and nothing else. */
      e.skipReason = "channel count is not (N+1)^2, so it is not an ambiX set";
      return;
    }
    e.outChannels = e.sourceChannels;
  }

  e.srate = GetMediaSourceSampleRate(source);
  if (e.srate < 8000) e.srate = 48000;

  /* Output goes next to the source media file; a source with no file on disk
   * (a click source, a reversed/section wrapper REAPER declines to name) falls
   * back to the project directory. */
  char srcfile[2048];
  srcfile[0] = 0;
  if (GetMediaSourceFileName) GetMediaSourceFileName(source, srcfile, sizeof(srcfile));

  std::string dir = DirectoryOf(srcfile);
  if (dir.empty()) dir = projectDir;
  if (dir.empty())
  {
    e.skipReason = "no place to write (unsaved project and no source file)";
    return;
  }

  std::string base = e.name;
  if (base.empty() || base == "item") base = BaseNameNoExt(srcfile);
  SanitizeBaseName(base);
  if (fuma) base += "-ambix";

  if (!BuildOutputPath(dir, base, overwrite, e.outPath))
    e.skipReason = "could not find a free output filename";
}

/* Gather the selection, run the batch behind the progress dialog. False means
 * the caller should return without reporting. */
static bool GatherAndConvert(ConvertJob &job, int numSelected, bool fuma,
                             const char *title)
{
  char projectPath[2048];
  projectPath[0] = 0;
  if (GetProjectPath) GetProjectPath(projectPath, sizeof(projectPath));
  const std::string projectDir = projectPath;

  job.fuma = fuma;
  job.entries.resize((size_t)numSelected);

  size_t convertible = 0;
  for (int i = 0; i < numSelected; ++i)
  {
    MediaItem *item = GetSelectedMediaItem(NULL, i);
    ConvertEntry &e = job.entries[(size_t)i];

    if (!item)
    {
      e.skipReason = "item disappeared";
      snprintf(e.name, sizeof(e.name), "item %d", i + 1);
      continue;
    }

    BuildEntry(e, item, i, fuma, job.overwrite, projectDir);
    if (!e.skipReason)
    {
      ++convertible;
      job.totalSeconds += GetMediaItemInfo_Value(item, "D_LENGTH");
    }
  }

  if (!convertible)
  {
    ShowMessageBox(fuma
      ? "None of the selected items could be converted.\n\n"
        "FuMa is defined up to third order, so a take needs 1, 3, 4, 5, 6, 7,\n"
        "8, 9, 11 or 16 channels."
      : "None of the selected items could be converted.\n\n"
        "An ambiX file stores a complete ambisonic set, so a take needs\n"
        "(N+1)^2 channels: 4, 9, 16, 25, 36, ...",
      title, 0);
    return false;
  }

  const int completed = (int)DialogBoxParam(g_hInst, MAKEINTRESOURCE(IDD_AMBIX_PROGRESS),
                                            GetMainHwnd ? GetMainHwnd() : NULL,
                                            ConvertDlgProc, (LPARAM)&job);
  return completed && !job.cancelled;
}

/* Ask for the options both actions share. False if cancelled. */
static bool AskOptions(ConvertJob &job, bool fuma, bool *addTakeOut)
{
  const char *storedWp = GetExtState(EXTSTATE_SECTION, KEY_WAVPACK);
  const char *storedBt = GetExtState(EXTSTATE_SECTION, KEY_WAVPACK_BITS);
  const char *storedOw = GetExtState(EXTSTATE_SECTION, KEY_OVERWRITE);
  const char *storedAt = GetExtState(EXTSTATE_SECTION, KEY_ADDTAKE);

  const int numFields = fuma ? 4 : 3;
  char vals[256];
  if (fuma)
    snprintf(vals, sizeof(vals), "%s,%s,%s,%s",
             (storedWp && *storedWp) ? storedWp : "y",
             (storedBt && *storedBt) ? storedBt : "0",
             (storedOw && *storedOw) ? storedOw : "n",
             (storedAt && *storedAt) ? storedAt : "y");
  else
    snprintf(vals, sizeof(vals), "%s,%s,%s",
             (storedWp && *storedWp) ? storedWp : "y",
             (storedBt && *storedBt) ? storedBt : "0",
             (storedOw && *storedOw) ? storedOw : "n");

  const char *captions = fuma
    ? "WavPack compression (y/n):,WavPack lossy bits/sample (0 = lossless, 2-23.9):,"
      "Overwrite existing files (y/n):,Add result as a new take (y/n):,extrawidth=60"
    : "WavPack compression (y/n):,WavPack lossy bits/sample (0 = lossless, 2-23.9):,"
      "Overwrite existing files (y/n):,extrawidth=60";

  if (!GetUserInputs(fuma ? FUMA_TITLE : CONVERT_TITLE, numFields,
                     captions, vals, sizeof(vals)))
    return false;

  const char *fields[4];
  SplitCsv(vals, fields, numFields);

  job.wavpack     = ParseYesNo(fields[0], true);
  job.wavpackBits = ParseBitsPerSample(fields[1]);
  job.overwrite   = ParseYesNo(fields[2], false);
  if (addTakeOut) *addTakeOut = fuma ? ParseYesNo(fields[3], true) : false;

  char bits[32];
  snprintf(bits, sizeof(bits), "%g", (double)job.wavpackBits);

  SetExtState(EXTSTATE_SECTION, KEY_WAVPACK,      job.wavpack   ? "y" : "n", true);
  SetExtState(EXTSTATE_SECTION, KEY_WAVPACK_BITS, bits, true);
  SetExtState(EXTSTATE_SECTION, KEY_OVERWRITE,    job.overwrite ? "y" : "n", true);
  if (fuma && addTakeOut)
    SetExtState(EXTSTATE_SECTION, KEY_ADDTAKE, *addTakeOut ? "y" : "n", true);

  return true;
}

/* Shared reporting. Returns how many files were written. */
static int ReportJob(const ConvertJob &job, const char *heading)
{
  char line[3072];
  snprintf(line, sizeof(line), "%s\n", heading);
  ShowConsoleMsg(line);

  int written = 0, skipped = 0;
  for (size_t i = 0; i < job.entries.size(); ++i)
  {
    const ConvertEntry &e = job.entries[i];
    if (e.written)
    {
      snprintf(line, sizeof(line), "  %s: %.1f s, %d ch -> %s\n",
               e.name, (double)e.framesWritten / (double)(e.srate ? e.srate : 1),
               e.outChannels, e.outPath.c_str());
      ++written;
    }
    else
    {
      snprintf(line, sizeof(line), "  %s: skipped (%s)\n", e.name,
               e.skipReason ? e.skipReason : "unknown");
      ++skipped;
    }
    ShowConsoleMsg(line);
  }

  snprintf(line, sizeof(line), "ambiX: %d file%s written, %d skipped\n\n",
           written, written == 1 ? "" : "s", skipped);
  ShowConsoleMsg(line);
  return written;
}

/* ---------------------------------------------------------------------------
 * action: convert to .ambix
 * -------------------------------------------------------------------------*/

static void ConvertSelectedItemsToAmbix()
{
  const int numSelected = CountSelectedMediaItems(NULL);
  if (numSelected < 1)
  {
    ShowMessageBox("Select the media items you want written out as .ambix files.",
                   CONVERT_TITLE, 0);
    return;
  }

  ConvertJob job;
  if (!AskOptions(job, false, NULL)) return;

  if (!GatherAndConvert(job, numSelected, false, CONVERT_TITLE))
  {
    if (job.cancelled)
      ShowConsoleMsg("ambiX: conversion cancelled, partial file removed\n\n");
    return;
  }

  char compression[64], heading[256];
  DescribeCompression(job, compression, sizeof(compression));
  snprintf(heading, sizeof(heading), "ambiX: converting %d item%s to .ambix (%s)",
           numSelected, numSelected == 1 ? "" : "s", compression);
  ReportJob(job, heading);
  /* Files were written; the project itself is untouched, so no undo point. */
}

/* ---------------------------------------------------------------------------
 * action: convert from FuMa
 * -------------------------------------------------------------------------*/

static void ConvertSelectedItemsFromFuMa()
{
  const int numSelected = CountSelectedMediaItems(NULL);
  if (numSelected < 1)
  {
    ShowMessageBox("Select the FuMa (B-format) media items you want converted "
                   "to the ambiX convention.",
                   FUMA_TITLE, 0);
    return;
  }

  bool addTake = true;
  ConvertJob job;
  if (!AskOptions(job, true, &addTake)) return;

  /* Adding takes needs API we may not have; fall back to writing files only
   * rather than half-doing it. */
  if (addTake && (!AddTakeToMediaItem || !PCM_Source_CreateFromFile ||
                  !SetMediaItemTake_Source))
    addTake = false;

  if (!GatherAndConvert(job, numSelected, true, FUMA_TITLE))
  {
    if (job.cancelled)
      ShowConsoleMsg("ambiX: FuMa conversion cancelled, partial file removed\n\n");
    return;
  }

  char compression[64], heading[256];
  DescribeCompression(job, compression, sizeof(compression));
  snprintf(heading, sizeof(heading),
           "ambiX: converting %d item%s from FuMa to ambiX (%s)",
           numSelected, numSelected == 1 ? "" : "s", compression);
  const int written = ReportJob(job, heading);

  if (!written || !addTake) return;

  /* The converted file goes on as an additional take rather than replacing
   * the original, so the FuMa source stays reachable from the take list. */
  Undo_BeginBlock();

  int added = 0;
  for (size_t i = 0; i < job.entries.size(); ++i)
  {
    const ConvertEntry &e = job.entries[i];
    if (!e.written || !EntryStillValid(e)) continue;

    PCM_source *src = PCM_Source_CreateFromFile(e.outPath.c_str());
    if (!src) continue;

    MediaItem_Take *take = AddTakeToMediaItem(e.item);
    if (!take) { continue; }

    if (!SetMediaItemTake_Source(take, src)) continue;

    if (GetSetMediaItemTakeInfo_String)
    {
      char takeName[300];
      snprintf(takeName, sizeof(takeName), "%s (ambiX)", e.name);
      GetSetMediaItemTakeInfo_String(take, "P_NAME", takeName, true);
    }
    ++added;
  }

  char line[256];
  snprintf(line, sizeof(line), "ambiX: %d converted take%s added\n\n",
           added, added == 1 ? "" : "s");
  ShowConsoleMsg(line);

  Undo_EndBlock("ambiX: Convert item(s) from FuMa to ambiX", UNDO_STATE_ITEMS);
  UpdateArrange();
}

/* ---------------------------------------------------------------------------
 * registration
 * -------------------------------------------------------------------------*/

static bool hookCommandProc(int command, int flag)
{
  if (g_convertCmd && command == g_convertCmd) { ConvertSelectedItemsToAmbix();  return true; }
  if (g_fumaCmd    && command == g_fumaCmd)    { ConvertSelectedItemsFromFuMa(); return true; }
  return false;
}

static gaccel_register_t g_convertAccel =
{
  { 0, 0, 0 },
  "ambiX: Convert selected item(s) to .ambix file(s)..."
};

static gaccel_register_t g_fumaAccel =
{
  { 0, 0, 0 },
  "ambiX: Convert selected item(s) from FuMa to ambiX..."
};

#define IMPAPI_OPT(x) if (!((*((void **)&(x)) = (void *)rec->GetFunc(#x)))) ++missing;

bool AmbixConvertActionsInit(reaper_plugin_info_t *rec)
{
  int missing = 0;

  IMPAPI_OPT(CountSelectedMediaItems);
  IMPAPI_OPT(GetSelectedMediaItem);
  IMPAPI_OPT(GetActiveTake);
  IMPAPI_OPT(GetMediaItemTake_Source);
  IMPAPI_OPT(GetMediaSourceNumChannels);
  IMPAPI_OPT(GetMediaSourceSampleRate);
  IMPAPI_OPT(GetMediaSourceFileName);
  IMPAPI_OPT(GetMediaItemInfo_Value);
  IMPAPI_OPT(CreateTakeAudioAccessor);
  IMPAPI_OPT(DestroyAudioAccessor);
  IMPAPI_OPT(GetAudioAccessorStartTime);
  IMPAPI_OPT(GetAudioAccessorEndTime);
  IMPAPI_OPT(GetAudioAccessorSamples);
  IMPAPI_OPT(GetUserInputs);
  IMPAPI_OPT(SetExtState);
  IMPAPI_OPT(GetExtState);
  IMPAPI_OPT(GetMainHwnd);
  IMPAPI_OPT(Undo_BeginBlock);
  IMPAPI_OPT(Undo_EndBlock);
  IMPAPI_OPT(UpdateArrange);

  if (missing) return false;   /* leave the rest of the plugin working */

  /* Nice-to-haves. */
  *((void **)&GetTakeName)                    = (void *)rec->GetFunc("GetTakeName");
  *((void **)&GetProjectPath)                 = (void *)rec->GetFunc("GetProjectPath");
  *((void **)&ValidatePtr2)                   = (void *)rec->GetFunc("ValidatePtr2");
  *((void **)&AddTakeToMediaItem)             = (void *)rec->GetFunc("AddTakeToMediaItem");
  *((void **)&PCM_Source_CreateFromFile)      = (void *)rec->GetFunc("PCM_Source_CreateFromFile");
  *((void **)&SetMediaItemTake_Source)        = (void *)rec->GetFunc("SetMediaItemTake_Source");
  *((void **)&GetSetMediaItemTakeInfo_String) = (void *)rec->GetFunc("GetSetMediaItemTakeInfo_String");

#ifdef _WIN32
  InitCommonControls();
#endif

  g_convertCmd = rec->Register("command_id", (void *)"AMBIX_CONVERT_ITEMS_TO_AMBIX");
  if (g_convertCmd)
  {
    g_convertAccel.accel.cmd = g_convertCmd;
    if (!rec->Register("gaccel", &g_convertAccel)) g_convertCmd = 0;
  }

  g_fumaCmd = rec->Register("command_id", (void *)"AMBIX_CONVERT_ITEMS_FROM_FUMA");
  if (g_fumaCmd)
  {
    g_fumaAccel.accel.cmd = g_fumaCmd;
    if (!rec->Register("gaccel", &g_fumaAccel)) g_fumaCmd = 0;
  }

  if (!g_convertCmd && !g_fumaCmd) return false;
  if (!rec->Register("hookcommand", (void *)hookCommandProc)) return false;

  return true;
}
