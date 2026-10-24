# Azeroth Plex

A native Plex client for the WarcraftXL 3.3.5a build-12340 client. Its Plex
browser and playback controls render in the in-game Azeroth Plex panel. A
hidden 64-bit worker handles libmpv playback and mirrors the current video onto
a client-local, movable world-space screen.

## Install

1. Use a WarcraftXL-enabled 32-bit build-12340 client.
2. Install the release containing `wxl-azeroth-plex.dll`,
   `wxl-video-host.exe`, `libmpv-2.dll`, `yt-dlp.exe`, `wxl.json`, and this
   documentation, all in `Extensions\wxl-azeroth-plex`.
3. Restart the client when WarcraftXL requests it.
4. Press **Insert**, open **Video Cinema**, and choose **Open Plex player inside
   WoW**.

No Microsoft Edge WebView2 runtime is required.

## First login and playback

1. Choose **Open Plex sign-in** in the in-game Azeroth Plex panel.
2. Complete Plex PIN authorization in the system browser.
3. Select a reachable Plex server and library.
4. Choose a movie, show, season, or episode and press **Play**.
5. Use the in-game panel for browsing, search, playback, tracks, and subtitles.
   Hiding the panel leaves playback and audio running.

## Plex navigation

- **Watchlist** appears above the Plex library list on the Home screen;
- selecting a show opens its seasons, and selecting a season opens its episodes;
- large movie, show, and child-item lists expose Previous page and Next page;
- movies, episodes, tracks, playlists, and other media leaves open their metadata
  view before playback when supported.

Poster paths are retained from Plex while navigating. WarcraftXL ABI 1.1 does
not expose an in-game image control, so live poster rendering requires a future
core UI image service; playback remains visible on the placed world-space screen.

The pinned x64 libmpv build includes the normal FFmpeg audio/video decoders;
separate codec DLLs are not required. If a direct stream ends early, the worker
reports the mpv error and retries through Plex transcoding when possible.

## Watch Together

AzerothPlex includes a state-only Watch Together relay. Every viewer streams the
media directly from their own Plex server; Plex credentials, authenticated media
URLs, decoded frames, and media files are never sent through the relay.

For local development, start the relay from `relay/` and leave the default
`ws://127.0.0.1:17777/ws` endpoint in place. In the in-game Plex panel, open
**Watch Together**, create a private room, then share its short `AZ-....` code.
Guests resolve the host's media against their own Plex library and follow the
relay's authoritative playback state.

Watch Together currently includes host/shared/request-control modes, optional
semantic audio/subtitle matching, a shared queue with independent **Host managed**
and **Everyone may add** policies, all-ready synchronized starts, host migration,
reconnect grace, disconnect policies, shared cinema layout, relay RTT/drift
diagnostics, and a compact mini-player. The mini-player includes 10-second seek
controls and its enabled state is saved locally.

### Live Broadcast (experimental, video only)

A room host can press **Go live** to broadcast the player screen and its audio
to the room without any playback synchronization. Video is H.264-encoded, audio
is captured with WASAPI process loopback (only AzerothPlex audio, not other
system sounds) and AAC-encoded with Windows Media Foundation. The relay
forwards both as binary WebSocket messages to room members who pressed
**Watch live**, and decodes them back into the client-local world-space screen
and the default audio device. Viewers need no Plex account or Plex access to
the broadcast media. The newest video frame wins when a viewer falls behind,
while audio keeps a short bounded backlog; local volume controls the broadcast
audio too. This does not capture the WoW desktop, only the AzerothPlex player
screen; audio broadcast is a new and experimental path and will keep improving.

Public room discovery is implemented but intentionally disabled by default. The
roadmap requires authentication plus abuse/moderation controls before public
rooms are considered production-ready. See `relay/README.md` for the explicit
development opt-in.

The Plex token is protected with Windows DPAPI under
`%LOCALAPPDATA%\WarcraftXL\plex-player`. The account password is never entered
into AzerothPlex and tokens are not sent through WarcraftXL shared memory.

## CyTube channels and yt-dlp

The in-game **Azeroth Plex** panel can join a CyTube channel (`cytu.be/r/<channel>`)
and play its current media in sync with the channel. Direct video files play
as they are. YouTube, Vimeo, Twitch, Google Drive and similar sites play
through mpv's ytdl hook, which needs [yt-dlp](https://github.com/yt-dlp/yt-dlp):

