# Modifications

This is a modified version of [AzerothPlex](https://github.com/STRHercules/AzerothPlex) by STRHercules, at commit
`7b2d7bb`, licensed under GPL-3.0 (see [LICENSE](LICENSE)). The modifications are released under the same license.

- **Who:** VIXAL-OS, with AI coding assistants (Claude and Codex).
- **When:** 14–23 October 2026.
- **Target:** the 3.3.5a client (build 12340) with WarcraftXL, played on ChromieCraft.

## What changed
- **CyTube channels:** the host can follow a CyTube channel as a passive viewer, playing its current media in sync
  with the channel's clock.
  - `host/CytubeProtocol.*`: channel links, the channel's socket servers, engine.io v4 framing, and media types.
  - `host/CytubeClient.*`: the viewer's connection, on a worker thread.
  - Media plays through mpv: direct files as they are, and YouTube and similar sites through mpv's ytdl hook and
    yt-dlp.
  - Drift is caught up by speed (up to 5% faster or slower) rather than by repeated seeking. Only drifts over 2.5 s
    seek.
- **In-game CyTube panel:** join or leave a channel from the extension, with a password box, status and drift. The
  last channel is remembered in the UI settings. While joined, the raw Play, Pause and Stop controls are hidden and
  refused by the host.
- **Picture fills the frame:** mpv no longer letterboxes 4:3 video inside the 640x360 frame (`keepaspect=no`).
- **Stonewall TV:** the optional neon sign from the Video Cinema build, drawn over the world screen and its frame.
- **Ambient glow:** the screen's edges are sampled in zones ten times a second and eased over about 0.3 s. A soft
  additive ring in those colours is drawn around the frame. Neither the sign nor the glow depth-tests, so they show
  with "Characters and world appear in front" on.
- **Depth that matches the world's:** with "Characters and world appear in front" on, the screen is drawn before
  the world with the projection the world is rendered with (the graphics device's second matrix) and the world
  pass's depth range, instead of the projection the device was given and the viewport's range. At the end of the
  scene the screen doesn't swap the depth surface either.
- **State put back whole, and only the world pass touched:** every state block is captured right after it is created (`CreateStateBlock` alone gives the shaders' float constants above c127 back wrong on this machine's D3D9 runtime). The screen drawn before the world goes only into the world pass's own clear, into the depth surface the world is drawn with, and the frame is uploaded at the end of the scene, not inside the pass.
- **No depth bias through hooked calls:** the screen's depth offset goes into its projection instead of
  `D3DRS_DEPTHBIAS`, depth writes are switched on only with the depth test, and the depth surface is only set when it
  isn't already bound. wxl-modern-water hooks the device's `SetRenderState` and `SetDepthStencilSurface` and took the
  screen's bias for the client's own; a state block's restore doesn't pass through those hooks, so it wrote the bias
  back for the world pass after the shadow maps, and the terrain was drawn nearer than it is (Living Azeroth's snow
  cover vanished, shadows went wrong).
- **Depth pull in yards:** the panel's "Depth pull" moves the depth-tested screen that many yards toward the camera
  (default 0.05, at most 0.15), by scaling view space about the eye in its projection: the same pixels, only nearer
  depth (`extension/src/ScreenDepth.hpp`). The old "Depth offset" of -0.010 had only looked right because the leaked
  bias moved the characters with the screen; in the projection alone it brought a screen 18 yd away to 9 yd, over the
  character standing in front of it. The value is still saved, kept in presets and shared with a party in its old
  units (pull = -5 x the old value), so the old default -0.010 is 0.05 yd and older copies read the same numbers.
- **yt-dlp, found and kept current** (`host/YtdlTool.*`): the release ships `yt-dlp.exe` beside the helper. The
  helper looks for yt-dlp where mpv's ytdl hook would: that copy first, then `yt-dlp`, `yt-dlp_x86` or `youtube-dl`
  through the process search (`PATH`). It points the hook at what it found, also when it turns up after mpv started.
  - Without yt-dlp, a YouTube, Vimeo, Twitch or similar item is skipped and the panel says where to put
    `yt-dlp.exe`, instead of a bare "loading failed". Direct files keep playing, and the skipped item starts once
    yt-dlp is there.
  - The panel shows yt-dlp's version and age, and past 30 days says that YouTube breaks old versions.
  - **Update yt-dlp** runs `yt-dlp -U` on the copy beside the helper, in the background, only when pressed, and
    shows the result. For a pip install, the panel gives `python -m pip install -U yt-dlp` instead.
  - The UI bridge is version 7 (two commands and a yt-dlp snapshot), so the DLL and the helper go together.
- **Tests:** `host/tests/CytubeTests.cpp` (protocol), `host/tests/CytubeProbe.cpp` (a live channel probe), and
  `host/tests/YtdlTests.cpp` (finding yt-dlp, its version's age and `-U`, against the stand-in
  `host/tests/FakeYtdlp.cpp`; no network).
