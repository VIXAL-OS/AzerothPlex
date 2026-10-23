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

## yt-dlp (not included)

CyTube channels that play YouTube and similar sites go through mpv's ytdl hook.
That needs [yt-dlp](https://github.com/yt-dlp/yt-dlp) on `PATH` or a
`yt-dlp.exe` beside the helper. It is not part of this package.

## The Stonewall TV sign (`stonewall-sign.bin`)

A picture rendered by this project's own code (`extension/src/StonewallArt.hpp`) and kept in `assets/`, because
its lettering uses two fonts that come with Microsoft Office rather than Windows (Harlow Solid Italic and Mistral).
Only the rendered picture is included, never the fonts. If the file is missing, the DLL renders the sign itself with
whatever fonts the machine has.
