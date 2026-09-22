/* ============================================================================
 *  channelcount_action.cpp — "ambiX: Set channel count of selected track(s)
 *  and their sends..." action.
 *
 *  Sets the track channel count on every selected track and resizes the sends
 *  that run from those tracks' channel 1 to match, so an ambisonic bus and the
 *  encoders feeding it change order in one step. Working at first order and
 *  switching to fifth just before rendering saves a lot of CPU, and doing it
 *  by hand across a large session is tedious and easy to get wrong.
 *
 *  This is a native port of change_channel_count.py from the ambix plug-in
 *  suite (Matthias Kronlachner and Daryl Pierce). The Python original edited
 *  the track state chunk with regular expressions; REAPER has had proper
 *  accessors for both halves of the job for years, so this uses I_NCHAN and
 *  I_SRCCHAN/I_DSTCHAN instead. Shipping it inside the extension also means it
 *  installs with reaper_ambix and needs no Python interpreter.
 *
 *  No external dependencies: everything needed is in the REAPER API.
 * ==========================================================================*/

#ifdef _WIN32
#include <windows.h>
#else
#include "swell/swell.h"
#endif

#include "reaper_plugin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* imported by pcmsrc_ambix.cpp */
extern void (*ShowConsoleMsg)(const char *msg);
extern int  (*ShowMessageBox)(const char *msg, const char *title, int type);

/* ---------------------------------------------------------------------------
 * REAPER API imports (resolved in AmbixChannelCountInit)
 * -------------------------------------------------------------------------*/
static int          (*CountSelectedTracks)(ReaProject *proj);
static MediaTrack  *(*GetSelectedTrack)(ReaProject *proj, int seltrackidx);
static double       (*GetMediaTrackInfo_Value)(MediaTrack *tr, const char *parmname);
static bool         (*SetMediaTrackInfo_Value)(MediaTrack *tr, const char *parmname, double newvalue);
static int          (*GetTrackNumSends)(MediaTrack *tr, int category);
static double       (*GetTrackSendInfo_Value)(MediaTrack *tr, int category, int sendidx, const char *parmname);
static bool         (*SetTrackSendInfo_Value)(MediaTrack *tr, int category, int sendidx, const char *parmname, double newvalue);
static bool         (*GetUserInputs)(const char *title, int num_inputs, const char *captions_csv,
                                     char *retvals_csv, int retvals_csv_sz);
static void         (*SetExtState)(const char *section, const char *key, const char *value, bool persist);
static const char  *(*GetExtState)(const char *section, const char *key);
static void         (*Undo_BeginBlock)(void);
static void         (*Undo_EndBlock)(const char *descchange, int extraflags);
static void         (*TrackList_AdjustWindows)(bool isMinor);
static void         (*UpdateArrange)(void);

/* Optional: used only to label console output and to warn about undersized
 * send destinations. The action works without them. */
static bool         (*GetTrackName)(MediaTrack *track, char *bufOut, int bufOut_sz);
static void        *(*GetSetTrackSendInfo)(MediaTrack *tr, int category, int sendidx,
                                           const char *parmname, void *setNewValue);
static bool         (*ValidatePtr2)(ReaProject *proj, void *pointer, const char *ctypename);

#define EXTSTATE_SECTION "reaper_ambix"
#define EXTSTATE_KEY     "track_channel_count"

#define ACTION_TITLE "ambiX: Set track channel count"

/* REAPER 7 raised the per-track maximum from 64 to 128. Accepting up to 128 on
 * an older build is harmless — SetMediaTrackInfo_Value() clamps to whatever
 * the running REAPER supports. */
#define AMBIX_MAX_TRACK_CHANNELS 128

static int g_channelCountCmd = 0;

/* ---------------------------------------------------------------------------
 * send/receive source-channel encoding
 *
 * REAPER packs a send's audio source selection into a single integer:
 *
 *     bits 0-9   first channel, 0-based
 *     bits 10+   width field: 0 = stereo, 1 = mono, n >= 2 = 2n channels
 *
 * and -1 means "no audio at all" (a MIDI-only send). So a 36-channel send from
 * channel 1 is (36/2) << 10 == 18432, which is exactly what REAPER writes into
 * the AUXRECV line of a project file. I_DSTCHAN is the matching first channel
 * on the receiving side.
 * -------------------------------------------------------------------------*/

static int SendFirstChannel(int srcchan)  { return srcchan & 1023; }
static int SendWidthField(int srcchan)    { return srcchan >> 10; }