- The release includes `yt-dlp.exe` beside `wxl-video-host.exe`, and the
  helper uses that copy first. Without it, the helper uses the first of
  `yt-dlp`, `yt-dlp_x86` or `youtube-dl` that Windows finds the way mpv's hook
  would: the helper's folder, the system folders, then `PATH` (a pip install,
  for example).
- If there is none, the panel says so: *YouTube (and Vimeo, Twitch, ...) needs
  yt-dlp: put yt-dlp.exe next to wxl-video-host.exe in
  Extensions\wxl-azeroth-plex.* That item is skipped, and the channel's direct
  files keep playing. **Check for yt-dlp again** picks up a newly added copy,
  and the skipped item then starts where the channel is.
- The panel shows yt-dlp's version and its age. YouTube breaks old yt-dlp
  versions, so past 30 days the panel asks for an update.
- **Update yt-dlp** runs `yt-dlp -U` on the copy beside the helper, in the
  background, and shows the result. That is yt-dlp's own updater, which
  downloads its latest release from GitHub. It runs only when the button is
  pressed; nothing updates by itself.
- A pip install updates with `python -m pip install -U yt-dlp`. Other installs
  update the way they were installed.

## World-space screen

The screen is client-local and is not a server GameObject or multiplayer
object. In the in-game **Video Cinema** panel:

- choose **Place / move screen in front of me**;
- change **Screen width** to resize the 16:9 screen;
- rotate, raise, lower, hide, or show the screen;
- enable character/world depth occlusion and adjust its depth pull (yards toward the camera);
- enable distance-based audio and tune its range.
- enable **Pin Plex screen to display** to keep the video fixed in the top-right
  of the game viewport while the character moves; use **Pinned size**, **Pinned
  X**, and **Pinned Y** to reposition or resize it. Dragging the video and its
  lower-right corner is also supported when the game cursor is available.

Placement is saved to
`Extensions/wxl-azeroth-plex/world-screen.tsv` and restored per client.

## Requirements and limits

- WarcraftXL ABI 1.1 and the 32-bit build-12340 client;
- internet access for Plex authorization and server metadata;
- a Plex account with access to at least one reachable server;
- server-side Plex transcoding may be used when direct play is unavailable.

Media is played from Plex. AzerothPlex does not download or cache media. The
only exception to the state-only relay rule is the explicit **Go live** opt-in
described above, which streams the host's already-decoded player screen to room
members over the relay; it remains disabled for anyone who does not join the
room and press **Watch live**. The world-space screen is client-local.

For Plex `*.plex.direct` connections, login and metadata remain HTTPS, but the
native media stream uses the server's standard HTTP port to avoid a known
32-bit libmpv/OpenSSL crash while opening HTTPS media (seen with the earlier 32-bit worker). This fallback is limited
to Plex direct-host names.

## Build

The hidden playback worker is an x64 C++20 target (the extension DLL stays Win32 for the 32-bit client).
The pinned x86_64 libmpv development package must be supplied through `MPV_ROOT`; the build dynamically
loads `libmpv-2.dll` and does not require an MSVC import library.

```powershell
cmake -S host -B host/build -A x64 `
  -DMPV_ROOT='C:/path/to/mpv-dev-x86_64-20261014-git-ec38361c56'
cmake --build host/build --config Release
ctest --test-dir host/build -C Release --output-on-failure
```

`yt-dlp.exe` isn't built here. The release workflow downloads the pinned release (`YTDLP_VERSION` in
`.github/workflows/build-release.yml`), checks it against that release's `SHA2-256SUMS` and the SHA-256 pinned in
the workflow, and packages it beside the helper. `ytdl_tests` covers finding yt-dlp, its version's age and `-U`
against a stand-in (`tests/FakeYtdlp.cpp`), with no network.

The current verified x64 runtime is mpv `v0.41.0-1154-gec38361c5`, from shinchiro/mpv-winbuild-cmake's
`20261014` package. The earlier 32-bit worker used the `20260608` i686 package; newer i686 packages reproduced an
upstream 32-bit OpenSSL assertion, and the 32-bit build couldn't open HTTPS or YouTube streams, hence the move to
x64.
