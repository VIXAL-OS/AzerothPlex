# Third-party notices

Azeroth Plex is licensed GPL-3.0-or-later (see [LICENSE](LICENSE)). This package
also contains or uses the following third-party software.

## libmpv (`libmpv-2.dll`)

This build's background player (`wxl-video-host.exe`) is 64-bit and loads the
64-bit libmpv at run time (`LoadLibrary`; it is not linked against it).

- Binary: `libmpv-2.dll` from the `mpv-dev-x86_64-20261014-git-ec38361c56.7z`
  package of the [shinchiro/mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake)
  release `20261014`, unmodified.
  SHA-256 `6a48faecb5ffe14a7be2f1fcf8b193bf3d51837cfb36e8d3527d82089de563d9`.
- mpv version: `v0.41.0-1154-gec38361c5`.
- mpv source: https://github.com/mpv-player/mpv (commit `ec38361c5`).
- Build recipe and the exact versions of the libraries built into the DLL:
  https://github.com/shinchiro/mpv-winbuild-cmake (release `20261014`).
- Licence: mpv is licensed GPL-2.0-or-later. Parts of it are LGPL-2.1-or-later;
  see mpv's `Copyright` and `LICENSE.GPL` / `LICENSE.LGPL` files. The libraries
  statically linked into this DLL, including FFmpeg, libass, libplacebo and
  their dependencies, stay under their own licences, which are listed in
  their upstream source trees.

This DLL replaces the 32-bit libmpv (`mpv-dev-i686-20260608-git-6444c05.7z`)
that the upstream Azeroth Plex release uses. The 32-bit build could not open
HTTPS streams in this setup.

## Dear ImGui

The helper's own UI uses [Dear ImGui](https://github.com/ocornut/imgui)
`v1.92.9b`, with its Win32 and Direct3D 11 backends, compiled into
`wxl-video-host.exe`. MIT licence; the text is in the source tree at
`host/third_party/imgui/LICENSE.txt`.

## nlohmann/json

Plex OAuth responses are parsed with [nlohmann/json](https://github.com/nlohmann/json)
`v3.12.0`, compiled into `wxl-video-host.exe`. MIT licence; the single header
is in the source tree at `host/third_party/json.hpp`.

## yt-dlp (`yt-dlp.exe`)

CyTube media from YouTube, Vimeo, Twitch, Google Drive and similar sites plays
through mpv's ytdl hook, which runs [yt-dlp](https://github.com/yt-dlp/yt-dlp).
The release package includes yt-dlp's official Windows build beside the helper.
It is a separate program, not linked into Azeroth Plex. mpv runs it to open
those links, and the helper runs `yt-dlp --version` to show its version in the
panel.

- Binary: `yt-dlp.exe` from yt-dlp's release
  [`2026.10.21`](https://github.com/yt-dlp/yt-dlp/releases/tag/2026.10.21),
  unmodified: 17,880,111 bytes, SHA-256
  `33121d192d706e62f684a06f80918e3f3a8ba52ac3a65213b6951c39c7d9f555`. Before
  packaging, the release build checks it against this hash and against that
  release's `SHA2-256SUMS`.
- Source: https://github.com/yt-dlp/yt-dlp, tag `2026.10.21`.
- Licence: yt-dlp is released into the public domain under the
  [Unlicense](https://github.com/yt-dlp/yt-dlp/blob/2026.10.21/LICENSE). The
  exe is a PyInstaller build that also contains Python and other libraries
  under their own licences. Their texts are in yt-dlp's
  `THIRD_PARTY_LICENSES.txt`, which is included as
  `yt-dlp-THIRD_PARTY_LICENSES.txt`.
- Security: `2026.10.21` includes the fixes for the 2026 advisories:
  - CVE-2026-26331 (fixed in 2026.02.21);
  - CVE-2026-50019, CVE-2026-50023, CVE-2026-50574 and the `--exec` command
    injection advisory (fixed in 2026.06.09);
  - CVE-2026-55404 (fixed in 2026.07.04).
- Updates: the panel's **Update yt-dlp** button runs this exe's own
  `yt-dlp -U`, and only when it is pressed. That replaces the exe with yt-dlp's
  latest release. After an update, the copy in the folder is no longer the file
  described above.

Without `yt-dlp.exe` in the folder, the helper uses a yt-dlp it finds on `PATH`,
for example a pip install. Direct files play without yt-dlp.

## The Stonewall TV sign (`stonewall-sign.bin`)

A picture rendered by this project's own code (`extension/src/StonewallArt.hpp`) and kept in `assets/`, because
its lettering uses two fonts that come with Microsoft Office rather than Windows (Harlow Solid Italic and Mistral).
Only the rendered picture is included, never the fonts. If the file is missing, the DLL renders the sign itself with
whatever fonts the machine has.