static int SendNumChannels(int srcchan)
{
  const int w = SendWidthField(srcchan);
  return w == 0 ? 2 : w == 1 ? 1 : w * 2;
}

/* Build an I_SRCCHAN for `nch` channels starting at channel 1. nch is always
 * even and >= 2 here; note that 2 channels is the width field 0, NOT 1 (which
 * would silently turn the send mono — a bug the Python original had). */
static int MakeSendSrcChan(int nch)
{
  return (nch == 2 ? 0 : nch / 2) << 10;
}

/* Which sends are ours to touch? The ones aligned at the front on both ends —
 * from the source track's channel 1 into the destination's channel 1, in
 * stereo or wider — which is the encoder-into-ambisonic-bus shape this action
 * exists for. A mono send and a send starting at a channel offset (a stereo
 * monitor tap off channels 5/6) are deliberate partial routings that have
 * nothing to do with the track's width, and keep exactly the width they have.
 *
 * Note that the send's CURRENT width is deliberately not part of the test. It
 * used to have to match the track's width before the action ran, which meant a
 * send narrower than its source track was never widened — so running the
 * action on a track that was already at the target count resized nothing at
 * all, and a track going 4 -> 36 left a stereo send on channel 1 behind. The
 * cost is that a partial routing off channel 1 — a 10-channel bed feed out of
 * a 128-channel track — is widened along with everything else; start it at a
 * channel offset to keep it out of this action's way. */
static bool SendFollowsTrackWidth(int srcchan, int dstchan)
{
  if (srcchan < 0) return false;                   /* MIDI-only send */
  if (SendFirstChannel(srcchan) != 0) return false;
  if (SendWidthField(srcchan) == 1) return false;  /* mono */
  return dstchan == 0;
}

/* ---------------------------------------------------------------------------
 * helpers
 * -------------------------------------------------------------------------*/

/* Ambisonic channel counts are the perfect squares (N+1)^2 — FOA=4, SOA=9,
 * TOA=16 ... 10th=121 — the same test loudness.cpp uses. Returns -1 for a
 * count that is not an ambisonic order. */
static int AmbisonicOrder(int nch)
{
  const int root = (int)lround(sqrt((double)nch));
  if (nch >= 4 && root * root == nch) return root - 1;
  return -1;
}

static void DescribeChannelCount(int nch, char *buf, int bufsz)
{
  const int order = AmbisonicOrder(nch);
  if (order >= 0)
    snprintf(buf, bufsz, "%d channels (ambisonic order %d)", nch, order);
  else
    snprintf(buf, bufsz, "%d channels", nch);
}

static void TrackLabel(MediaTrack *tr, int fallbackIndex, char *buf, int bufsz)
{
  if (GetTrackName && GetTrackName(tr, buf, bufsz) && buf[0]) return;
  snprintf(buf, bufsz, "track %d", fallbackIndex + 1);
}

/* The destination of a send, or NULL if we cannot get at it. ValidatePtr2()
 * keeps a surprise here harmless: an unusable pointer just means no warning. */
static MediaTrack *SendDestination(MediaTrack *tr, int category, int sendidx)
{
  if (!GetSetTrackSendInfo || !ValidatePtr2) return NULL;
  void *p = GetSetTrackSendInfo(tr, category, sendidx, "P_DESTTRACK", NULL);
  if (!p || !ValidatePtr2(NULL, p, "MediaTrack*")) return NULL;
  return (MediaTrack *)p;
}

/* ---------------------------------------------------------------------------
 * the action
 * -------------------------------------------------------------------------*/

