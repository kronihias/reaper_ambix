reaper_ambix
============

Tools for REAPER, all installable from one [ReaPack](https://reapack.com/)
repository. The ambisonics extension the repo is named after, plus whatever
else turns out to be worth keeping around.

| Package | What it is |
| --- | --- |
| **reaper_ambix** | Native extension: `.ambix` file read/write plus five actions ([below](#actions)) |
| **REAPER Live** | Touch mixer and transport for REAPER's web server ([below](#reaper-live-web-interface)) |

See [ReaPack](#reapack) for the one-time repository import; after that both
show up in *Browse packages* and can be installed independently.


reaper_ambix (extension)
------------------------

REAPER plug-in that adds read/write support for `.ambix` files following the
ambiX (Ambisonics eXchangeable) specification [1].

Output can be written uncompressed (CAF container) or WavPack-compressed,
either lossless or lossy (WavPack's hybrid mode, see
[Lossy WavPack](#lossy-wavpack)). Reading auto-detects the container from the
file's magic bytes, and the source properties of a loaded `.ambix` item say
which of the three it is.

It also adds five actions to REAPER's main action list — see
[Actions](#actions) below.

[1] C. Nachbar, F. Zotter, E. Deleflie, A. Sontacchi. *ambiX – A Suggested
Ambisonics Format.* Proceedings of the Ambisonics Symposium 2011, Lexington,
KY, June 2–3, 2011.
[ambisonics.iem.at](https://ambisonics.iem.at/proceedings-of-the-ambisonics-symposium-2011/ambix-a-suggested-ambisonics-format)


Actions
-------

The extension registers five actions in REAPER's main action list.

### ambiX: Normalize selected item(s) to target loudness (LUFS)...

It asks for a target level, measures the integrated loudness of each selected
item's active take, and sets the take volume so the item lands on that target.
The measured and applied values are printed to the ReaScript console, and the
whole run is a single undo point. The target is remembered between sessions.

Analysis runs behind a progress dialog and can be cancelled — cancelling
applies nothing. The work is done in small slices on the main thread rather
than on a worker, so REAPER stays responsive without any of the API being
called off-thread.

The measurement is ITU-R BS.1770-4 (K-weighting, 400 ms blocks at 75 % overlap,
absolute -70 LUFS and relative -10 LU gating), implemented locally in
[src/loudness.cpp](src/loudness.cpp) — no external library is involved.

Channel handling follows BS.1770-5 Annex 3:

| Source | Measured |
| --- | --- |
| Ambisonic (channel count is a perfect square ≥ 4: FOA 4, SOA 9, TOA 16, …) | W channel only, G = 1.00 |
| Up to 7.1.4 (12 ch, Dolby/SMPTE order) | LFE excluded, Ls/Rs at G = 1.41, everything else G = 1.00 |
| 9.1.4 (14 ch, Dolby/SMPTE order) | as above, plus Lss/Rss at G = 1.41 |
| Mono / stereo | G = 1.00 per channel |

Measuring only W for ambisonic material is both correct — per Peters & Epain
(AES 154th, 2023) applying BS.1770 to W matches a full loudspeaker rendering —
and much cheaper, since the other HOA channels never have to be read.

Only channels that carry weight are read at all. A fifth-order bed is 36
channels but costs one; an LFE is requested but never filtered; and a source
wider than the 12-channel surround layout is capped there instead of running
K-weighting over channels that contribute nothing.

Note that a 4-channel item is read as first-order ambisonics rather than quad
or LCRS, which is the useful default for this plugin but worth knowing if you
point the action at a non-ambisonic 4-channel file.

### ambiX: Measure loudness of selected item(s) (LUFS)

The same measurement as above, reported to the ReaScript console and changing
nothing. For more than one item it also prints the quietest, the loudest and
the spread, which is the number that matters when checking whether a batch of
deliverables is consistent.

REAPER has its own item loudness analysis, and it also gives peak and LRA — but
it has no ambisonic mode, so it measures every channel of a 36-channel bed
rather than W alone. This action reports exactly the quantity the normalize
action drives to a target, take gain and volume envelope included, so running
it after a normalize reads back the target you asked for.

### ambiX: Set channel count of selected track(s) and their sends...

Sets the track channel count on every selected track and resizes the sends that
carried those tracks' full width, so a whole encoder-into-bus chain changes
ambisonic order in one step. Working at first order and switching to fifth just
before rendering saves a lot of CPU; doing it by hand across a large session is
tedious and easy to get wrong.

Select the bus **and** every track feeding it, run the action, and enter the
channel count. Ambisonic orders are the perfect squares — 4, 9, 16, 25, 36, 49,
64, 81, 100, 121 — and REAPER only has even channel counts, so an odd order is
rounded up to the next even number. Up to 128 channels (REAPER 7; older
versions clamp to 64). The count is remembered between sessions and the whole
run is a single undo point.

A send is resized only if it carried its source track's entire width from
channel 1 into the destination's channel 1. Mono sends, sends starting at a
channel offset, and sends that only ever carried part of the track — a stereo
monitor tap, a 10-channel bed feed off a 128-channel track — keep the width
they have. Hardware outputs are never touched.

Only sends *out of* selected tracks are resized. A receive from a track you did
not select comes from a track whose width is not changing, so touching it would
break the routing rather than follow it. If a resized send now feeds a track
narrower than the new count, the console says so by name — that is usually a
track you forgot to select.

This is a native port of
[`change_channel_count.py`](https://github.com/kronihias/ambix/blob/master/reaper_tools/change_channel_count.py)
from the ambix plug-in suite (Matthias Kronlachner and Daryl Pierce). The Python
original needed a configured Python interpreter and edited the track state chunk
with regular expressions; this uses `I_NCHAN` and `I_SRCCHAN`/`I_DSTCHAN` and
ships inside the extension, so it installs with reaper_ambix and needs no
interpreter.

### ambiX: Convert selected item(s) to .ambix file(s)...

Writes each selected item out as an `.ambix` file, next to the item's source
media file and named after the take. The render dialog can also produce
`.ambix` per item (source *Selected media items*), and that is the right tool
when FX have to be baked in, since it runs the take FX chain and writes the
file directly. This action is for material that is already ambiX: it copies the
item pre-FX, needs no render settings, and puts each file next to its own
source rather than in one render directory.

What gets written is the **item**, not the whole source file: its own trimmed
extent, so two items cut out of one long recording become two files. It is the
material the item uses, though, not the item's contribution to a mix — take and
item volume, fades, envelopes and take FX all stay where they are, and a take's
playback rate and pitch adjustment are *not* baked in. A stretched item
converts to an unstretched file and keeps its rate, which is why the file can
come out longer than the item it came from, and why the conversion can be
undone. (The render dialog is the one that bakes everything in.) Existing files
are never clobbered silently — a `-1`, `-2` suffix is added unless you ask for
overwrite.

A take needs a complete ambisonic set, `(N+1)^2` channels, since that is what
the ambiX basic format stores. Container and compression come from a dropdown
offering the same presets the render dialog does, from uncompressed CAF through
WavPack lossless to WavPack's hybrid mode at a chosen bit rate (see
[Lossy WavPack](#lossy-wavpack)).

*After converting* decides what happens to the project: nothing, the result
added to each item as an extra take, or the item pointed straight at the new
file. Replacing keeps the item's gain, fades and playback rate, so it sounds
exactly as it did.

### ambiX: Convert selected item(s) from FuMa to ambiX...

Converts Furse-Malham (classic B-format) material to the ambiX convention —
ACN channel ordering, SN3D normalization — writing an `.ambix` file the same
way as the action above, with the same *After converting* choice. Adding the
result as an extra take is the default here: the FuMa original stays in the
take list, so nothing is destroyed.

FuMa is defined up to third order, so takes of 1, 3, 4, 5, 6, 7, 8, 9, 11 or 16
channels are accepted; the reduced sets (`WXY`, `WXYUV`, …) expand to the full
`(N+1)^2` set with the components FuMa does not carry left silent.

The conversion follows section 4.2.1 of the ambiX paper. It deliberately does
**not** use libambix's `AMBIX_MATRIX_FUMA`, which is wrong in two ways: it
emits the inverse of its own (correct) ordering table, so first-order X lands
in the ambiX Z slot, and it applies a Condon-Shortley `(-1)^m` sign that the
ambiX paper explicitly rejects. Its `AMBIX_MATRIX_TO_FUMA` counterpart is
inverted the same way, so a FuMa → ambiX → FuMa round trip is a clean identity
and libambix's own test suite does not catch it. See
[src/fuma.cpp](src/fuma.cpp) and [tests/test_fuma.cpp](tests/test_fuma.cpp).


REAPER Live (web interface)
---------------------------

A touch mixer and transport served by REAPER's built-in web server, meant for a
phone or tablet next to the desk rather than a desktop browser.

One strip per track plus the master — name, track colour, level meter and
fader — over a transport bar with go-to-start, stop, pause, play, record and
repeat, the play position and the time signature.

Three details that matter in use:

- **Padlocks for the things that can bite you.** Three of them sit together in
  the transport bar:

  | Lock | Default | Governs |
  | --- | --- | --- |
  | **Mute/Solo** | locked | every strip's MUTE and SOLO |
  | **Faders** | *unlocked* | every fader |
  | **Transport** | locked | play, stop, pause, record, repeat, go-to-start |

  Faders default open because riding levels is the usual reason the tablet is
  on the desk in the first place, and a fader cannot silence a channel outright
  the way a stray tap on MUTE or STOP can.

  Nothing is hidden when locked, only dimmed and made inert: which tracks are
  muted or soloed, and whether REAPER is rolling or armed, is exactly what you
  want to read at a glance even when you must not touch it.

- **Per-track fader locks.** Each strip header carries its own small padlock,
  for pinning the couple of channels that must not move — a playback stem, a
  safety mic — while the rest stay live. The global fader lock overrides them,
  so closing it locks everything regardless.

  Every choice is remembered per browser, so a tablet that lives on the desk
  keeps whatever you picked. Anything unexpected — a reload, a browser that
  refuses storage — falls back to the defaults above rather than to something
  more permissive.

  One caveat: per-track locks are stored by **track index**, because that is
  all the web API's `TRACK` reply gives us to key on. Inserting or deleting a
  track shifts the locks along with the numbering.

- **Faders drag relatively.** Putting a finger down never jumps the level to
  where you touched, which is the failure mode that makes most tablet mixers
  unusable live. Movement only counts past a small threshold, and the fader
  re-anchors there so it does not lurch. Double-tap resets to 0 dB.
- **A dead connection is visible.** REAPER's polling framework has no error
  callback, so a stalled server otherwise just looks like a frozen desk. If no
  reply arrives for two seconds the page shows a banner and dims the mixer,
  and clears it as soon as replies resume.

### Setting it up

ReaPack drops the page into `reaper_www_root/`, but REAPER still has to be told
to serve it:

1. *Options → Preferences → Control/OSC/web*
2. Add (or edit) a **Web browser interface**
3. Set a port, and choose `reaper_mixer_live.html` as the default web browser
   interface page
4. Open `http://<computer-ip>:<port>/` on the phone or tablet

Both devices need to be on the same network. The page pulls `main.js` from
REAPER itself, so there is nothing else to install.

The interface talks to REAPER over the plain-HTTP web API, which has no
authentication — anyone who can reach that port can drive the session. Keep it
on a network you trust.


Lossy WavPack
-------------

The compression choice in the render format options is one list: CAF
uncompressed, WavPack lossless, or WavPack lossy at 6, 4 or 3 bits per sample
and channel. The convert actions take the same choice as a number, with 0
meaning lossless.

Lossy here is WavPack's hybrid mode without a correction file. It is not a
psychoacoustic codec: it quantises the prediction residual so each channel is
stored at roughly the requested number of bits, which puts the noise floor
about 6 dB per bit below that channel's own level, with WavPack's dynamic
noise shaping on top. That property is what makes it usable for ambisonics.
The higher-order channels of a bed are quiet, and a noise floor that follows
each channel's level survives the decoder matrix, or a rotation, the way a
masking-based codec's does not. At 4 bits/sample the noise is a signal-following
hiss about 24 dB down per channel; 6 bits/sample is roughly 36 dB.

For a 36-channel float32 bed at 48 kHz, the raw rate is 6.9 MB/s, lossless
typically lands at 3 to 4 MB/s depending on the material, and hybrid at 4
bits/sample is 0.86 MB/s. There is no way back to lossless from a hybrid file,
so keep the lossless master. The source properties of a loaded item report
`WavPack lossy (4.1 bit/sample)` or `WavPack lossless`, so a delivery can be
checked without leaving REAPER.


Screenshots
-----------

Render settings (REAPER's render dialog) — pick ambisonic order, format,
sample format and the container/compression:

![ambiX render settings — BASIC format](docs/screenshot_1.png)

EXTENDED format with an adaptor matrix — reduces the stored channel count
for restricted geometries (e.g. upper hemisphere) while keeping a full
periphonic decoder available via the embedded matrix:

![ambiX render settings — EXTENDED format with adaptor matrix](docs/screenshot_3.png)

File properties for a loaded `.ambix` media item — shows the container,
ambisonic order, channel layout and adapter matrix info:

![ambix file properties](docs/screenshot_2.png)


Releases
--------

Signed installers for Windows (`.exe`, x64 and ARM64), macOS (`.pkg`, universal)
and a Linux tarball (x86_64 and aarch64) are built automatically and published in
[GitHub Releases](https://github.com/kronihias/reaper_ambix/releases).


ReaPack
-------

Everything here installs through [ReaPack](https://reapack.com/). In REAPER
choose *Extensions → ReaPack → Import repositories…* and paste:

```
https://github.com/kronihias/reaper_ambix/raw/master/index.xml
```

Then open *Extensions → ReaPack → Browse packages*:

- **reaper_ambix** under *Extensions* — the plug-in binary, downloaded straight
  from the matching GitHub release asset for your platform. Restart REAPER
  after installing or updating.
- **REAPER Live** under *WebInterfaces* — installed into `reaper_www_root/`.
  Needs the one-time setup described [above](#setting-it-up).

*Synchronize packages* only updates what you already have, so anything added
here later still has to be picked up from *Browse packages* once. To have new
packages install themselves instead, right-click this repository in *Manage
repositories* and set **Install new packages → When synchronizing**. That is a
local ReaPack preference, not something the repository can set for you.

The package index ([index.xml](index.xml)) is generated from the metadata files
([Extensions/reaper_ambix.ext](Extensions/reaper_ambix.ext),
[WebInterfaces/reaper_mixer_live.www](WebInterfaces/reaper_mixer_live.www)) by
`scripts/reapack_index.sh` (cfillion's `reapack-index` tool). A package's
version lives in its own metadata file, so they release independently — only
the extension is tied to `VERSION` and the GitHub release tags.


Cutting a release
-----------------

1. Bump the `VERSION` file **and** the `@version` in
   [Extensions/reaper_ambix.ext](Extensions/reaper_ambix.ext) (keep them in
   sync), update the `@changelog`, and commit.
2. Run `./scripts/reapack_index.sh` to regenerate `index.xml`, and commit it.
3. Publish a GitHub release tagged `vX.Y.Z` (matching `VERSION`). Publishing
   triggers the build + signing workflow, which builds the installers **and**
   uploads the raw per-platform binaries (`.dll`/`.dylib`/`.so`) that ReaPack
   pulls from the release assets.

Code-signing setup is documented in [.github/CODE_SIGNING.md](.github/CODE_SIGNING.md).


Building from source
--------------------

Requirements:

- CMake 3.13+ and a C++ toolchain
- macOS: Xcode command-line tools
- Windows: Visual Studio 2022 (Desktop development with C++)
- Linux: `build-essential`

`libambix` (with native CAF backend) and `WavPack` are vendored as git
submodules and linked statically — no external audio libraries required.

```
git clone --recursive https://github.com/kronihias/reaper_ambix
cd reaper_ambix
./scripts/setup.sh        # init submodules + cmake configure
cmake --build build-dev   # writes the plug-in straight into REAPER's UserPlugins
```

For signed/notarized installer builds, see [scripts/build_osx.sh](scripts/build_osx.sh),
[scripts/build_win.bat](scripts/build_win.bat) and
[scripts/build_linux.sh](scripts/build_linux.sh).

**Note:** `scripts/setup.sh` configures the dev tree with
`REAPER_AMBIX_INSTALL_USER_PLUGINS=ON`, so a rebuild writes into the *user*
plugin folder (`~/Library/Application Support/REAPER/UserPlugins` on macOS,
`~/.config/REAPER/UserPlugins` on Linux). The macOS installer writes into the
*system* folder (`/Library/Application Support/REAPER/UserPlugins`) instead.
REAPER scans both and the user copy wins on identical filenames, so **delete
the dev build before testing an installer or ReaPack install** — otherwise it
silently shadows the packaged one and you keep running the old binary.

The option itself defaults to **OFF** precisely so that no build ends up in a
folder REAPER loads from unless you asked for it; only the dev tree opts in.


Tests
-----

Two suites run without REAPER.

[tests/test_loudness.cpp](tests/test_loudness.cpp) covers the loudness
measurement: the EBU Tech 3341
integrated-loudness compliance cases, the BS.1770-5 channel weightings, the
ambisonic layout detection, sample-rate independence, and a check that the
derived K-weighting coefficients reproduce the values tabulated in BS.1770-4 at
48 kHz.

[tests/test_fuma.cpp](tests/test_fuma.cpp) covers the FuMa conversion: the
paper's first-order matrix entry by entry, the ordering and maxN gains for
orders 2 and 3, the reduced FuMa sets, the channel counts that must be
rejected, and the absence of any negative coefficient. It exists because the
obvious shortcut — libambix's `AMBIX_MATRIX_FUMA` — is wrong and its own
round-trip test does not notice.

`REAPER_AMBIX_BUILD_PLUGIN=OFF` skips the extension itself, so the tests build
without the vendored submodules — a plain `git clone` is enough:

```
cmake -S . -B build-tests -DREAPER_AMBIX_BUILD_TESTS=ON -DREAPER_AMBIX_BUILD_PLUGIN=OFF
cmake --build build-tests --target test_loudness
ctest --test-dir build-tests --output-on-failure
```

CI runs this on every push, and a release build will not package anything until
it passes.


Credits
-------

- *libambix* by IOhannes m zmölnig (LGPL) — https://git.iem.at/ambisonics/libambix
- *WavPack* by David Bryant — https://github.com/dbry/wavpack
- Originally based on Xenakios' `reaper_libsndfilewrapper`


Author
------

2014–2026 Matthias Kronlachner

m.kronlachner (ät) gmail.com — https://www.matthiaskronlachner.com
