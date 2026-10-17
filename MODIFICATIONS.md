# Modifications

This is a modified version of [AzerothPlex](https://github.com/STRHercules/AzerothPlex) by STRHercules, at commit
`7b2d7bb`, licensed under GPL-3.0 (see [LICENSE](LICENSE)). The modifications are released under the same license.

- **Who:** VIXAL-OS, with AI coding assistants (Claude and Codex).
- **When:** 14–17 October 2026.
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
- **Tests:** `host/tests/CytubeTests.cpp` (protocol) and `host/tests/CytubeProbe.cpp` (a live channel probe).