static void SetSelectedTrackChannelCounts(void)
{
  const int numSelected = CountSelectedTracks(NULL);
  if (numSelected < 1)
  {
    ShowMessageBox("Select the tracks whose channel count you want to change.\n\n"
                   "For an ambisonic session that usually means the bus and "
                   "every encoder track feeding it, so their sends can be "
                   "resized along with the tracks.",
                   ACTION_TITLE, 0);
    return;
  }

  /* Channel count: remembered across sessions in reaper-extstate.ini. */
  char input[64];
  const char *stored = GetExtState(EXTSTATE_SECTION, EXTSTATE_KEY);
  if (stored && *stored)
    snprintf(input, sizeof(input), "%s", stored);
  else
    snprintf(input, sizeof(input), "16");

  char caption[128];
  snprintf(caption, sizeof(caption),
           "Track channels (2-%d):,extrawidth=60", AMBIX_MAX_TRACK_CHANNELS);

  if (!GetUserInputs(ACTION_TITLE, 1, caption, input, sizeof(input)))
    return;  /* cancelled */

  char *parseEnd = NULL;
  long requested = strtol(input, &parseEnd, 10);
  /* strtol() reports junk as 0, so check that it actually consumed digits. */
  if (parseEnd == input || requested < 2 || requested > AMBIX_MAX_TRACK_CHANNELS)
  {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Enter a channel count between 2 and %d.\n\n"
             "Ambisonic orders are 4, 9, 16, 25, 36, 49, 64, 81, 100 and 121 "
             "channels. REAPER track channel counts are even, so an odd one is "
             "rounded up to the next even number.",
             AMBIX_MAX_TRACK_CHANNELS);
    ShowMessageBox(msg, ACTION_TITLE, 0);
    return;
  }

  /* REAPER only has even track channel counts, so an ambisonic order lands on
   * the next even number up: 9 -> 10, 25 -> 26. Rounding up rather than down
   * keeps every ambisonic channel, at the cost of one unused channel. */
  int nch = (int)requested;
  const bool roundedUp = (nch & 1) != 0;
  if (roundedUp) ++nch;

  SetExtState(EXTSTATE_SECTION, EXTSTATE_KEY, input, true);

  Undo_BeginBlock();

  char line[1024];
  char desc[64];
  DescribeChannelCount((int)requested, desc, sizeof(desc));
  snprintf(line, sizeof(line), "ambiX: setting %d track%s to %s\n",
           numSelected, numSelected == 1 ? "" : "s", desc);
  ShowConsoleMsg(line);
  if (roundedUp)
  {
    snprintf(line, sizeof(line),
             "  note: REAPER track channel counts are even, using %d\n",
             nch);
    ShowConsoleMsg(line);
  }

  /* Snapshot the tracks and their current widths first: pass 1 overwrites
   * I_NCHAN, and the console line reports what each track came from. Anything
   * above 64 tracks spills onto the heap through the vector rather than
   * sitting on the stack. */
  std::vector<MediaTrack *> tracks((size_t)numSelected, (MediaTrack *)NULL);
  std::vector<int> oldChannels((size_t)numSelected, 0);

  for (int i = 0; i < numSelected; ++i)
  {
    MediaTrack *tr = GetSelectedTrack(NULL, i);
    tracks[(size_t)i] = tr;
    if (tr) oldChannels[(size_t)i] = (int)GetMediaTrackInfo_Value(tr, "I_NCHAN");
  }

  /* Pass 1: the tracks. Doing every track before any send means that when we
   * widen, the source channels already exist by the time a send asks for them;
   * and when we narrow, pass 2 overwrites whatever REAPER clamped a now-too-
   * wide send to. */
  for (int i = 0; i < numSelected; ++i)
    if (tracks[(size_t)i]) SetMediaTrackInfo_Value(tracks[(size_t)i], "I_NCHAN", (double)nch);

  /* Pass 2: the routing out of those tracks.
   *
   * Sends only (category 0), never receives: every send is also a receive on
   * the other end, so walking the sends of each selected track already covers
   * every routing whose SOURCE is changing width — and a receive from a track
   * the user did not select is one whose source is NOT changing width, so
   * resizing it would break the routing rather than follow it. (The Python
   * original walked receives instead, which is why it only worked when you
   * remembered to select the bus.)
   *
   * Hardware outputs (category > 0) are left alone as well: silently spreading
   * a track across more physical outputs is not this action's business. */
  const int newSrcChan = MakeSendSrcChan(nch);
  int tracksChanged = 0, routingChanged = 0, narrowDest = 0;

  for (int i = 0; i < numSelected; ++i)
  {
    MediaTrack *tr = tracks[(size_t)i];
    if (!tr) continue;

    char name[256];
    TrackLabel(tr, i, name, sizeof(name));

    int updated = 0, kept = 0;
    const int numSends = GetTrackNumSends(tr, 0);

    for (int s = 0; s < numSends; ++s)
    {
      const int srcchan = (int)GetTrackSendInfo_Value(tr, 0, s, "I_SRCCHAN");
      const int dstchan = (int)GetTrackSendInfo_Value(tr, 0, s, "I_DSTCHAN");

      if (!SendFollowsTrackWidth(srcchan, dstchan))
      {
        if (srcchan >= 0) ++kept;
        continue;
      }
      if (srcchan == newSrcChan) continue;  /* already the right width */

      if (!SetTrackSendInfo_Value(tr, 0, s, "I_SRCCHAN", (double)newSrcChan))
        continue;
      ++updated;

      /* A send into a track narrower than the new width drops the channels
       * past its end. That is almost always a track the user forgot to
       * select, so it is worth saying out loud. */
      MediaTrack *dest = SendDestination(tr, 0, s);
      const int destChannels = dest ? (int)GetMediaTrackInfo_Value(dest, "I_NCHAN") : nch;
      if (dest && destChannels < nch)
      {
        char destName[256];
        TrackLabel(dest, -1, destName, sizeof(destName));
        snprintf(line, sizeof(line),
                 "  warning: \"%s\" now sends %d ch into \"%s\", which has only %d\n",
                 name, nch, destName, destChannels);
        ShowConsoleMsg(line);
        ++narrowDest;
      }
    }

    snprintf(line, sizeof(line), "  %s: %d -> %d ch, %d send%s resized%s\n",
             name, oldChannels[(size_t)i], nch, updated, updated == 1 ? "" : "s",
             kept ? ", partial routings left alone" : "");
    ShowConsoleMsg(line);

    ++tracksChanged;
    routingChanged += updated;
  }

  snprintf(line, sizeof(line),
           "ambiX: %d track%s set to %d channels, %d send%s resized\n",
           tracksChanged, tracksChanged == 1 ? "" : "s", nch,
           routingChanged, routingChanged == 1 ? "" : "s");
  ShowConsoleMsg(line);

  if (narrowDest)
  {
    snprintf(line, sizeof(line),
             "ambiX: %d send%s wider than the destination track - select the "
             "destination%s too and run the action again\n",
             narrowDest, narrowDest == 1 ? " is" : "s are",
             narrowDest == 1 ? "" : "s");
    ShowConsoleMsg(line);
  }
  ShowConsoleMsg("\n");

  Undo_EndBlock(ACTION_TITLE, UNDO_STATE_TRACKCFG);
  TrackList_AdjustWindows(false);
  UpdateArrange();
}

static bool hookCommandProc(int command, int flag)
{
  if (!g_channelCountCmd || command != g_channelCountCmd) return false;
  SetSelectedTrackChannelCounts();
  return true;
}

static gaccel_register_t g_channelCountAccel =
{
  { 0, 0, 0 },
  "ambiX: Set channel count of selected track(s) and their sends..."
};

/* ---------------------------------------------------------------------------
 * registration
 * -------------------------------------------------------------------------*/
#define IMPAPI_OPT(x) if (!((*((void **)&(x)) = (void *)rec->GetFunc(#x)))) ++missing;

bool AmbixChannelCountInit(reaper_plugin_info_t *rec)
{
  int missing = 0;

  IMPAPI_OPT(CountSelectedTracks);
  IMPAPI_OPT(GetSelectedTrack);
  IMPAPI_OPT(GetMediaTrackInfo_Value);
  IMPAPI_OPT(SetMediaTrackInfo_Value);
  IMPAPI_OPT(GetTrackNumSends);
  IMPAPI_OPT(GetTrackSendInfo_Value);
  IMPAPI_OPT(SetTrackSendInfo_Value);
  IMPAPI_OPT(GetUserInputs);
  IMPAPI_OPT(SetExtState);
  IMPAPI_OPT(GetExtState);
  IMPAPI_OPT(Undo_BeginBlock);
  IMPAPI_OPT(Undo_EndBlock);
  IMPAPI_OPT(TrackList_AdjustWindows);
  IMPAPI_OPT(UpdateArrange);

  if (missing) return false;  /* leave the rest of the plugin working */

  /* Nice-to-haves: the action still works without them. */
  *((void **)&GetTrackName)         = (void *)rec->GetFunc("GetTrackName");
  *((void **)&GetSetTrackSendInfo)  = (void *)rec->GetFunc("GetSetTrackSendInfo");
  *((void **)&ValidatePtr2)         = (void *)rec->GetFunc("ValidatePtr2");

  g_channelCountCmd = rec->Register("command_id", (void *)"AMBIX_SET_TRACK_CHANNEL_COUNT");
  if (!g_channelCountCmd) return false;

  g_channelCountAccel.accel.cmd = g_channelCountCmd;
  if (!rec->Register("gaccel", &g_channelCountAccel)) return false;
  if (!rec->Register("hookcommand", (void *)hookCommandProc)) return false;

  return true;
}
