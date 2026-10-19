// Placeable world-space native Plex video surface for WarcraftXL build 12340.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoSurface.hpp"

#include "StonewallArt.hpp"
#include "VideoShared.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/World.hpp"

#include <d3d9.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>

namespace wxl_video_screen
{
    VideoSurface::SceneClearFn VideoSurface::originalSceneClear_ = nullptr;

    namespace
    {
        constexpr char kTag[] = "wxl-azeroth-plex";
        constexpr wchar_t kHostWindowClass[] = L"WXLNativePlexHost";
        constexpr float kPi = 3.14159265358979323846f;

        void ModuleAddressMarker() {}

        bool Finite3(const float value[3])
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) &&
                   std::isfinite(value[2]) && std::abs(value[0]) <= 65536.0f &&
                   std::abs(value[1]) <= 65536.0f && std::abs(value[2]) <= 65536.0f;
        }

        struct Vertex
        {
            float x, y, z;
            D3DCOLOR color;
            float u, v;
        };

        // The world pass's own projection and depth range (Living Azeroth verified both in the client, 2026-10-02; the
        // disassembly agrees):
        //  - the graphics device keeps the projection it was GIVEN at +0xF88 (what gfx::SceneMatrices returns) and the
        //    one it RENDERS with at +0xFC8: its set-projection routine 0x6A9B40 recomputes the depth terms (stored at
        //    0x6A9CED), and the shader system (0x872C10) hands the world that one. The same x and y, so the same place
        //    on screen; but a depth that isn't the world's, so the screen and the characters in front of it were
        //    compared by two different measures.
        //  - the world render (0x4F8EA0) sets viewport z = 0..[0xADEEE4] (a static 0.94) at 0x4F905A, before the scene
        //    render's own clear (0x79AB9A, where the pre-world screen is drawn), and puts 0..1 back before
        //    OnWorldSceneEnd.
        constexpr size_t kDeviceRenderProjection = 0xFC8;
        constexpr uintptr_t kWorldPassMaxZ = 0x00ADEEE4;

        bool RenderProjection(float projection[16])
        {
            const void* graphics = wxl::game::gx::RawGraphicsDevice();
            if (!graphics) return false;
            std::memcpy(projection, static_cast<const uint8_t*>(graphics) + kDeviceRenderProjection, 16 * sizeof(float));
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(projection[i])) return false;
            return projection[11] != 0.0f; // a perspective (w = view z), as the world's always is
        }

        float WorldPassMaxZ()
        {
            const float z = *reinterpret_cast<const volatile float*>(kWorldPassMaxZ);
            return std::isfinite(z) && z > 0.5f && z <= 1.0f ? z : 1.0f;
        }

        // What a state block can't put back for everyone else. wxl-modern-water patches the live device's SetRenderState
        // and SetDepthStencilSurface (its d3d9_wrap.cpp, "device adoption", for its shadow-map bias): it takes every
        // D3DRS_DEPTHBIAS and D3DRS_SLOPESCALEDEPTHBIAS that goes through SetRenderState for the client's own, raises the
        // bias while a shadow map is the depth target, and when another depth is bound writes "the client's" back. Apply()
        // restores states inside the runtime, not through the device's functions, so that hook never sees the restore.
        // The screen's -0.010 therefore came back for the world: with "Characters and world appear in front" the screen is
        // drawn at the world pass's clear, ahead of the shadow maps (CWorldScene::Render 0x79A870: the clear,
        // CShadowQuery::Update, then the terrain); the bias was written back when the world's depth was bound again after
        // them, and the client (which re-sends nothing it believes is still set) drew the terrain 0.01 nearer than it is --
        // Living Azeroth's snow cover, a few centimetres above the terrain with no bias of its own, lost the depth test
        // everywhere, and the shadow maps were drawn 0.01 toward the light (Video Cinema, 2026-10-19). At the end of the
        // scene the screen did the same for the next frame unless the sign's bias of 0 followed it.
        // So nothing here sets a depth bias: the screen's offset goes into its projection (OffsetDepth), and depth writes
        // are switched on only where the depth test is (with depth buffering off nothing is written).
        // Window depth z' = z + bias with depth range r becomes NDC z' = z + bias / r, i.e. clip z' = z + w * bias / r.
        void OffsetDepth(D3DMATRIX& projection, float bias, float range)
        {
            if (!(range > 0.0f) || bias == 0.0f) return;
            const float k = bias / range;
            projection._13 += k * projection._14;
            projection._23 += k * projection._24;
            projection._33 += k * projection._34;
            projection._43 += k * projection._44;
        }

        // A state block holding everything as it is now. CreateStateBlock(D3DSBT_ALL) alone records the vertex
        // shader's float constants c128..c255 and the pixel shader's c128..c223 wrong: Apply() then writes copies
        // of c0..c127 / c0..c95 there (this machine's D3D9 runtime, on the GeForce and the Radeon alike, 2026-10-17;
        // every other state comes back intact). Capture() right after records them properly -- Dear ImGui's DX9
        // backend does the same (its issue #3857). Drawn before the world, the screen left those registers wrong for
        // the world pass: whatever was set there before it and not set again (the client's model shaders read
        // c224..c235, for one) read copies of the low registers instead.
        HRESULT CaptureState(IDirect3DDevice9* device, IDirect3DStateBlock9** state)
        {
            *state = nullptr;
            HRESULT result = device->CreateStateBlock(D3DSBT_ALL, state);
            if (SUCCEEDED(result) && *state && FAILED(result = (*state)->Capture()))
            {
                (*state)->Release();
                *state = nullptr;
            }
            return result;
        }

        struct ScreenVertex
        {
            float x, y, z, rhw;
            D3DCOLOR color;
            float u, v;
        };

        bool CopyClipboardText(std::string_view text)
        {
            if (text.empty() || !OpenClipboard(nullptr)) return false;
            EmptyClipboard();
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
            if (!memory)
            {
                CloseClipboard();
                return false;
            }
            void* target = GlobalLock(memory);
            if (!target)
            {
                GlobalFree(memory);
                CloseClipboard();
                return false;
            }
            std::memcpy(target, text.data(), text.size());
            static_cast<char*>(target)[text.size()] = '\0';
            GlobalUnlock(memory);
            const bool ok = SetClipboardData(CF_TEXT, memory) != nullptr;
            if (!ok) GlobalFree(memory);
            CloseClipboard();
            return ok;
        }

        D3DCOLOR FrameColor(int style)
        {
            switch (style)
            {
            case 1: return 0xFF303030u; // Classic television.
            case 2: return 0xFF3C6A35u; // Goblin.
            case 3: return 0xFFB58B42u; // Gnomish brass.
            case 4: return 0xFF5A241Eu; // Orcish.
            case 5: return 0xFFD6A84Bu; // Blood elf.
            case 6: return 0xFF62BFD4u; // Titan.
            case 7: return 0xFF5B3A24u; // Tavern wood.
            case 8: return 0xFF181818u; // Outdoor cinema.
            default: return 0xFF101010u;
            }
        }

        IDirect3DTexture9* MakeTexture(IDirect3DDevice9* device, int w, int h, const std::vector<uint32_t>& pixels)
        {
            IDirect3DTexture9* texture = nullptr;
            if (FAILED(device->CreateTexture(w, h, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) &&
                FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
                return nullptr;
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) { texture->Release(); return nullptr; }
            for (int y = 0; y < h; ++y)
                std::memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, pixels.data() + size_t(y) * w, size_t(w) * 4);
            texture->UnlockRect(0);
            return texture;
        }

        D3DCOLOR FrameAccentColor(int style)
        {
            switch (style)
            {
            case 1: return 0xFF111111u;
            case 2: return 0xFFC58A25u;
            case 3: return 0xFFE3C56Du;
            case 4: return 0xFF2B100Du;
            case 5: return 0xFFFFE0A3u;
            case 6: return 0xFFA6F4FFu;
            case 7: return 0xFF2A160Cu;
            case 8: return 0xFF595959u;
            default: return 0xFF202020u;
            }
        }

        template <typename DrawRect>
        HRESULT DrawDecorativeFrame(int style, float halfWidth, float halfHeight,
                                    bool ambientLighting, D3DCOLOR ambientColor,
                                    DrawRect&& drawRect)
        {
            if (style <= 0 && !ambientLighting) return S_OK;

            HRESULT result = S_OK;
            const auto draw = [&](float left, float right, float bottom, float top, D3DCOLOR color)
            {
                const HRESULT current = drawRect(left, right, bottom, top, color);
                if (FAILED(current) && SUCCEEDED(result)) result = current;
            };
            const float tx = std::max(0.001f, halfWidth * 0.035f);
            const float ty = std::max(0.001f, halfHeight * 0.060f);
            const D3DCOLOR base = FrameColor(style);
            const D3DCOLOR accent = FrameAccentColor(style);

            const auto ring = [&](float xScale, float yScale, D3DCOLOR color)
            {
                const float x = tx * xScale;
                const float y = ty * yScale;
                draw(-halfWidth - x, halfWidth + x, halfHeight, halfHeight + y, color);
                draw(-halfWidth - x, halfWidth + x, -halfHeight - y, -halfHeight, color);
                draw(-halfWidth - x, -halfWidth, -halfHeight, halfHeight, color);
                draw(halfWidth, halfWidth + x, -halfHeight, halfHeight, color);
            };

            switch (style)
            {
            case 1: // Classic Television: heavy bezel, lower cabinet, and feet.
                ring(1.65f, 1.65f, base);
                draw(-halfWidth * 0.62f, halfWidth * 0.62f,
                     -halfHeight - ty * 2.45f, -halfHeight - ty * 1.65f, accent);
                draw(-halfWidth * 0.72f, -halfWidth * 0.45f,
                     -halfHeight - ty * 3.35f, -halfHeight - ty * 2.45f, base);
                draw(halfWidth * 0.45f, halfWidth * 0.72f,
                     -halfHeight - ty * 3.35f, -halfHeight - ty * 2.45f, base);
                break;
            case 2: // Goblin Television: chunky green shell with brass corner plates.
                ring(1.25f, 1.25f, base);
                draw(-halfWidth - tx * 2.0f, -halfWidth + tx * 0.25f,
                     halfHeight - ty * 0.65f, halfHeight + ty * 1.95f, accent);
                draw(halfWidth - tx * 0.25f, halfWidth + tx * 2.0f,
                     halfHeight - ty * 0.65f, halfHeight + ty * 1.95f, accent);
                draw(-halfWidth - tx * 2.0f, -halfWidth + tx * 0.25f,
                     -halfHeight - ty * 1.95f, -halfHeight + ty * 0.65f, accent);
                draw(halfWidth - tx * 0.25f, halfWidth + tx * 2.0f,
                     -halfHeight - ty * 1.95f, -halfHeight + ty * 0.65f, accent);
                break;
            case 3: // Gnomish Projector: slim brass bezel with precision rails.
                ring(0.80f, 0.80f, base);
                draw(-halfWidth * 0.78f, halfWidth * 0.78f,
                     halfHeight + ty * 1.05f, halfHeight + ty * 1.42f, accent);
                draw(-halfWidth * 0.78f, halfWidth * 0.78f,
                     -halfHeight - ty * 1.42f, -halfHeight - ty * 1.05f, accent);
                draw(-halfWidth - tx * 1.35f, -halfWidth - tx * 0.80f,
                     -halfHeight * 0.22f, halfHeight * 0.22f, accent);
                draw(halfWidth + tx * 0.80f, halfWidth + tx * 1.35f,
                     -halfHeight * 0.22f, halfHeight * 0.22f, accent);
                break;
            case 4: // Orcish Cinema: rugged red-brown surround with side lugs.
                ring(1.35f, 1.35f, base);
                draw(-halfWidth - tx * 2.35f, -halfWidth - tx * 1.35f,
                     halfHeight * 0.38f, halfHeight * 0.78f, accent);
                draw(-halfWidth - tx * 2.35f, -halfWidth - tx * 1.35f,
                     -halfHeight * 0.78f, -halfHeight * 0.38f, accent);
                draw(halfWidth + tx * 1.35f, halfWidth + tx * 2.35f,
                     halfHeight * 0.38f, halfHeight * 0.78f, accent);
                draw(halfWidth + tx * 1.35f, halfWidth + tx * 2.35f,
                     -halfHeight * 0.78f, -halfHeight * 0.38f, accent);
                draw(-halfWidth * 0.52f, halfWidth * 0.52f,
                     -halfHeight - ty * 2.05f, -halfHeight - ty * 1.35f, accent);
                break;
            case 5: // Blood Elf Arcane Screen: narrow gold frame and central arcane crests.
                ring(0.65f, 0.65f, base);
                draw(-halfWidth * 0.18f, halfWidth * 0.18f,
                     halfHeight + ty * 0.65f, halfHeight + ty * 2.05f, accent);
                draw(-halfWidth * 0.18f, halfWidth * 0.18f,
                     -halfHeight - ty * 2.05f, -halfHeight - ty * 0.65f, accent);
                draw(-halfWidth - tx * 1.45f, -halfWidth - tx * 0.65f,
                     -halfHeight * 0.16f, halfHeight * 0.16f, accent);
                draw(halfWidth + tx * 0.65f, halfWidth + tx * 1.45f,
                     -halfHeight * 0.16f, halfHeight * 0.16f, accent);
                break;
            case 6: // Titan Hologram: double luminous rail with corner nodes.
                ring(0.55f, 0.55f, base);
                ring(1.25f, 1.25f, accent);
                draw(-halfWidth - tx * 1.75f, -halfWidth - tx * 0.25f,
                     halfHeight + ty * 0.25f, halfHeight + ty * 1.75f, accent);
                draw(halfWidth + tx * 0.25f, halfWidth + tx * 1.75f,
                     halfHeight + ty * 0.25f, halfHeight + ty * 1.75f, accent);
                draw(-halfWidth - tx * 1.75f, -halfWidth - tx * 0.25f,
                     -halfHeight - ty * 1.75f, -halfHeight - ty * 0.25f, accent);
                draw(halfWidth + tx * 0.25f, halfWidth + tx * 1.75f,
                     -halfHeight - ty * 1.75f, -halfHeight - ty * 0.25f, accent);
                break;
            case 7: // Tavern Projection Screen: oversized timber rails and side posts.
                draw(-halfWidth - tx * 1.15f, halfWidth + tx * 1.15f,
                     halfHeight, halfHeight + ty * 2.10f, base);
                draw(-halfWidth - tx * 1.15f, halfWidth + tx * 1.15f,
                     -halfHeight - ty * 2.10f, -halfHeight, base);
                draw(-halfWidth - tx * 1.15f, -halfWidth,
                     -halfHeight - ty * 0.65f, halfHeight + ty * 0.65f, accent);
                draw(halfWidth, halfWidth + tx * 1.15f,
                     -halfHeight - ty * 0.65f, halfHeight + ty * 0.65f, accent);
                break;
            case 8: // Outdoor Cinema: simple dark surround with two support legs.
                ring(0.85f, 0.85f, base);
                draw(-halfWidth * 0.72f, -halfWidth * 0.60f,
                     -halfHeight - ty * 4.50f, -halfHeight - ty * 0.85f, accent);
                draw(halfWidth * 0.60f, halfWidth * 0.72f,
                     -halfHeight - ty * 4.50f, -halfHeight - ty * 0.85f, accent);
                draw(-halfWidth * 0.88f, -halfWidth * 0.44f,
                     -halfHeight - ty * 4.90f, -halfHeight - ty * 4.50f, base);
                draw(halfWidth * 0.44f, halfWidth * 0.88f,
                     -halfHeight - ty * 4.90f, -halfHeight - ty * 4.50f, base);
                break;
            default:
                break;
            }

            if (ambientLighting)
            {
                // Low-cost ambience: a thin sampled-color halo outside the cosmetic geometry.
                const float x = tx * (style == 0 ? 0.65f : 2.65f);
                const float y = ty * (style == 0 ? 0.65f : 2.65f);
                const float haloX = tx * 0.28f;
                const float haloY = ty * 0.28f;
                draw(-halfWidth - x - haloX, halfWidth + x + haloX,
                     halfHeight + y, halfHeight + y + haloY, ambientColor);
                draw(-halfWidth - x - haloX, halfWidth + x + haloX,
                     -halfHeight - y - haloY, -halfHeight - y, ambientColor);
                draw(-halfWidth - x - haloX, -halfWidth - x,
                     -halfHeight - y, halfHeight + y, ambientColor);
                draw(halfWidth + x, halfWidth + x + haloX,
                     -halfHeight - y, halfHeight + y, ambientColor);
            }
            return result;
        }

        void FormatTime(char* output, size_t size, uint32_t milliseconds)
        {
            const uint32_t totalSeconds = milliseconds / 1000;
            const uint32_t seconds = totalSeconds % 60;
            const uint32_t minutes = (totalSeconds / 60) % 60;
            const uint32_t hours = totalSeconds / 3600;
            if (hours > 0)
                std::snprintf(output, size, "%u:%02u:%02u", hours, minutes, seconds);
            else
                std::snprintf(output, size, "%u:%02u", minutes, seconds);
        }
    }

    VideoSurface& VideoSurface::Instance()
    {
        static VideoSurface instance;
        return instance;
    }

    VideoSurface::VideoSurface()
    {
        on<&VideoSurface::OnFrame>(wxl::events::Event::OnFrame);
        on<&VideoSurface::OnWorldRender>(wxl::events::Event::OnWorldRender);
        on<&VideoSurface::OnWorldSceneEnd>(wxl::events::Event::OnWorldSceneEnd);
        on<&VideoSurface::OnInput>(wxl::events::Event::OnInput);
        on<&VideoSurface::OnUpdate>(wxl::events::Event::OnUpdate);
        on<&VideoSurface::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
        on<&VideoSurface::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
        on<&VideoSurface::OnWorldLeave>(wxl::events::Event::OnWorldLeave);
    }

    bool VideoSurface::Initialize(const WXL_Api* api)
    {
        if (!api || initialized_) return initialized_;
        api_ = api;
        frameScratch_.resize(wxl_video_shared::kFrameBytes);
        ConnectControl();
        ConnectUi();
        LoadPlacement();
        LoadUiPreferences();
        LoadCinemaPresets();
        if (!api_->HookAttach || !api_->HookAttach(
                "wxl-azeroth-plex.scene-clear",
                wxl::game::gx::kSceneClearSeam,
                reinterpret_cast<void*>(&VideoSurface::SceneClearHook),
                reinterpret_cast<void**>(&originalSceneClear_),
                WXL_HOOK_DEFAULT_PRIORITY))
        {
            status_ = "Could not attach the pre-world screen renderer.";
            api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }
        initialized_ = true;
        return true;
    }

    void __cdecl VideoSurface::SceneClearHook(uint32_t flags, uint32_t colour)
    {
        if (originalSceneClear_) originalSceneClear_(flags, colour);

        auto& self = Instance();
        // Only the world pass's own clear (some twenty callers clear colour and depth, shadow maps and portraits among
        // them): after OnWorldRender, into the depth surface the world is drawn with. Without that event (an older core)
        // the first such clear of the frame, as before.
        if (self.preWorldDrawn_ || self.pinned_ || !self.initialized_ || !self.depthTest_ || !self.visible_ ||
            !self.placed_ || (flags & (wxl::game::gx::clear::kColor |
                                      wxl::game::gx::clear::kDepth)) !=
                             (wxl::game::gx::clear::kColor |
                              wxl::game::gx::clear::kDepth) ||
            (self.worldRenderEvents_ && !self.gateOff_ && !self.inWorldPass_) ||
            wxl::game::world::CurrentMapId() != self.mapId_)
            return;

        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device) return;

        IDirect3DSurface9* depth = nullptr;
        device->GetDepthStencilSurface(&depth);
        if (!self.worldRenderEvents_ || self.gateOff_ || (depth && depth == self.worldPassDepth_))
        {
            self.DrawWorldScreen(device, depth, true);
            self.preWorldDrawn_ = true;
        }
        if (depth) depth->Release();
    }

    void VideoSurface::OnFrame(const wxl::events::FrameArgs&)
    {
        preWorldDrawn_ = false;
    }

    void VideoSurface::OnWorldRender(const wxl::events::WorldRenderArgs& args)
    {
        ++worldRenderEvents_;
        inWorldPass_ = true;
        worldPassDepth_ = nullptr;
        if (auto* device = static_cast<IDirect3DDevice9*>(args.device))
        {
            IDirect3DSurface9* depth = nullptr;
            if (SUCCEEDED(device->GetDepthStencilSurface(&depth)) && depth)
            {
                worldPassDepth_ = depth; // compared by address only, never used
                depth->Release();
            }
        }
    }

    void VideoSurface::OnInput(const wxl::events::InputArgs& args)
    {
        if (!pinned_ || !visible_ || (api_ && api_->UiIsOpen && api_->UiIsOpen())) return;

        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device) return;
        const LPARAM lparam = static_cast<LPARAM>(args.lparam);
        const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));

        if (args.message == WM_LBUTTONDOWN)
        {
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (!GetPinnedRect(device, left, top, right, bottom) ||
                x < left || x > right || y < top || y > bottom)
                return;

            pinnedDragging_ = true;
            pinnedResizing_ = x >= right - 24.0f && y >= bottom - 24.0f;
            if (!pinnedResizing_)
            {
                pinnedDragOffsetX_ = x - left;
                pinnedDragOffsetY_ = y - top;
            }
            D3DDEVICE_CREATION_PARAMETERS parameters{};
            if (SUCCEEDED(device->GetCreationParameters(&parameters)) && parameters.hFocusWindow)
                SetCapture(parameters.hFocusWindow);
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_MOUSEMOVE)
        {
            D3DVIEWPORT9 viewport{};
            if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
                return;

            const float viewportWidth = static_cast<float>(viewport.Width);
            const float viewportHeight = static_cast<float>(viewport.Height);
            if (pinnedResizing_)
            {
                pinnedWidth_ = (x - static_cast<float>(viewport.X)) / viewportWidth - pinnedLeft_;
            }
            else
            {
                pinnedLeft_ = (x - pinnedDragOffsetX_ - static_cast<float>(viewport.X)) /
                              viewportWidth;
                pinnedTop_ = (y - pinnedDragOffsetY_ - static_cast<float>(viewport.Y)) /
                             viewportHeight;
            }
            wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_LBUTTONUP)
        {
            pinnedDragging_ = false;
            pinnedResizing_ = false;
            ReleaseCapture();
            SavePlacement();
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_CANCELMODE)
        {
            pinnedDragging_ = false;
            pinnedResizing_ = false;
            ReleaseCapture();
            SavePlacement();
            *args.handled = true;
        }
    }

    std::wstring VideoSurface::ExtensionDirectory() const
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleAddressMarker), &module))
            return {};

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH) return {};
        return std::filesystem::path(path).parent_path().wstring();
    }

    bool VideoSurface::OpenHost()
    {
        if (FindWindowW(kHostWindowClass, nullptr)) return true;

        const std::wstring directory = ExtensionDirectory();
        const std::wstring hostPath = directory.empty()
            ? std::wstring{} : (std::filesystem::path(directory) / L"wxl-video-host.exe").wstring();
        if (hostPath.empty() || GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            status_ = "Plex playback worker is missing from the extension folder.";
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        if (ConnectUi())
        {
            auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
            std::memset(bridge, 0, sizeof(*bridge));
            bridge->magic = wxl_video_shared::kUiMagic;
            bridge->version = wxl_video_shared::kUiVersion;
            bridge->structBytes = sizeof(*bridge);
            bridge->slots[0].structBytes = sizeof(wxl_video_shared::UiSnapshot);
            bridge->slots[1].structBytes = sizeof(wxl_video_shared::UiSnapshot);
        }

        std::wstring commandLine = L"\"" + hostPath + L"\" --parent-pid " +
                                   std::to_wstring(GetCurrentProcessId()) + L" --background";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(hostPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP, nullptr, directory.c_str(), &startup, &process))
        {
            char error[128]{};
            std::snprintf(error, sizeof(error), "Could not start helper (Win32 error %lu).",
                          static_cast<unsigned long>(GetLastError()));
            status_ = error;
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        status_ = "Background Plex player started.";
        return true;
    }

    void VideoSurface::HideHost()
    {
        plexPanelOpen_ = false;
        status_ = "Plex player is running in the background.";
    }

    void VideoSurface::CloseHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
            PostMessageW(host, WM_CLOSE, 0, 0);
        plexPanelOpen_ = false;
        status_ = "Plex player closed.";
    }

    bool VideoSurface::ConnectControl()
    {
        if (controlView_)
        {
            const auto* control = static_cast<const wxl_video_shared::ControlBlock*>(controlView_);
            return control->magic == wxl_video_shared::kControlMagic &&
                   control->version == wxl_video_shared::kControlVersion &&
                   control->structBytes == sizeof(*control);
        }

        controlMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::ControlBlock)),
            wxl_video_shared::kControlMappingName);
        if (!controlMapping_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        controlView_ = MapViewOfFile(controlMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                     sizeof(wxl_video_shared::ControlBlock));
        if (!controlView_)
        {
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }

        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        if (created)
        {
            std::memset(control, 0, sizeof(*control));
            control->magic = wxl_video_shared::kControlMagic;
            control->version = wxl_video_shared::kControlVersion;
            control->structBytes = sizeof(*control);
            control->volumePercent = masterVolume_;
        }
        if (control->magic != wxl_video_shared::kControlMagic ||
            control->version != wxl_video_shared::kControlVersion ||
            control->structBytes != sizeof(*control))
        {
            UnmapViewOfFile(controlView_);
            controlView_ = nullptr;
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }
        return true;
    }

    bool VideoSurface::SendCommand(LONG command)
    {
        if (!ConnectControl() || !OpenHost()) return false;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->command, command);
        MemoryBarrier();
        LONG sequence = control->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->commandSequence, sequence);
        status_ = "Command sent to the background Plex player.";
        return true;
    }

    bool VideoSurface::ConnectUi()
    {
        if (uiView_)
        {
            const auto* bridge = static_cast<const wxl_video_shared::UiBridge*>(uiView_);
            return bridge->magic == wxl_video_shared::kUiMagic &&
                   bridge->version == wxl_video_shared::kUiVersion &&
                   bridge->structBytes == sizeof(*bridge);
        }

        uiMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::UiBridge)),
            wxl_video_shared::kUiMappingName);
        if (!uiMapping_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        uiView_ = MapViewOfFile(uiMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                sizeof(wxl_video_shared::UiBridge));
        if (!uiView_)
        {
            CloseHandle(uiMapping_);
            uiMapping_ = nullptr;
            return false;
        }

        auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
        if (created)
        {
            std::memset(bridge, 0, sizeof(*bridge));
            bridge->magic = wxl_video_shared::kUiMagic;
            bridge->version = wxl_video_shared::kUiVersion;
            bridge->structBytes = sizeof(*bridge);
            bridge->slots[0].structBytes = sizeof(wxl_video_shared::UiSnapshot);
            bridge->slots[1].structBytes = sizeof(wxl_video_shared::UiSnapshot);
        }
        if (bridge->magic != wxl_video_shared::kUiMagic ||
            bridge->version != wxl_video_shared::kUiVersion ||
            bridge->structBytes != sizeof(*bridge))
        {
            UnmapViewOfFile(uiView_);
            uiView_ = nullptr;
            CloseHandle(uiMapping_);
            uiMapping_ = nullptr;
            return false;
        }
        return true;
    }

    bool VideoSurface::SendUiCommand(wxl_video_shared::UiCommand command, LONG index,
                                     LONG value, const char* text)
    {
        if (!ConnectUi() || !OpenHost()) return false;
        auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
        std::memset(&bridge->command, 0, sizeof(bridge->command));
        bridge->command.command = static_cast<LONG>(command);
        bridge->command.index = index;
        bridge->command.value = value;
        if (text)
        {
            const size_t length = std::min(std::strlen(text),
                                           wxl_video_shared::kUiTextBytes - 1);
            std::memcpy(bridge->command.text, text, length);
            bridge->command.text[length] = '\0';
        }
        MemoryBarrier();
        LONG sequence = bridge->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&bridge->commandSequence, sequence);
        return true;
    }

    bool VideoSurface::RefreshUiState()
    {
        if (!ConnectUi()) return false;
        wxl_video_shared::UiSnapshot snapshot{};
        if (!wxl_video_shared::ReadUiSnapshot(
                static_cast<const wxl_video_shared::UiBridge*>(uiView_), snapshot))
            return false;
        uiState_ = snapshot;
        ApplySharedCinema();
        return true;
    }

    void VideoSurface::SendVolume(int volume)
    {
        volume = std::clamp(volume, 0, 100);
        effectiveVolume_ = volume;
        if (!ConnectControl() || volume == lastSentVolume_) return;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->volumePercent, volume);
        MemoryBarrier();
        LONG sequence = control->volumeSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->volumeSequence, sequence);
        lastSentVolume_ = volume;
    }


    bool VideoSurface::PlaceInFront()
    {
        namespace camera = wxl::game::camera;
        namespace world = wxl::game::world;

        const int map = world::CurrentMapId();
        const unsigned long long guid = world::ActivePlayerGuid();
        void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (map < 0 || !player)
        {
            status_ = "Enter the world before placing the screen.";
            return false;
        }

        float playerPosition[3]{};
        float cameraPosition[3]{};
        world::UnitPosition(player, playerPosition);
        camera::GetPosition(cameraPosition);
        if (!Finite3(playerPosition) || !Finite3(cameraPosition))
        {
            status_ = "Player or camera position is unavailable.";
            return false;
        }

        float forwardX = playerPosition[0] - cameraPosition[0];
        float forwardY = playerPosition[1] - cameraPosition[1];
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!(length > 0.001f) || !std::isfinite(length))
        {
            forwardX = 1.0f;
            forwardY = 0.0f;
            length = 1.0f;
        }
        forwardX /= length;
        forwardY /= length;

        right_[0] = -forwardY;
        right_[1] = forwardX;
        right_[2] = 0.0f;
        const float height = width_ / ScreenAspectRatio();
        screenRotationDegrees_ = 0.0f;
        center_[0] = playerPosition[0] + forwardX * placeDistance_;
        center_[1] = playerPosition[1] + forwardY * placeDistance_;
        center_[2] = playerPosition[2] + bottomOffset_ + height * 0.5f;
        mapId_ = map;
        placed_ = true;
        visible_ = 1;
        SavePlacement();
        status_ = "World screen placed in front of your character.";
        if (api_)
            api_->Log(WXL_LOG_INFO, kTag, "placed world screen map=%d xyz=%.3f %.3f %.3f width=%.2f",
                      mapId_, center_[0], center_[1], center_[2], width_);
        return true;
    }

    void VideoSurface::Rotate(float degrees)
    {
        if (!placed_) return;
        const float radians = degrees * kPi / 180.0f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const float x = right_[0] * cosine - right_[1] * sine;
        const float y = right_[0] * sine + right_[1] * cosine;
        right_[0] = x;
        right_[1] = y;
        screenRotationDegrees_ += degrees;
        while (screenRotationDegrees_ > 180.0f) screenRotationDegrees_ -= 360.0f;
        while (screenRotationDegrees_ < -180.0f) screenRotationDegrees_ += 360.0f;
        SavePlacement();
    }

    void VideoSurface::SavePlacement()
    {
        if (!placed_) return;
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;

        const std::filesystem::path target = std::filesystem::path(directory) / L"world-screen.tsv";
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output.imbue(std::locale::classic());
        output << "wxl-video-screen-v1\t" << mapId_ << '\t'
               << std::setprecision(9) << center_[0] << '\t' << center_[1] << '\t' << center_[2] << '\t'
               << right_[0] << '\t' << right_[1] << '\t' << right_[2] << '\t'
               << width_ << '\t' << visible_ << '\t' << depthTest_ << '\t' << depthBias_
               << '\t' << pinned_ << '\t' << pinnedLeft_ << '\t' << pinnedTop_
               << '\t' << pinnedWidth_ << '\t' << aspectRatioMode_ << '\t' << fitMode_
               << '\t' << frameStyle_ << '\t' << ambientLighting_ << '\t' << spatialAudio_
               << '\t' << silentDistance_ << '\t' << stonewall_ << '\t' << signSize_ << '\n';
        output.close();
        if (!output) return;
        MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void VideoSurface::LoadPlacement()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        std::ifstream input(std::filesystem::path(directory) / L"world-screen.tsv");
        if (!input) return;
        input.imbue(std::locale::classic());

        std::string version;
        int map = -1;
        int visible = 1;
        int depth = 1;
        float center[3]{};
        float right[3]{};
        float width = 0.0f;
        float depthBias = -0.010f;
        int pinned = 0;
        float pinnedLeft = 0.64f;
        float pinnedTop = 0.04f;
        float pinnedWidth = 0.32f;
        int aspectRatioMode = 0;
        int fitMode = 0;
        int frameStyle = 0;
        int ambientLighting = 0;
        int spatialAudio = 1;
        float silentDistance = 60.0f;
        int stonewall = 0;
        if (!std::getline(input, version, '\t') || version != "wxl-video-screen-v1" ||
            !(input >> map) || input.get() != '\t' ||
            !(input >> center[0]) || input.get() != '\t' ||
            !(input >> center[1]) || input.get() != '\t' ||
            !(input >> center[2]) || input.get() != '\t' ||
            !(input >> right[0]) || input.get() != '\t' ||
            !(input >> right[1]) || input.get() != '\t' ||
            !(input >> right[2]) || input.get() != '\t' ||
            !(input >> width) || input.get() != '\t' ||
            !(input >> visible) || input.get() != '\t' || !(input >> depth))
            return;
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> depthBias)) depthBias = -0.010f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinned)) pinned = 0;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedLeft)) pinnedLeft = 0.64f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedTop)) pinnedTop = 0.04f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedWidth)) pinnedWidth = 0.32f;
        }
        if (input.peek() == '\t') { input.get(); if (!(input >> aspectRatioMode)) aspectRatioMode = 0; }
        if (input.peek() == '\t') { input.get(); if (!(input >> fitMode)) fitMode = 0; }
        if (input.peek() == '\t') { input.get(); if (!(input >> frameStyle)) frameStyle = 0; }
        if (input.peek() == '\t') { input.get(); if (!(input >> ambientLighting)) ambientLighting = 0; }
        if (input.peek() == '\t') { input.get(); if (!(input >> spatialAudio)) spatialAudio = 1; }
        if (input.peek() == '\t') { input.get(); if (!(input >> silentDistance)) silentDistance = 60.0f; }
        if (input.peek() == '\t') { input.get(); if (!(input >> stonewall)) stonewall = 0; }
        float signSize = 0.95f;
        if (input.peek() == '\t') { input.get(); if (!(input >> signSize)) signSize = 0.95f; }

        const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1]);
        if (map < 0 || !Finite3(center) || !(rightLength > 0.9f && rightLength < 1.1f) ||
            !std::isfinite(width) || width < 2.0f || width > 30.0f)
            return;

        mapId_ = map;
        std::memcpy(center_, center, sizeof(center_));
        right_[0] = right[0] / rightLength;
        right_[1] = right[1] / rightLength;
        right_[2] = 0.0f;
        width_ = width;
        screenRotationDegrees_ = 0.0f;
        visible_ = visible ? 1 : 0;
        depthTest_ = depth ? 1 : 0;
        depthBias_ = std::isfinite(depthBias) ? std::clamp(depthBias, -0.030f, 0.005f) : -0.010f;
        pinned_ = pinned ? 1 : 0;
        pinnedWidth_ = std::isfinite(pinnedWidth) ? pinnedWidth : 0.32f;
        pinnedLeft_ = std::isfinite(pinnedLeft) ? pinnedLeft : 0.64f;
        pinnedTop_ = std::isfinite(pinnedTop) ? pinnedTop : 0.04f;
        wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
        aspectRatioMode_ = std::clamp(aspectRatioMode, 0, 4);
        fitMode_ = std::clamp(fitMode, 0, 3);
        frameStyle_ = std::clamp(frameStyle, 0, 8);
        ambientLighting_ = ambientLighting ? 1 : 0;
        stonewall_ = stonewall ? 1 : 0;
        signSize_ = std::isfinite(signSize) ? std::clamp(signSize, 0.4f, 1.6f) : 0.95f;
        spatialAudio_ = spatialAudio ? 1 : 0;
        silentDistance_ = std::isfinite(silentDistance) ? std::clamp(silentDistance, 10.0f, 200.0f) : 60.0f;
        placed_ = true;
        status_ = "Restored the saved world-screen placement.";
    }

    void VideoSurface::SaveUiPreferences()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        const auto target = std::filesystem::path(directory) / L"ui-settings.tsv";
        const auto temporary = target.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        std::string channel(cytubeChannel_);
        for (char& c : channel)
            if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        output << "azerothplex-ui-v1\t" << (miniPlayerEnabled_ ? 1 : 0) << '\t' << channel << '\n';
        output.close();
        if (output)
            MoveFileExW(temporary.c_str(), target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void VideoSurface::LoadUiPreferences()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        std::ifstream input(std::filesystem::path(directory) / L"ui-settings.tsv");
        if (!input) return;
        std::string version;
        int miniPlayerEnabled = 1;
        if (!std::getline(input, version, '\t') || version != "azerothplex-ui-v1" ||
            !(input >> miniPlayerEnabled))
            return;
        miniPlayerEnabled_ = miniPlayerEnabled ? 1 : 0;
        std::string channel;
        if (input.get() == '\t' && std::getline(input, channel))
        {
            if (!channel.empty() && channel.back() == '\r') channel.pop_back();
            strncpy_s(cytubeChannel_, channel.c_str(), _TRUNCATE);
        }
    }

    void VideoSurface::LoadCinemaPresets()
    {
        cinemaPresets_.clear();
        auto builtIn = [this](const char* name, float width, int pinned, float pinnedWidth,
                              float audioRange, int frameStyle) {
            CinemaPreset preset;
            preset.name = name;
            preset.width = width;
            preset.pinned = pinned;
            preset.pinnedWidth = pinnedWidth;
            preset.silentDistance = audioRange;
            preset.frameStyle = frameStyle;
            preset.builtIn = true;
            cinemaPresets_.push_back(std::move(preset));
        };
        builtIn("Cozy", 6.0f, 0, 0.32f, 35.0f, 7);
        builtIn("Movie Theater", 14.0f, 0, 0.32f, 90.0f, 8);
        builtIn("Raid Night", 10.0f, 0, 0.32f, 70.0f, 2);
        builtIn("Outdoor Screen", 22.0f, 0, 0.32f, 120.0f, 0);
        builtIn("Picture in Picture", 8.0f, 1, 0.28f, 60.0f, 0);
        builtIn("Desktop", 8.0f, 1, 0.45f, 60.0f, 1);
        builtIn("Tavern TV", 5.0f, 0, 0.32f, 30.0f, 1);

        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        std::ifstream input(std::filesystem::path(directory) / L"cinema-presets.tsv");
        if (!input) return;
        input.imbue(std::locale::classic());
        std::string version;
        if (!std::getline(input, version) || version != "azerothplex-cinema-v1") return;
        CinemaPreset preset;
        while (input >> std::quoted(preset.name) >> preset.width >> preset.bottomOffset >>
               preset.rotationDegrees >> preset.pinned >> preset.pinnedLeft >> preset.pinnedTop >>
               preset.pinnedWidth >> preset.depthTest >> preset.depthBias >> preset.spatialAudio >>
               preset.silentDistance >> preset.aspectRatioMode >> preset.fitMode >> preset.frameStyle >>
               preset.ambientLighting)
        {
            preset.builtIn = false;
            preset.name = preset.name.substr(0, 63);
            preset.width = std::clamp(preset.width, 2.0f, 30.0f);
            preset.pinnedWidth = std::clamp(preset.pinnedWidth, 0.15f, 0.80f);
            preset.aspectRatioMode = std::clamp(preset.aspectRatioMode, 0, 4);
            preset.fitMode = std::clamp(preset.fitMode, 0, 3);
            preset.frameStyle = std::clamp(preset.frameStyle, 0, 8);
            cinemaPresets_.push_back(preset);
        }
    }

    void VideoSurface::SaveCinemaPresets()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        const auto target = std::filesystem::path(directory) / L"cinema-presets.tsv";
        const auto temporary = target.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output.imbue(std::locale::classic());
        output << "azerothplex-cinema-v1\n";
        for (const auto& preset : cinemaPresets_)
        {
            if (preset.builtIn) continue;
            output << std::quoted(preset.name) << ' ' << std::setprecision(9)
                   << preset.width << ' ' << preset.bottomOffset << ' ' << preset.rotationDegrees << ' '
                   << preset.pinned << ' ' << preset.pinnedLeft << ' ' << preset.pinnedTop << ' '
                   << preset.pinnedWidth << ' ' << preset.depthTest << ' ' << preset.depthBias << ' '
                   << preset.spatialAudio << ' ' << preset.silentDistance << ' '
                   << preset.aspectRatioMode << ' ' << preset.fitMode << ' ' << preset.frameStyle << ' '
                   << preset.ambientLighting << '\n';
        }
        output.close();
        if (output)
            MoveFileExW(temporary.c_str(), target.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void VideoSurface::ApplyCinemaPreset(size_t index)
    {
        if (index >= cinemaPresets_.size()) return;
        const auto preset = cinemaPresets_[index];
        selectedCinemaPreset_ = static_cast<int>(index);
        width_ = preset.width;
        bottomOffset_ = preset.bottomOffset;
        if (placed_)
            Rotate(preset.rotationDegrees - screenRotationDegrees_);
        screenRotationDegrees_ = preset.rotationDegrees;
        pinned_ = preset.pinned;
        pinnedLeft_ = preset.pinnedLeft;
        pinnedTop_ = preset.pinnedTop;
        pinnedWidth_ = preset.pinnedWidth;
        wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
        depthTest_ = preset.depthTest;
        depthBias_ = preset.depthBias;
        spatialAudio_ = preset.spatialAudio;
        silentDistance_ = preset.silentDistance;
        aspectRatioMode_ = preset.aspectRatioMode;
        fitMode_ = preset.fitMode;
        frameStyle_ = preset.frameStyle;
        ambientLighting_ = preset.ambientLighting;
        std::snprintf(cinemaPresetName_, sizeof(cinemaPresetName_), "%s", preset.name.c_str());
        if (placed_) SavePlacement();
        status_ = "Applied cinema preset: " + preset.name;
    }

    void VideoSurface::SaveCurrentCinemaPreset()
    {
        CinemaPreset preset;
        preset.name = cinemaPresetName_[0] ? cinemaPresetName_ : "Custom Cinema";
        preset.width = width_;
        preset.bottomOffset = bottomOffset_;
        preset.rotationDegrees = screenRotationDegrees_;
        preset.pinned = pinned_;
        preset.pinnedLeft = pinnedLeft_;
        preset.pinnedTop = pinnedTop_;
        preset.pinnedWidth = pinnedWidth_;
        preset.depthTest = depthTest_;
        preset.depthBias = depthBias_;
        preset.spatialAudio = spatialAudio_;
        preset.silentDistance = silentDistance_;
        preset.aspectRatioMode = aspectRatioMode_;
        preset.fitMode = fitMode_;
        preset.frameStyle = frameStyle_;
        preset.ambientLighting = ambientLighting_;
        preset.builtIn = false;
        cinemaPresets_.push_back(std::move(preset));
        selectedCinemaPreset_ = static_cast<int>(cinemaPresets_.size() - 1);
        SaveCinemaPresets();
        status_ = "Saved the current cinema preset.";
    }

    void VideoSurface::ShareCurrentCinema()
    {
        char payload[wxl_video_shared::kUiTextBytes]{};
        std::snprintf(payload, sizeof(payload),
                      "%s|%.3f|%.3f|%.3f|%d|%.4f|%.4f|%.4f|%d|%.5f|%d|%.3f|%d|%d|%d|%d",
                      cinemaPresetName_[0] ? cinemaPresetName_ : "Shared Cinema",
                      width_, bottomOffset_, screenRotationDegrees_, pinned_, pinnedLeft_, pinnedTop_,
                      pinnedWidth_, depthTest_, depthBias_, spatialAudio_, silentDistance_,
                      aspectRatioMode_, fitMode_, frameStyle_, ambientLighting_);
        SendUiCommand(wxl_video_shared::UiCommand::ShareCinemaSettings, -1, 0, payload);
    }

    void VideoSurface::ApplySharedCinema()
    {
        if (!uiState_.partyRoomCode[0] || !uiState_.sharedCinema.presetName[0])
        {
            lastSharedCinemaSignature_.clear();
            return;
        }
        char signature[384]{};
        std::snprintf(signature, sizeof(signature),
                      "%s|%.3f|%.3f|%.3f|%ld|%.4f|%.4f|%.4f|%ld|%.5f|%ld|%.3f|%ld|%ld|%ld|%ld",
                      uiState_.sharedCinema.presetName, uiState_.sharedCinema.width,
                      uiState_.sharedCinema.heightOffset, uiState_.sharedCinema.rotationDegrees,
                      static_cast<long>(uiState_.sharedCinema.pinned), uiState_.sharedCinema.pinnedX,
                      uiState_.sharedCinema.pinnedY, uiState_.sharedCinema.pinnedWidth,
                      static_cast<long>(uiState_.sharedCinema.depthOcclusion),
                      uiState_.sharedCinema.depthOffset,
                      static_cast<long>(uiState_.sharedCinema.distanceAudio), uiState_.sharedCinema.audioRange,
                      static_cast<long>(uiState_.sharedCinema.aspectRatioMode),
                      static_cast<long>(uiState_.sharedCinema.fitMode),
                      static_cast<long>(uiState_.sharedCinema.frameStyle),
                      static_cast<long>(uiState_.sharedCinema.ambientLighting));
        if (signature == lastSharedCinemaSignature_) return;
        lastSharedCinemaSignature_ = signature;

        width_ = std::clamp(uiState_.sharedCinema.width, 2.0f, 30.0f);
        bottomOffset_ = uiState_.sharedCinema.heightOffset;
        if (placed_)
            Rotate(uiState_.sharedCinema.rotationDegrees - screenRotationDegrees_);
        screenRotationDegrees_ = uiState_.sharedCinema.rotationDegrees;
        pinned_ = uiState_.sharedCinema.pinned ? 1 : 0;
        pinnedLeft_ = uiState_.sharedCinema.pinnedX;
        pinnedTop_ = uiState_.sharedCinema.pinnedY;
        pinnedWidth_ = uiState_.sharedCinema.pinnedWidth;
        wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
        depthTest_ = uiState_.sharedCinema.depthOcclusion ? 1 : 0;
        depthBias_ = uiState_.sharedCinema.depthOffset;
        spatialAudio_ = uiState_.sharedCinema.distanceAudio ? 1 : 0;
        silentDistance_ = uiState_.sharedCinema.audioRange;
        aspectRatioMode_ = std::clamp(static_cast<int>(uiState_.sharedCinema.aspectRatioMode), 0, 4);
        fitMode_ = std::clamp(static_cast<int>(uiState_.sharedCinema.fitMode), 0, 3);
        frameStyle_ = std::clamp(static_cast<int>(uiState_.sharedCinema.frameStyle), 0, 8);
        ambientLighting_ = uiState_.sharedCinema.ambientLighting ? 1 : 0;
        std::snprintf(cinemaPresetName_, sizeof(cinemaPresetName_), "%s", uiState_.sharedCinema.presetName);
        if (placed_) SavePlacement();
        status_ = "Applied the host's shared cinema layout.";
    }

    float VideoSurface::ScreenAspectRatio() const
    {
        switch (aspectRatioMode_)
        {
        case 1: return 16.0f / 9.0f;
        case 2: return 16.0f / 10.0f;
        case 3: return 4.0f / 3.0f;
        case 4: return 21.0f / 9.0f;
        default:
            return std::isfinite(uiState_.videoAspectRatio) && uiState_.videoAspectRatio > 0.2f
                ? std::clamp(uiState_.videoAspectRatio, 0.5f, 4.0f) : 16.0f / 9.0f;
        }
    }

    void VideoSurface::VideoLayout(float targetHalfWidth, float targetHalfHeight,
                                    float& videoHalfWidth, float& videoHalfHeight,
                                    float& u0, float& v0, float& u1, float& v1) const
    {
        videoHalfWidth = targetHalfWidth;
        videoHalfHeight = targetHalfHeight;
        u0 = 0.0f; v0 = 0.0f; u1 = 1.0f; v1 = 1.0f;
        const float targetAspect = targetHalfWidth / std::max(0.001f, targetHalfHeight);
        const float sourceAspect = std::isfinite(uiState_.videoAspectRatio) && uiState_.videoAspectRatio > 0.2f
            ? std::clamp(uiState_.videoAspectRatio, 0.5f, 4.0f) : 16.0f / 9.0f;
        if (fitMode_ == 2) return; // Stretch.
        if (fitMode_ == 0) // Fit.
        {
            if (sourceAspect > targetAspect)
                videoHalfHeight = targetHalfWidth / sourceAspect;
            else
                videoHalfWidth = targetHalfHeight * sourceAspect;
            return;
        }
        // Fill and Crop both fill the selected frame without distortion by trimming UVs.
        if (sourceAspect > targetAspect)
        {
            const float visible = targetAspect / sourceAspect;
            u0 = (1.0f - visible) * 0.5f;
            u1 = 1.0f - u0;
        }
        else if (sourceAspect < targetAspect)
        {
            const float visible = sourceAspect / targetAspect;
            v0 = (1.0f - visible) * 0.5f;
            v1 = 1.0f - v0;
        }
    }

    bool VideoSurface::ValidateFrames() const
    {
        if (!mappingView_) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        return header->magic == wxl_video_shared::kMagic &&
               header->version == wxl_video_shared::kVersion &&
               header->headerBytes == sizeof(*header) &&
               header->width == wxl_video_shared::kWidth &&
               header->height == wxl_video_shared::kHeight &&
               header->stride == wxl_video_shared::kStride &&
               header->frameBytes == wxl_video_shared::kFrameBytes;
    }

    bool VideoSurface::ConnectFrames()
    {
        if (mappingView_) return ValidateFrames();
        const DWORD now = GetTickCount();
        if (now - lastConnectAttempt_ < 1000) return false;
        lastConnectAttempt_ = now;

        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, wxl_video_shared::kMappingName);
        if (!mapping_) return false;
        mappingView_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, wxl_video_shared::kMappingBytes);
        if (!mappingView_ || !ValidateFrames())
        {
            if (mappingView_) UnmapViewOfFile(mappingView_);
            mappingView_ = nullptr;
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        return true;
    }

    void VideoSurface::ReleaseTexture()
    {
        if (texture_)
        {
            texture_->Release();
            texture_ = nullptr;
        }
        textureDevice_ = nullptr;
        lastFrameSequence_ = 0;
    }

    bool VideoSurface::UploadLatestFrame(IDirect3DDevice9* device)
    {
        if (!device || !ConnectFrames() || !ValidateFrames()) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        if (!header->ready) return false;

        const unsigned active = static_cast<unsigned>(header->activeIndex) & 1u;
        const LONG before = header->slotSequence[active];
        if (before <= 0 || before == lastFrameSequence_) return texture_ != nullptr;
        std::memcpy(frameScratch_.data(), wxl_video_shared::Pixels(mappingView_, active),
                    frameScratch_.size());
        MemoryBarrier();
        if (header->slotSequence[active] != before) return texture_ != nullptr;

        if (ambientLighting_ && GetTickCount() - lastAmbientSampleAt_ >= 100)
        {
            SampleGlow();
            lastAmbientSampleAt_ = GetTickCount();
        }

        if (textureDevice_ != device)
            ReleaseTexture();
        if (!texture_)
        {
            if (FAILED(device->CreateTexture(wxl_video_shared::kWidth, wxl_video_shared::kHeight,
                                             1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                             D3DPOOL_DEFAULT, &texture_, nullptr)) || !texture_)
            {
                status_ = "D3D9 could not create the world-screen texture.";
                return false;
            }
            textureDevice_ = device;
        }

        D3DLOCKED_RECT locked{};
        if (FAILED(texture_->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
            return false;
        const auto* source = frameScratch_.data();
        auto* destination = static_cast<uint8_t*>(locked.pBits);
        for (uint32_t row = 0; row < wxl_video_shared::kHeight; ++row)
            std::memcpy(destination + size_t(row) * locked.Pitch,
                        source + size_t(row) * wxl_video_shared::kStride,
                        wxl_video_shared::kStride);
        texture_->UnlockRect(0);
        lastFrameSequence_ = before;

        char message[128]{};
        std::snprintf(message, sizeof(message), "Live video frame %ld connected (640x360).", before);
        status_ = message;
        return true;
    }

    void VideoSurface::DrawWorldScreen(IDirect3DDevice9* device, void* sceneDepthRaw,
                                       bool beforeWorld)
    {
        if (!device || !placed_ || !visible_ || wxl::game::world::CurrentMapId() != mapId_)
            return;

        ++drawAttempts_;
        // Before the world only the draw itself: the frame is uploaded at the end of the scene (OnWorldSceneEnd).
        if (!beforeWorld) UploadLatestFrame(device);

        const float halfWidth = width_ * 0.5f;
        const float halfHeight = halfWidth / ScreenAspectRatio();
        float videoHalfWidth = halfWidth;
        float videoHalfHeight = halfHeight;
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        VideoLayout(halfWidth, halfHeight, videoHalfWidth, videoHalfHeight, u0, v0, u1, v1);
        // A missing helper/video must still produce an unmistakable world-space test card.  The
        // previous nearly-black placeholder was effectively invisible in dark zones and, because it
        // also honored world depth, gave no way to distinguish placement from playback failures.
        const D3DCOLOR topLeft = texture_ ? 0xFFFFFFFFu : 0xFFFF2D55u;
        const D3DCOLOR topRight = texture_ ? 0xFFFFFFFFu : 0xFFFFD60Au;
        const D3DCOLOR bottomLeft = texture_ ? 0xFFFFFFFFu : 0xFF00E5FFu;
        const D3DCOLOR bottomRight = texture_ ? 0xFFFFFFFFu : 0xFF7C4DFFu;
        const Vertex vertices[4] = {
            { center_[0] - right_[0] * videoHalfWidth, center_[1] - right_[1] * videoHalfWidth,
              center_[2] + videoHalfHeight, topLeft, u1, v0 },
            { center_[0] + right_[0] * videoHalfWidth, center_[1] + right_[1] * videoHalfWidth,
              center_[2] + videoHalfHeight, topRight, u0, v0 },
            { center_[0] - right_[0] * videoHalfWidth, center_[1] - right_[1] * videoHalfWidth,
              center_[2] - videoHalfHeight, bottomLeft, u1, v1 },
            { center_[0] + right_[0] * videoHalfWidth, center_[1] + right_[1] * videoHalfWidth,
              center_[2] - videoHalfHeight, bottomRight, u0, v1 },
        };
        const D3DCOLOR outlineColor = ambientLighting_ ? static_cast<D3DCOLOR>(ambientColor_)
            : (frameStyle_ > 0 ? FrameAccentColor(frameStyle_) : 0xFFFFFF00u);
        const Vertex border[5] = {
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] + halfHeight, outlineColor, 0.0f, 0.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] + halfHeight, outlineColor, 0.0f, 0.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] - halfHeight, outlineColor, 0.0f, 0.0f },
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] - halfHeight, outlineColor, 0.0f, 0.0f },
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] + halfHeight, outlineColor, 0.0f, 0.0f },
        };

        IDirect3DStateBlock9* state = nullptr;
        const HRESULT stateResult = CaptureState(device, &state);
        lastStateResult_ = stateResult;
        if (FAILED(stateResult) || !state)
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen state capture failed hr=0x%08lX",
                          static_cast<unsigned long>(stateResult));
                lastDrawFailureLog_ = now;
            }
            return;
        }
        // Depth-tested only before the world: the freshly cleared depth is seeded with the screen, and nearer characters
        // and props then draw over it. At the end of the scene (only the through-world mode draws there) it isn't, and
        // the depth surface is left alone, as the sign and the glow leave it (754023c): the world text batch is next.
        auto* sceneDepth = static_cast<IDirect3DSurface9*>(sceneDepthRaw);
        const bool depthTested = sceneDepth && beforeWorld;
        IDirect3DSurface9* oldDepth = nullptr;
        bool swapDepth = false;
        if (depthTested)
        {
            // The pre-world screen draws into the depth already bound (the hook passes it): no SetDepthStencilSurface
            // then, which other modules hook (see OffsetDepth).
            device->GetDepthStencilSurface(&oldDepth);
            swapDepth = oldDepth != sceneDepth;
            if (swapDepth) device->SetDepthStencilSurface(sceneDepth);
        }
        float depthRange = 1.0f;

        // Build 12340 creates a pure D3D9 device, so GetTransform is unavailable.  Use WarcraftXL's
        // exact scene-matrix bank while the world-scene-end event still owns it.  Camera globals are
        // close enough to place the quad visually, but not guaranteed to use the identical projection
        // that populated this depth surface; that mismatch makes a live video texture appear in broken
        // patches even though the depth-free diagnostic card is complete.
        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{};
        D3DMATRIX projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11))
        {
            if (swapDepth) device->SetDepthStencilSurface(oldDepth);
            if (oldDepth) oldDepth->Release();
            state->Apply();
            state->Release();
            return;
        }
        // The projection the world is rendered with, not the one it was given (see RenderProjection): the same place
        // on screen either way, but only this one gives the world's depth.
        float rendered[16];
        if (RenderProjection(rendered)) std::memcpy(&projection._11, rendered, sizeof(rendered));
        if (depthTested)
        {
            D3DVIEWPORT9 viewport{};
            if (SUCCEEDED(device->GetViewport(&viewport)))
            {
                viewport.MinZ = 0.0f;
                viewport.MaxZ = WorldPassMaxZ();
                device->SetViewport(&viewport);
                depthRange = viewport.MaxZ - viewport.MinZ;
            }
        }
        const float* sceneView = &view._11;
        const float* sceneProjection = &projection._11;

        const float relativeCenter[4] = {
            center_[0] - eye[0], center_[1] - eye[1], center_[2] - eye[2], 1.0f
        };
        float viewCenter[4]{};
        float clipCenter[4]{};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
                viewCenter[column] += relativeCenter[row] * sceneView[row * 4 + column];
            for (int row = 0; row < 4; ++row)
                clipCenter[column] += viewCenter[row] * sceneProjection[row * 4 + column];
        }
        projectedClipW_ = clipCenter[3];
        projectedCenterValid_ = std::isfinite(projectedClipW_) &&
                                std::abs(projectedClipW_) > 0.0001f;
        if (projectedCenterValid_)
        {
            projectedCenter_[0] = clipCenter[0] / projectedClipW_;
            projectedCenter_[1] = clipCenter[1] / projectedClipW_;
            projectedCenter_[2] = clipCenter[2] / projectedClipW_;
            projectedCenterValid_ = Finite3(projectedCenter_);
        }

        // The depth offset (the panel's "Depth offset") in the projection, not as D3DRS_DEPTHBIAS (see OffsetDepth). It
        // only matters where the screen is depth-tested.
        D3DMATRIX drawProjection = projection;
        if (depthTested) OffsetDepth(drawProjection, depthBias_, depthRange);
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &drawProjection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, texture_);
        if (texture_)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_ZENABLE, depthTested ? D3DZB_TRUE : D3DZB_FALSE);
        // Depth writes only with the depth test (the pre-world screen seeds the world's depth); otherwise depth
        // buffering is off and nothing is written, so ZWRITEENABLE, which other modules hook, is left alone.
        if (depthTested) device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        // Seed the cleared depth buffer unconditionally; world geometry rendered afterward uses its
        // normal depth test and can overwrite the screen wherever it is closer to the camera.
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        // No D3DRS_DEPTHBIAS: the offset is in drawProjection (OffsetDepth says why).

        const HRESULT drawResult = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));

        HRESULT borderResult = S_OK;
        if (outline_ || frameStyle_ > 0 || ambientLighting_)
        {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

            if (frameStyle_ > 0 || ambientLighting_)
            {
                borderResult = DrawDecorativeFrame(
                    frameStyle_, halfWidth, halfHeight, false,
                    static_cast<D3DCOLOR>(ambientColor_),
                    [&](float left, float right, float bottom, float top, D3DCOLOR color)
                    {
                        const Vertex quad[4] = {
                            { center_[0] + right_[0] * left, center_[1] + right_[1] * left,
                              center_[2] + top, color, 0.0f, 0.0f },
                            { center_[0] + right_[0] * right, center_[1] + right_[1] * right,
                              center_[2] + top, color, 0.0f, 0.0f },
                            { center_[0] + right_[0] * left, center_[1] + right_[1] * left,
                              center_[2] + bottom, color, 0.0f, 0.0f },
                            { center_[0] + right_[0] * right, center_[1] + right_[1] * right,
                              center_[2] + bottom, color, 0.0f, 0.0f },
                        };
                        return device->DrawPrimitiveUP(
                            D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
                    });
            }
            if (outline_)
            {
                const HRESULT outlineResult = device->DrawPrimitiveUP(
                    D3DPT_LINESTRIP, 4, border, sizeof(Vertex));
                if (FAILED(outlineResult) && SUCCEEDED(borderResult)) borderResult = outlineResult;
            }
        }
        lastDrawResult_ = FAILED(drawResult) ? drawResult : borderResult;
        if (SUCCEEDED(drawResult) && SUCCEEDED(borderResult))
        {
            ++drawSuccesses_;
        }
        else
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen draw failed fill=0x%08lX border=0x%08lX",
                          static_cast<unsigned long>(drawResult),
                          static_cast<unsigned long>(borderResult));
                lastDrawFailureLog_ = now;
            }
        }

        if (swapDepth) device->SetDepthStencilSurface(oldDepth);
        if (oldDepth) oldDepth->Release();
        state->Apply(); // includes the viewport
        state->Release();
    }

    bool VideoSurface::GetPinnedRect(IDirect3DDevice9* device, float& left, float& top,
                                     float& right, float& bottom) const
    {
        if (!device) return false;
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
            return false;

        const float viewportWidth = static_cast<float>(viewport.Width);
        const float viewportHeight = static_cast<float>(viewport.Height);
        float leftFraction = pinnedLeft_;
        float topFraction = pinnedTop_;
        float widthFraction = pinnedWidth_;
        wxl_video_shared::ClampPinnedLayout(leftFraction, topFraction, widthFraction);
        const float width = viewportWidth * widthFraction;
        const float height = width / ScreenAspectRatio();
        topFraction = std::clamp(topFraction, 0.0f,
                                 std::max(0.0f, 1.0f - height / viewportHeight));
        left = static_cast<float>(viewport.X) + leftFraction * viewportWidth - 0.5f;
        top = static_cast<float>(viewport.Y) + topFraction * viewportHeight - 0.5f;
        right = left + width;
        bottom = top + height;
        return true;
    }

    void VideoSurface::DrawPinnedScreen(IDirect3DDevice9* device)
    {
        if (!device || !visible_) return;

        UploadLatestFrame(device);
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (!GetPinnedRect(device, left, top, right, bottom)) return;

        const D3DCOLOR topLeft = texture_ ? 0xFFFFFFFFu : 0xFFFF2D55u;
        const D3DCOLOR topRight = texture_ ? 0xFFFFFFFFu : 0xFFFFD60Au;
        const D3DCOLOR bottomLeft = texture_ ? 0xFFFFFFFFu : 0xFF00E5FFu;
        const D3DCOLOR bottomRight = texture_ ? 0xFFFFFFFFu : 0xFF7C4DFFu;
        const float targetHalfWidth = (right - left) * 0.5f;
        const float targetHalfHeight = (bottom - top) * 0.5f;
        float videoHalfWidth = targetHalfWidth;
        float videoHalfHeight = targetHalfHeight;
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        VideoLayout(targetHalfWidth, targetHalfHeight, videoHalfWidth, videoHalfHeight,
                    u0, v0, u1, v1);
        const float centerX = (left + right) * 0.5f;
        const float centerY = (top + bottom) * 0.5f;
        const float videoLeft = centerX - videoHalfWidth;
        const float videoRight = centerX + videoHalfWidth;
        const float videoTop = centerY - videoHalfHeight;
        const float videoBottom = centerY + videoHalfHeight;
        const ScreenVertex vertices[4] = {
            {videoLeft, videoTop, 0.0f, 1.0f, topLeft, u0, v0},
            {videoRight, videoTop, 0.0f, 1.0f, topRight, u1, v0},
            {videoLeft, videoBottom, 0.0f, 1.0f, bottomLeft, u0, v1},
            {videoRight, videoBottom, 0.0f, 1.0f, bottomRight, u1, v1},
        };
        const D3DCOLOR outlineColor = ambientLighting_ ? static_cast<D3DCOLOR>(ambientColor_)
            : (frameStyle_ > 0 ? FrameAccentColor(frameStyle_) : 0xFFFFFF00u);
        const ScreenVertex border[5] = {
            {left, top, 0.0f, 1.0f, outlineColor, 0.0f, 0.0f},
            {right, top, 0.0f, 1.0f, outlineColor, 0.0f, 0.0f},
            {right, bottom, 0.0f, 1.0f, outlineColor, 0.0f, 0.0f},
            {left, bottom, 0.0f, 1.0f, outlineColor, 0.0f, 0.0f},
            {left, top, 0.0f, 1.0f, outlineColor, 0.0f, 0.0f},
        };

        IDirect3DStateBlock9* state = nullptr;
        const HRESULT stateResult = CaptureState(device, &state);
        lastStateResult_ = stateResult;
        if (FAILED(stateResult) || !state) return;

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, texture_);
        if (texture_)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE); // depth buffering off: nothing written (no ZWRITEENABLE: OffsetDepth)
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);

        const HRESULT drawResult = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ScreenVertex));
        HRESULT borderResult = S_OK;
        if (outline_ || frameStyle_ > 0 || ambientLighting_)
        {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

            if (frameStyle_ > 0 || ambientLighting_)
            {
                borderResult = DrawDecorativeFrame(
                    frameStyle_, targetHalfWidth, targetHalfHeight, ambientLighting_ != 0,
                    static_cast<D3DCOLOR>(ambientColor_),
                    [&](float rectLeft, float rectRight, float rectBottom, float rectTop,
                        D3DCOLOR color)
                    {
                        const ScreenVertex quad[4] = {
                            {centerX + rectLeft, centerY - rectTop, 0.0f, 1.0f,
                             color, 0.0f, 0.0f},
                            {centerX + rectRight, centerY - rectTop, 0.0f, 1.0f,
                             color, 0.0f, 0.0f},
                            {centerX + rectLeft, centerY - rectBottom, 0.0f, 1.0f,
                             color, 0.0f, 0.0f},
                            {centerX + rectRight, centerY - rectBottom, 0.0f, 1.0f,
                             color, 0.0f, 0.0f},
                        };
                        return device->DrawPrimitiveUP(
                            D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
                    });
            }
            if (outline_)
            {
                const HRESULT outlineResult = device->DrawPrimitiveUP(
                    D3DPT_LINESTRIP, 4, border, sizeof(ScreenVertex));
                if (FAILED(outlineResult) && SUCCEEDED(borderResult)) borderResult = outlineResult;
            }
        }
        lastDrawResult_ = FAILED(drawResult) ? drawResult : borderResult;
        if (SUCCEEDED(drawResult) && SUCCEEDED(borderResult)) ++drawSuccesses_;
        state->Apply();
        state->Release();
    }

    void VideoSurface::OnWorldSceneEnd(const wxl::events::WorldSceneEndArgs& args)
    {
        ++worldSceneCalls_;
        inWorldPass_ = false;
        // The gate above should match every world pass; if it never does (another module drawing the world into a
        // depth surface of its own, say), fall back to the first colour + depth clear of the frame rather than lose
        // the screen.
        if (depthTest_ && visible_ && placed_ && !gateOff_ && worldRenderEvents_ && !pinned_ && wxl::game::world::CurrentMapId() == mapId_)
        {
            gateMisses_ = preWorldDrawn_ ? 0u : gateMisses_ + 1u;
            if (gateMisses_ >= 300u)
            {
                gateOff_ = true;
                if (api_) api_->Log(WXL_LOG_WARN, kTag, "pre-world screen: the world pass's clear never matched its depth surface; drawing at the first colour and depth clear of the frame instead");
            }
        }
        if (!loggedFirstWorldScene_ && api_)
        {
            api_->Log(WXL_LOG_INFO, kTag,
                      "first world scene device=%p depth=%p currentMap=%d placed=%d savedMap=%d",
                      args.device, args.sceneDepth, wxl::game::world::CurrentMapId(),
                      placed_ ? 1 : 0, mapId_);
            loggedFirstWorldScene_ = true;
        }
        if (pinned_)
            DrawPinnedScreen(static_cast<IDirect3DDevice9*>(args.device));
        else
        {
            auto* device = static_cast<IDirect3DDevice9*>(args.device);
            if (depthTest_ && device && placed_ && visible_ && wxl::game::world::CurrentMapId() == mapId_)
                UploadLatestFrame(device); // for the next frame's pre-world screen (and the glow's samples)
            DrawAmbientGlow(device, args.sceneDepth);
            if (!depthTest_)
                DrawWorldScreen(device, args.sceneDepth, false);
            DrawStonewall(device, args.sceneDepth);
        }
    }

    void VideoSurface::OnUpdate(const wxl::events::UpdateArgs& args)
    {
        if (args.timeMs - lastVolumeUpdate_ < 200) return;
        lastVolumeUpdate_ = args.timeMs;

        int targetVolume = masterVolume_;
        if (spatialAudio_ && placed_)
        {
            namespace world = wxl::game::world;
            if (world::CurrentMapId() != mapId_)
            {
                targetVolume = 0;
            }
            else
            {
                const unsigned long long guid = world::ActivePlayerGuid();
                void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
                float position[3]{};
                if (!player)
                {
                    targetVolume = 0;
                }
                else
                {
                    world::UnitPosition(player, position);
                    const float dx = position[0] - center_[0];
                    const float dy = position[1] - center_[1];
                    const float dz = position[2] - center_[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (!std::isfinite(distance) || distance >= silentDistance_)
                        targetVolume = 0;
                    else if (distance > fullVolumeDistance_)
                    {
                        const float range = std::max(1.0f, silentDistance_ - fullVolumeDistance_);
                        const float gain = 1.0f - (distance - fullVolumeDistance_) / range;
                        targetVolume = static_cast<int>(std::lround(masterVolume_ * gain));
                    }
                }
            }
        }
        SendVolume(targetVolume);
    }

    void VideoSurface::ReleaseStonewallTextures()
    {
        for (IDirect3DTexture9** texture : { &signTexture_, &tvTexture_, &sparkleTexture_ })
            if (*texture) { (*texture)->Release(); *texture = nullptr; }
        stonewallDevice_ = nullptr;
    }

    bool VideoSurface::EnsureStonewallTextures(IDirect3DDevice9* device)
    {
        if (stonewallDevice_ != device) ReleaseStonewallTextures();
        if (signTexture_ && tvTexture_ && sparkleTexture_) return true;
        if (stonewallFailed_) return false;
        stonewallDevice_ = device;
        // The sign as shipped (its fonts come with Office, not Windows); rendered here only if the file is missing.
        art::Sign sign;
        const std::wstring directory = ExtensionDirectory();
        const std::wstring path = directory.empty() ? std::wstring{} : (std::filesystem::path(directory) / L"stonewall-sign.bin").wstring();
        if (path.empty() || !art::LoadSign(path.c_str(), sign))
        {
            sign = art::MakeSign(1024, 256);
            if (api_) api_->Log(WXL_LOG_WARN, kTag, "Stonewall TV: stonewall-sign.bin missing or unreadable; rendered the sign here.");
        }
        signSplit_ = sign.split;
        signArc_[0] = sign.arcU; signArc_[1] = sign.arcV; signArc_[2] = sign.arcInner; signArc_[3] = sign.arcOuter;
        signArc_[4] = sign.arcEnd;
        signCenter_ = sign.centerU;
        {
            int x0 = sign.w, x1 = -1, y0 = sign.h, y1 = -1;
            for (int y = 0; y < sign.h; ++y)
                for (int x = 0; x < sign.w; ++x)
                {
                    const size_t at = size_t(y) * sign.w + x;
                    const uint32_t alpha = std::max(sign.pixels[at] >> 24, sign.tv.empty() ? 0u : sign.tv[at] >> 24);
                    if (alpha < 64u) continue;
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
            if (x1 >= x0 && y1 >= y0 && sign.w > 0 && sign.h > 0)
            {
                signInk_[0] = static_cast<float>(x0) / sign.w;
                signInk_[1] = static_cast<float>(x1 + 1) / sign.w;
                signInk_[2] = static_cast<float>(y0) / sign.h;
                signInk_[3] = static_cast<float>(y1 + 1) / sign.h;
            }
        }
        signAspect_ = sign.w > 0 ? static_cast<float>(sign.h) / sign.w : 0.25f;
        signTexture_ = MakeTexture(device, sign.w, sign.h, sign.pixels);
        tvTexture_ = MakeTexture(device, sign.w, sign.h, sign.tv);
        sparkleTexture_ = MakeTexture(device, 64, 64, art::SparklePixels(64));
        if (!signTexture_ || !tvTexture_ || !sparkleTexture_)
        {
            stonewallFailed_ = true;
            ReleaseStonewallTextures();
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "Stonewall TV: could not create its textures.");
            return false;
        }
        return true;
    }

    namespace
    {
        uint32_t Hash(uint32_t x)
        {
            x ^= x >> 16; x *= 0x7FEB352Du;
            x ^= x >> 15; x *= 0x846CA68Bu;
            return x ^ (x >> 16);
        }
        float Hash01(uint32_t x) { return (Hash(x) & 0xFFFFFFu) / 16777216.0f; }
        D3DCOLOR Grey(float level)
        {
            const uint32_t c = static_cast<uint32_t>(std::clamp(level, 0.0f, 1.0f) * 255.0f + 0.5f);
            return (c << 24) | (c << 16) | (c << 8) | c;
        }
    }

    // After the world, so it blends over what's behind it; depth-tested (characters walk in front), no depth writes.
    void VideoSurface::DrawStonewall(IDirect3DDevice9* device, void* sceneDepthRaw)
    {
        if (!device || !placed_ || !visible_ || !stonewall_ || wxl::game::world::CurrentMapId() != mapId_) return;
        if (!EnsureStonewallTextures(device)) return;

        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{}, projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11)) return;

        IDirect3DStateBlock9* state = nullptr;
        if (FAILED(CaptureState(device, &state)) || !state) return;
        // Not depth-tested: at the end of the world pass that rejected every pixel in game (and took the player names,
        // drawn next, with it), while untested drawing there is fine.
        (void)sceneDepthRaw;

        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x07u);
        // Depth buffering off: no depth test, nothing written. Neither ZWRITEENABLE nor DEPTHBIAS is set (other modules
        // hook them and take what they see for the client's: OffsetDepth).
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

        // Positions in the screen's plane: h yards toward the viewer's right of its centre (the screen's u runs that way:
        // its viewer-right edge is center - right * half), z yards up, `lift` yards toward the camera.
        float toward[3] = { right_[1], -right_[0], 0.0f };
        if ((eye[0] - center_[0]) * toward[0] + (eye[1] - center_[1]) * toward[1] < 0.0f) { toward[0] = -toward[0]; toward[1] = -toward[1]; }
        const auto quad = [&](IDirect3DTexture9* texture, float h0, float h1, float z0, float z1, float u0, float u1,
                              float lift, D3DCOLOR colour) {
            const auto at = [&](float h, float z, float u, float v) {
                return Vertex{ center_[0] - right_[0] * h + toward[0] * lift, center_[1] - right_[1] * h + toward[1] * lift,
                               center_[2] + z, colour, u, v };
            };
            const Vertex v[4] = { at(h0, z1, u0, 0.0f), at(h1, z1, u1, 0.0f), at(h0, z0, u0, 1.0f), at(h1, z0, u1, 1.0f) };
            device->SetTexture(0, texture);
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
        };

        // On the frame: the top of the screen (its height follows the aspect setting) plus however far the frame's top rail
        // reaches over it (a rail is 6% of the half-height).
        static constexpr float kFrameTopRails[9] = { 0.0f, 1.65f, 1.95f, 1.42f, 1.35f, 2.05f, 1.75f, 2.10f, 0.85f };
        const float W = width_, halfHeight = W * 0.5f / ScreenAspectRatio(), rail = halfHeight * 0.06f;
        const float top = halfHeight + rail * kFrameTopRails[std::clamp(frameStyle_, 0, 8)];
        const float t = static_cast<float>(GetTickCount() % 3600000u) / 1000.0f;

        // The sign, centred above the screen. The rainbow and "Stonewall" hum; "TV" is a tube on its way out: every few
        // seconds it stutters (60 ms on/off for about half a second).
        // Sized and placed by the tubes, not the texture round them: their width signSize_ of the screen's, centred, their
        // bottom on the frame (texture v runs down from signZ + signH to signZ).
        const float signW = W * signSize_ / std::max(0.05f, signInk_[1] - signInk_[0]), signH = signW * signAspect_;
        const float signZ = top - (1.0f - signInk_[3]) * signH;
        const float left = -signW * 0.5f * (signInk_[0] + signInk_[1]);
        const float hum = 0.93f + 0.07f * std::sin(t * 1.3f);
        quad(signTexture_, left, left + signW, signZ, signZ + signH, 0.0f, 1.0f, 0.0f, Grey(hum));
        const uint32_t epoch = static_cast<uint32_t>(t / 7.0f);
        const float burst = epoch * 7.0f + Hash01(epoch * 3u + 1u) * 5.5f;
        float tv = 0.96f;
        if (t >= burst && t < burst + 0.55f) tv = (Hash(static_cast<uint32_t>(t / 0.06f)) & 3u) ? 1.0f : 0.12f;
        quad(tvTexture_, left, left + signW, signZ, signZ + signH, 0.0f, 1.0f, 0.0f, Grey(tv * hum));

        // Glitter on the rainbow: sparkles that twinkle along its visible arc (up to where it passes behind the S), each
        // somewhere new every time it flashes.
        const float arcU = signArc_[0], arcV = signArc_[1], inner = signArc_[2], outer = signArc_[3], arcEnd = signArc_[4];
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const float period = 1.1f + Hash01(i * 7u + 3u) * 1.6f, phase = Hash01(i * 11u + 5u) * period;
            const float cycles = (t + phase) / period;
            const uint32_t cycle = static_cast<uint32_t>(cycles);
            const float flash = std::pow(std::sin((cycles - static_cast<float>(cycle)) * 3.14159265f), 6.0f);
            if (flash < 0.02f) continue;
            const uint32_t seed = i * 977u + cycle * 131u;
            const float r = inner + (outer - inner) * (0.15f + 0.7f * Hash01(seed + 1u)); // in texture heights
            const float angle = arcEnd + (3.14159265f - arcEnd) * (0.04f + 0.92f * Hash01(seed));
            const float u = arcU + std::cos(angle) * r * signAspect_, v = arcV - std::sin(angle) * r;
            const float sh = left + u * signW, sz = signZ + (1.0f - v) * signH, size = signH * 0.16f * (0.5f + 0.5f * flash);
            quad(sparkleTexture_, sh - size * 0.5f, sh + size * 0.5f, sz - size * 0.5f, sz + size * 0.5f, 0.0f, 1.0f, 0.005f,
                 Grey(flash));
        }
        device->SetTexture(0, nullptr);

        state->Apply();
        state->Release();
    }

    // The picture's edges in zones: a band 15% deep along each edge, every third pixel each way (BGRA, top row first).
    void VideoSurface::SampleGlow()
    {
        constexpr int width = static_cast<int>(wxl_video_shared::kWidth), height = static_cast<int>(wxl_video_shared::kHeight);
        constexpr size_t stride = wxl_video_shared::kStride;
        if (frameScratch_.size() < stride * height) return;
        const auto average = [&](int x0, int x1, int y0, int y1, float* out) {
            uint64_t red = 0, green = 0, blue = 0, count = 0;
            for (int y = y0; y < y1; y += 3)
                for (int x = x0; x < x1; x += 3)
                {
                    const uint8_t* pixel = frameScratch_.data() + size_t(y) * stride + size_t(x) * 4;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                    ++count;
                }
            const float scale = count ? 1.0f / (255.0f * static_cast<float>(count)) : 0.0f;
            out[0] = red * scale;
            out[1] = green * scale;
            out[2] = blue * scale;
        };
        const int bandX = width * 15 / 100, bandY = height * 15 / 100;
        for (int i = 0; i < kGlowAcross; ++i)
        {
            const int x0 = width * i / kGlowAcross, x1 = width * (i + 1) / kGlowAcross;
            average(x0, x1, 0, bandY, glowTarget_[i]);
            average(x0, x1, height - bandY, height, glowTarget_[kGlowAcross + i]);
        }
        for (int i = 0; i < kGlowDown; ++i)
        {
            const int y0 = height * i / kGlowDown, y1 = height * (i + 1) / kGlowDown;
            average(0, bandX, y0, y1, glowTarget_[2 * kGlowAcross + i]);
            average(width - bandX, width, y0, y1, glowTarget_[2 * kGlowAcross + kGlowDown + i]);
        }
        float mean[3]{};
        for (const auto& zone : glowTarget_)
            for (int c = 0; c < 3; ++c) mean[c] += zone[c] / kGlowZones;
        const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        ambientColor_ = 0xFF000000u | (byte(mean[0]) << 16) | (byte(mean[1]) << 8) | byte(mean[2]);
        glowSampled_ = true;
    }

    // A soft additive ring in the edge colours, a little behind the screen: grid lines three deep outside each edge
    // (fading 1, 0.55, 0.18, 0) and at the zone boundaries along it; every cell but the inner ones is drawn. Positions are
    // (h, z) in the screen's plane as for the sign: h toward the picture's right (u), z up.
    void VideoSurface::DrawAmbientGlow(IDirect3DDevice9* device, void* sceneDepthRaw)
    {
        if (!device || !placed_ || !visible_ || !ambientLighting_ || !glowSampled_ ||
            wxl::game::world::CurrentMapId() != mapId_)
            return;

        const DWORD now = GetTickCount();
        const float dt = lastGlowDrawAt_ ? std::min(0.5f, (now - lastGlowDrawAt_) / 1000.0f) : 10.0f;
        lastGlowDrawAt_ = now;
        const float follow = 1.0f - std::exp(-dt / 0.3f);
        for (int zone = 0; zone < kGlowZones; ++zone)
            for (int c = 0; c < 3; ++c) glow_[zone][c] += (glowTarget_[zone][c] - glow_[zone][c]) * follow;

        // From the frame's outer edge (the ring of each style, in rails: 3.5% of the half-width across, 6% of the
        // half-height up and down; ornaments past it just get some of the light), so it never washes over the picture.
        static constexpr float kSideRails[9] = { 0.0f, 1.65f, 1.25f, 0.80f, 1.35f, 0.65f, 1.25f, 1.15f, 0.85f };
        static constexpr float kTopRails[9] = { 0.0f, 1.65f, 1.25f, 0.80f, 1.35f, 0.65f, 1.25f, 2.10f, 0.85f };
        const int style = std::clamp(frameStyle_, 0, 8);
        const float halfWidth = width_ * 0.5f, halfHeight = halfWidth / ScreenAspectRatio();
        const float innerW = halfWidth + halfWidth * 0.035f * kSideRails[style];
        const float innerH = halfHeight + halfHeight * 0.06f * kTopRails[style];
        const float spread = halfHeight * 0.6f;
        static constexpr float kStep[4] = { 0.0f, 0.15f, 0.45f, 1.0f };
        static constexpr float kFade[4] = { 1.0f, 0.55f, 0.18f, 0.0f };
        static constexpr float kStrength = 0.75f;

        struct Line { float at; int fade; };
        const auto lines = [&](float inner, int zones, Line* out) {
            int n = 0;
            for (int i = 3; i >= 1; --i) out[n++] = { -(inner + spread * kStep[i]), i };
            for (int k = 0; k <= zones; ++k) out[n++] = { -inner + 2.0f * inner * k / zones, 0 };
            for (int i = 1; i <= 3; ++i) out[n++] = { inner + spread * kStep[i], i };
            return n;
        };
        Line across[kGlowAcross + 7], down[kGlowDown + 7];
        const int columns = lines(innerW, kGlowAcross, across), rows = lines(innerH, kGlowDown, down);

        // An edge's colour at t (0..1 along it), between its zones' centres.
        const auto along = [&](int first, int zones, float t, float* out) {
            const float x = std::clamp(t, 0.0f, 1.0f) * zones - 0.5f;
            const int i0 = std::clamp(static_cast<int>(std::floor(x)), 0, zones - 1), i1 = std::min(i0 + 1, zones - 1);
            const float f = std::clamp(x - static_cast<float>(i0), 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) out[c] = glow_[first + i0][c] + (glow_[first + i1][c] - glow_[first + i0][c]) * f;
        };
        const auto colourAt = [&](const Line& column, const Line& row) -> D3DCOLOR {
            const float h = std::clamp(column.at, -innerW, innerW), z = std::clamp(row.at, -innerH, innerH);
            const float u = (h + innerW) / (2.0f * innerW), v = (innerH - z) / (2.0f * innerH);
            const bool topBottom = row.fade > 0 || std::abs(row.at) >= innerH * 0.999f;
            const bool side = column.fade > 0 || std::abs(column.at) >= innerW * 0.999f;
            float rgb[3]{}, other[3]{};
            const int horizontal = z > 0.0f ? 0 : kGlowAcross;                                 // top or bottom zones
            const int vertical = 2 * kGlowAcross + (h > 0.0f ? kGlowDown : 0);                // left or right zones
            if (topBottom) along(horizontal, kGlowAcross, u, rgb);
            if (side)
            {
                along(vertical, kGlowDown, v, other);
                for (int c = 0; c < 3; ++c) rgb[c] = topBottom ? 0.5f * (rgb[c] + other[c]) : other[c];
            }
            const float alpha = kStrength * kFade[column.fade] * kFade[row.fade];
            // A little more saturated than the picture, as a glow reads.
            const float luma = 0.30f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
            const auto byte = [&](float value) {
                return static_cast<uint32_t>(std::clamp(luma + (value - luma) * 1.4f, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            return (a << 24) | (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
        };

        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        float toward[3] = { right_[1], -right_[0], 0.0f };
        if ((eye[0] - center_[0]) * toward[0] + (eye[1] - center_[1]) * toward[1] < 0.0f) { toward[0] = -toward[0]; toward[1] = -toward[1]; }
        constexpr float lift = -0.02f; // just behind the frame's plane
        const auto vertex = [&](int a, int b) {
            const Line& column = across[a];
            const Line& row = down[b];
            return Vertex{ center_[0] - right_[0] * column.at + toward[0] * lift,
                           center_[1] - right_[1] * column.at + toward[1] * lift,
                           center_[2] + row.at, colourAt(column, row), 0.0f, 0.0f };
        };
        Vertex triangles[(kGlowAcross + 6) * (kGlowDown + 6) * 6];
        int count = 0;
        for (int a = 0; a + 1 < columns; ++a)
            for (int b = 0; b + 1 < rows; ++b)
            {
                if (a >= 3 && a < 3 + kGlowAcross && b >= 3 && b < 3 + kGlowDown) continue; // behind the picture
                const Vertex v00 = vertex(a, b), v10 = vertex(a + 1, b), v01 = vertex(a, b + 1), v11 = vertex(a + 1, b + 1);
                triangles[count++] = v00; triangles[count++] = v10; triangles[count++] = v01;
                triangles[count++] = v10; triangles[count++] = v11; triangles[count++] = v01;
            }

        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{}, projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11)) return;

        IDirect3DStateBlock9* state = nullptr;
        if (FAILED(CaptureState(device, &state)) || !state) return;
        // Not depth-tested: at the end of the world pass that rejected every pixel in game (and took the player names,
        // drawn next, with it), while untested drawing there is fine.
        (void)sceneDepthRaw;

        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE); // light: adds, never darkens
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x07u);
        // Depth buffering off: no depth test, nothing written. Neither ZWRITEENABLE nor DEPTHBIAS is set (other modules
        // hook them and take what they see for the client's: OffsetDepth).
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(count / 3), triangles, sizeof(Vertex));

        state->Apply();
        state->Release();
    }

    void VideoSurface::OnDeviceLost(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::OnDeviceReset(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::OnWorldLeave(const wxl::events::WorldLeaveArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::DrawPlexLogin()
    {
        if (api_->UiButton("Open Plex sign-in"))
            SendUiCommand(wxl_video_shared::UiCommand::StartLogin);
    }

    void VideoSurface::DrawPlexServers()
    {
        api_->UiText("Choose a Plex server.");
        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.serverCount), wxl_video_shared::kUiMaxServers));
        for (uint32_t index = 0; index < count; ++index)
        {
            char label[192]{};
            std::snprintf(label, sizeof(label), "%u. %s", index + 1,
                          uiState_.servers[index].name[0]
                              ? uiState_.servers[index].name : "Plex server");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectServer,
                              static_cast<LONG>(index));
            api_->UiSameLine();
            api_->UiText(uiState_.servers[index].uri[0]
                             ? uiState_.servers[index].uri : "address unavailable");
        }
    }

    void VideoSurface::DrawPlexHome()
    {
        if (api_->UiButton("Choose another Plex server"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Servers));
        api_->UiSameLine();
        if (api_->UiButton("Search Plex"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Search));
        api_->UiSameLine();
        if (api_->UiButton("Watch Together"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Party));
        api_->UiSameLine();
        if (api_->UiButton("Settings"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Settings));
        api_->UiSameLine();
        if (api_->UiButton("Sign out of Plex"))
            SendUiCommand(wxl_video_shared::UiCommand::SignOut);

        api_->UiText("Plex home:");
        if (api_->UiButton("Continue Watching"))
            SendUiCommand(wxl_video_shared::UiCommand::OpenHomeSection, -1,
                          static_cast<LONG>(0));
        api_->UiSameLine();
        if (api_->UiButton("On Deck"))
            SendUiCommand(wxl_video_shared::UiCommand::OpenHomeSection, -1,
                          static_cast<LONG>(3));
        if (api_->UiButton("Recently Added Movies"))
            SendUiCommand(wxl_video_shared::UiCommand::OpenHomeSection, -1,
                          static_cast<LONG>(1));
        api_->UiSameLine();
        if (api_->UiButton("Recently Added TV"))
            SendUiCommand(wxl_video_shared::UiCommand::OpenHomeSection, -1,
                          static_cast<LONG>(2));
        api_->UiSameLine();
        if (api_->UiButton("Recently Watched"))
            SendUiCommand(wxl_video_shared::UiCommand::OpenHomeSection, -1,
                          static_cast<LONG>(4));

        if (!uiState_.watchlistLoaded)
            api_->UiText("Loading Plex watchlist...");
        else
        {
            char watchlist[96]{};
            std::snprintf(watchlist, sizeof(watchlist), "Watchlist (%u)",
                          uiState_.watchlistCount);
            if (api_->UiButton(watchlist))
                SendUiCommand(wxl_video_shared::UiCommand::OpenWatchlist);
            if (uiState_.watchlistError[0]) api_->UiText(uiState_.watchlistError);
        }

        api_->UiText("Plex libraries:");
        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.sectionCount), wxl_video_shared::kUiMaxSections));
        for (uint32_t index = 0; index < count; ++index)
        {
            char label[224]{};
            std::snprintf(label, sizeof(label), "%u. %s (%s)", index + 1,
                          uiState_.sections[index].title[0]
                              ? uiState_.sections[index].title : "Plex library",
                          uiState_.sections[index].type[0]
                              ? uiState_.sections[index].type : "unknown");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectSection,
                              static_cast<LONG>(index));
        }
    }

    void VideoSurface::DrawPlexLibrary()
    {
        if (uiState_.browseDepth > 0)
        {
            char back[192]{};
            std::snprintf(back, sizeof(back), "Back from %s",
                          uiState_.browseTitle[0] ? uiState_.browseTitle : "child items");
            if (api_->UiButton(back))
                SendUiCommand(wxl_video_shared::UiCommand::NavigateBack);
        }
        else if (api_->UiButton("Back to Plex libraries"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Home));
        if (uiState_.browseTitle[0]) api_->UiText(uiState_.browseTitle);
        api_->UiSeparator();

        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.itemCount), wxl_video_shared::kUiMaxItems));
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& item = uiState_.items[index];
            char label[256]{};
            if (item.grandparentTitle[0])
                std::snprintf(label, sizeof(label), "%u. %s - %s", index + 1,
                              item.grandparentTitle, item.title[0] ? item.title : "Plex item");
            else
                std::snprintf(label, sizeof(label), "%u. %s", index + 1,
                              item.title[0] ? item.title : "Plex item");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectItem,
                              static_cast<LONG>(index));
        }
        if (count == 0) api_->UiText("No Plex items found.");

        const bool hasPrevious = uiState_.pageOffset > 0;
        const bool hasNext = uiState_.pageSize > 0 &&
                             uiState_.pageOffset < uiState_.pageTotalSize &&
                             uiState_.pageSize <= uiState_.pageTotalSize - uiState_.pageOffset;
        if (hasPrevious && api_->UiButton("Previous page"))
            SendUiCommand(wxl_video_shared::UiCommand::PreviousPage);
        if (hasPrevious && hasNext) api_->UiSameLine();
        if (hasNext && api_->UiButton("Next page"))
            SendUiCommand(wxl_video_shared::UiCommand::NextPage);
        if (count > 0 && uiState_.pageTotalSize > 0)
        {
            char page[96]{};
            const uint32_t first = uiState_.pageOffset + 1;
            const uint32_t last = std::min(
                uiState_.pageOffset + count, uiState_.pageTotalSize);
            std::snprintf(page, sizeof(page), "Showing %u-%u of %u",
                          first, last, uiState_.pageTotalSize);
            api_->UiText(page);
        }
    }

    void VideoSurface::DrawPlexSearch()
    {
        api_->UiInputText("Plex search", search_, sizeof(search_));
        if (api_->UiButton("Search Plex now"))
            SendUiCommand(wxl_video_shared::UiCommand::Search, -1, 0, search_);
        api_->UiSameLine();
        if (api_->UiButton("Back to Plex libraries"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Home));
    }

    void VideoSurface::DrawPlexDetails()
    {
        if (api_->UiButton("Back to Plex items"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Library));
        api_->UiText(uiState_.item.title[0] ? uiState_.item.title : "Plex item");
        api_->UiText(uiState_.item.type[0] ? uiState_.item.type : "Unknown type");
        if (uiState_.item.grandparentTitle[0])
            api_->UiText(uiState_.item.grandparentTitle);
        if (uiState_.item.viewOffsetMs > 0)
        {
            char resume[96]{};
            std::snprintf(resume, sizeof(resume), "Resume position: %ld ms",
                          static_cast<long>(uiState_.item.viewOffsetMs));
            api_->UiText(resume);
        }
        if (api_->UiButton("Play selected Plex item"))
            SendUiCommand(wxl_video_shared::UiCommand::Play);
        if (uiState_.partyRoomCode[0])
        {
            api_->UiSameLine();
            if (api_->UiButton("Add selected item to party queue"))
                SendUiCommand(wxl_video_shared::UiCommand::AddCurrentToQueue);
        }

        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.streamCount), wxl_video_shared::kUiMaxStreams));
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& stream = uiState_.streams[index];
            if (stream.streamType == 2)
            {
                char label[192]{};
                std::snprintf(label, sizeof(label), "%u. Audio: %s%s%s", index + 1, stream.title,
                              stream.title[0] && stream.language[0] ? " (" : "",
                              stream.title[0] && stream.language[0] ? stream.language : "");
                if (stream.title[0] && stream.language[0])
                    std::strncat(label, ")", sizeof(label) - std::strlen(label) - 1);
                if (api_->UiButton(label))
                    SendUiCommand(wxl_video_shared::UiCommand::SelectAudio,
                                  static_cast<LONG>(index));
            }
            else if (stream.streamType == 3)
            {
                char label[192]{};
                std::snprintf(label, sizeof(label), "%u. Subtitle: %s%s%s", index + 1,
                              stream.title,
                              stream.title[0] && stream.language[0] ? " (" : "",
                              stream.title[0] && stream.language[0] ? stream.language : "");
                if (stream.title[0] && stream.language[0])
                    std::strncat(label, ")", sizeof(label) - std::strlen(label) - 1);
                if (api_->UiButton(label))
                    SendUiCommand(wxl_video_shared::UiCommand::SelectSubtitle,
                                  static_cast<LONG>(index));
            }
        }
    }

    void VideoSurface::DrawPlexPlayer()
    {
        api_->UiText("Video continues on the placed world-space screen.");
        if (uiState_.playbackDurationMs > 0)
        {
            const float duration = uiState_.playbackDurationMs / 1000.0f;
            scrubPositionSeconds_ = std::clamp(
                scrubPositionSeconds_, 0.0f, duration);
            if (GetTickCount() - lastScrubAt_ > 250)
                scrubPositionSeconds_ = std::clamp(
                    uiState_.playbackPositionMs / 1000.0f, 0.0f, duration);
            if (api_->UiSliderFloat("Timeline", &scrubPositionSeconds_, 0.0f, duration))
            {
                lastScrubAt_ = GetTickCount();
                const double milliseconds = scrubPositionSeconds_ * 1000.0;
                const LONG positionMs = static_cast<LONG>(std::min(
                    milliseconds, static_cast<double>(0x7FFFFFFF)));
                SendUiCommand(wxl_video_shared::UiCommand::Seek, -1, positionMs);
            }
            char timeline[64]{};
            char current[32]{};
            char total[32]{};
            FormatTime(current, sizeof(current), uiState_.playbackPositionMs);
            FormatTime(total, sizeof(total), uiState_.playbackDurationMs);
            std::snprintf(timeline, sizeof(timeline), "%s / %s", current, total);
            api_->UiText(timeline);
        }
        else
        {
            api_->UiText("Timeline unavailable for this item.");
        }
        if (api_->UiButton("Play Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Play);
        api_->UiSameLine();
        if (api_->UiButton("Pause Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Pause);
        api_->UiSameLine();
        if (api_->UiButton("Stop Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Stop);

        int speed = static_cast<int>(uiState_.playbackSpeedPermille);
        if (api_->UiSliderInt("Playback speed (x1000)", &speed, 500, 2000))
            SendUiCommand(wxl_video_shared::UiCommand::SetPlaybackSpeed, -1, speed);

        const uint32_t markerCount = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.markerCount), wxl_video_shared::kUiMaxMarkers));
        for (uint32_t index = 0; index < markerCount; ++index)
        {
            const auto& marker = uiState_.markers[index];
            if (uiState_.playbackPositionMs >= static_cast<uint32_t>(std::max<LONG>(0, marker.startMs)) &&
                uiState_.playbackPositionMs < static_cast<uint32_t>(std::max<LONG>(0, marker.endMs)))
            {
                char label[96]{};
                std::snprintf(label, sizeof(label), "Skip %s",
                              marker.type[0] ? marker.type : "marker");
                if (api_->UiButton(label))
                    SendUiCommand(wxl_video_shared::UiCommand::SkipMarker,
                                  static_cast<LONG>(index));
            }
        }

        if (uiState_.nextEpisodeAvailable)
        {
            char next[192]{};
            std::snprintf(next, sizeof(next), "Next: %s (%u seconds)",
                          uiState_.nextEpisode.title[0] ? uiState_.nextEpisode.title : "episode",
                          (uiState_.autoplayRemainingMs + 999) / 1000);
            api_->UiText(next);
            if (api_->UiButton("Play next episode now"))
                SendUiCommand(wxl_video_shared::UiCommand::PlayNextEpisode);
            api_->UiSameLine();
            if (api_->UiButton("Cancel autoplay"))
                SendUiCommand(wxl_video_shared::UiCommand::CancelAutoplay);
        }

        if (uiState_.partyRoomCode[0])
        {
            if (api_->UiButton("Watch Together"))
                SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                              static_cast<LONG>(wxl_video_shared::UiView::Party));
            api_->UiSameLine();
            if (api_->UiButton("Party queue"))
                SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                              static_cast<LONG>(wxl_video_shared::UiView::Queue));
        }
        if (api_->UiButton("Back to Plex item"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Details));
    }

    void VideoSurface::DrawPlexParty()
    {
        if (api_->UiButton("Back to Plex"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Home));
        api_->UiSameLine();
        if (api_->UiButton("Party settings"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Settings));

        if (!uiState_.partyRoomCode[0])
        {
            api_->UiText("Create a room or join another AzerothPlex viewer.");
            if (api_->UiButton("Create private watch party"))
                SendUiCommand(wxl_video_shared::UiCommand::CreateParty);
            if (uiState_.partyPublicRoomsEnabled)
            {
                api_->UiSameLine();
                if (api_->UiButton("Create public watch party"))
                    SendUiCommand(wxl_video_shared::UiCommand::CreatePublicParty);
            }
            api_->UiInputText("Watch-party code", partyCode_, sizeof(partyCode_));
            if (api_->UiButton("Join watch party"))
                SendUiCommand(wxl_video_shared::UiCommand::JoinParty, -1, 0, partyCode_);

            if (uiState_.partyPublicRoomsEnabled)
            {
                if (api_->UiButton("Refresh public rooms"))
                    SendUiCommand(wxl_video_shared::UiCommand::RefreshPublicRooms);

                const uint32_t roomCount = static_cast<uint32_t>(std::min(
                    static_cast<size_t>(uiState_.publicRoomCount), wxl_video_shared::kUiMaxPublicRooms));
                for (uint32_t index = 0; index < roomCount; ++index)
                {
                    const auto& room = uiState_.publicRooms[index];
                    char label[256]{};
                    std::snprintf(label, sizeof(label), "Join %s - %s (%ld viewer%s)",
                                  room.code, room.title[0] ? room.title : "Watch Party",
                                  static_cast<long>(room.memberCount), room.memberCount == 1 ? "" : "s");
                    if (api_->UiButton(label))
                        SendUiCommand(wxl_video_shared::UiCommand::JoinPublicRoom,
                                      static_cast<LONG>(index));
                }
            }
            return;
        }

        char roomLine[192]{};
        std::snprintf(roomLine, sizeof(roomLine), "Room %s | Host: %s",
                      uiState_.partyRoomCode,
                      uiState_.partyHostName[0] ? uiState_.partyHostName : "unknown");
        api_->UiText(roomLine);
        char diagnostics[256]{};
        std::snprintf(diagnostics, sizeof(diagnostics),
                      "RTT %lld ms | drift %lld ms | correction %s | epoch %llu / seq %llu | reconnects %u",
                      static_cast<long long>(uiState_.partyRelayRttMs),
                      static_cast<long long>(uiState_.partyDriftMs),
                      uiState_.partyCorrection[0] ? uiState_.partyCorrection : "None",
                      static_cast<unsigned long long>(uiState_.partyRoomEpoch),
                      static_cast<unsigned long long>(uiState_.partySequence),
                      uiState_.partyReconnectCount);
        api_->UiText(diagnostics);
        if (uiState_.partyMediaMatch[0]) api_->UiText(uiState_.partyMediaMatch);
        if (uiState_.partyError[0]) api_->UiText(uiState_.partyError);

        if (api_->UiButton("Copy party invite"))
        {
            std::string invite = "Join my AzerothPlex Watch Party: ";
            invite += uiState_.partyRoomCode;
            status_ = CopyClipboardText(invite)
                ? "Watch-party invite copied. Paste it into party, raid, guild, or whisper chat."
                : "Could not copy the watch-party invite.";
        }
        api_->UiSameLine();
        if (api_->UiButton("Party queue"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Queue));
        api_->UiSameLine();
        if (api_->UiButton("Leave party"))
            SendUiCommand(wxl_video_shared::UiCommand::LeaveParty);

        api_->UiSeparator();
        api_->UiText("Members:");
        const uint32_t memberCount = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.partyMemberCount), wxl_video_shared::kUiMaxPartyMembers));
        for (uint32_t index = 0; index < memberCount; ++index)
        {
            const auto& member = uiState_.partyMembers[index];
            char memberLine[192]{};
            std::snprintf(memberLine, sizeof(memberLine), "%s%s%s%s",
                          member.name[0] ? member.name : "Player",
                          member.role == 1 ? " [Host]" : "",
                          member.ready ? " [Ready]" : "",
                          member.connected ? "" : " [Disconnected]");
            api_->UiText(memberLine);
        }

        api_->UiSeparator();
        api_->UiText("Live broadcast");
        if (uiState_.partyLiveActive)
        {
            char liveLine[192]{};
            std::snprintf(liveLine, sizeof(liveLine), "Live now | %u viewer%s | %u fps%s",
                          uiState_.partyLiveViewerCount,
                          uiState_.partyLiveViewerCount == 1 ? "" : "s",
                          uiState_.partyLiveFpsPermille / 1000,
                          uiState_.partyLiveAudioActive ? " | audio" : "");
            api_->UiText(liveLine);
            if (uiState_.partyLiveWatching)
            {
                if (api_->UiButton("Leave live broadcast"))
                    SendUiCommand(wxl_video_shared::UiCommand::StopWatchingLive);
            }
            else if (api_->UiButton("Watch live"))
            {
                SendUiCommand(wxl_video_shared::UiCommand::WatchLive);
            }
            if (uiState_.partyLiveBroadcasting)
            {
                api_->UiSameLine();
                if (api_->UiButton("Stop broadcasting"))
                    SendUiCommand(wxl_video_shared::UiCommand::StopLive);
            }
            if (uiState_.partyLiveWatching &&
                (uiState_.partyLiveFramesReceived || uiState_.partyLiveAudioFramesReceived))
            {
                char received[160]{};
                std::snprintf(received, sizeof(received), "Received %u video / %u audio frames",
                              uiState_.partyLiveFramesReceived,
                              uiState_.partyLiveAudioFramesReceived);
                api_->UiText(received);
            }
        }
        else if (uiState_.partyRole == 1)
        {
            api_->UiText("Broadcast your player screen to room members. Viewers do not need Plex.");
            if (api_->UiButton("Go live"))
                SendUiCommand(wxl_video_shared::UiCommand::GoLive);
        }

        if (uiState_.partyRole == 1)
        {
            if (api_->UiButton("Start ready check"))
                SendUiCommand(wxl_video_shared::UiCommand::StartReadyCheck);
        }
        if (uiState_.partyReadyCheckActive)
        {
            if (uiState_.partyReadyStartAtMs > 0)
            {
                const int64_t relayNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() +
                    uiState_.partyClockOffsetMs;
                const int64_t remainingMs = std::max<int64_t>(0, uiState_.partyReadyStartAtMs - relayNowMs);
                char countdown[96]{};
                std::snprintf(countdown, sizeof(countdown), "Everyone ready. Starting in %lld...",
                              static_cast<long long>((remainingMs + 999) / 1000));
                api_->UiText(countdown);
            }
            else
            {
                api_->UiText("Ready check: waiting for everyone.");
            }
            if (api_->UiButton("I'm ready"))
                SendUiCommand(wxl_video_shared::UiCommand::SetReadyState, -1, 1);
        }

        if (uiState_.partyRole == 1 && uiState_.controlRequest[0])
        {
            char request[192]{};
            std::snprintf(request, sizeof(request), "Playback control request: %s",
                          uiState_.controlRequest);
            api_->UiText(request);
            if (api_->UiButton("Allow request"))
                SendUiCommand(wxl_video_shared::UiCommand::RespondControlRequest, -1, 1);
            api_->UiSameLine();
            if (api_->UiButton("Ignore request"))
                SendUiCommand(wxl_video_shared::UiCommand::RespondControlRequest, -1, 0);
        }
    }

    void VideoSurface::DrawPlexQueue()
    {
        if (api_->UiButton("Back to watch party"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Party));
        const bool mayAdd = uiState_.partyRole == 1 || uiState_.partyQueueMode == 1;
        if (mayAdd && uiState_.item.ratingKey[0])
        {
            api_->UiSameLine();
            if (api_->UiButton("Add selected Plex item"))
                SendUiCommand(wxl_video_shared::UiCommand::AddCurrentToQueue);
        }

        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.partyQueueCount), wxl_video_shared::kUiMaxPartyQueue));
        if (!count) api_->UiText("The watch-party queue is empty.");
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& item = uiState_.partyQueue[index];
            char line[192]{};
            std::snprintf(line, sizeof(line), "%u. %s (%s)%s", index + 1,
                          item.title[0] ? item.title : "Plex item",
                          item.type[0] ? item.type : "media",
                          item.resolved ? "" : " [needs local match]");
            api_->UiText(line);
            if (api_->UiButton((std::string("Play##q") + std::to_string(index)).c_str()))
                SendUiCommand(wxl_video_shared::UiCommand::PlayQueueItem,
                              static_cast<LONG>(index));
            if (uiState_.partyRole == 1)
            {
                api_->UiSameLine();
                if (index > 0 && api_->UiButton((std::string("Up##q") + std::to_string(index)).c_str()))
                    SendUiCommand(wxl_video_shared::UiCommand::MoveQueueItem,
                                  static_cast<LONG>(index), static_cast<LONG>(index - 1));
                if (index + 1 < count)
                {
                    api_->UiSameLine();
                    if (api_->UiButton((std::string("Down##q") + std::to_string(index)).c_str()))
                        SendUiCommand(wxl_video_shared::UiCommand::MoveQueueItem,
                                      static_cast<LONG>(index), static_cast<LONG>(index + 1));
                }
                api_->UiSameLine();
                if (api_->UiButton((std::string("Remove##q") + std::to_string(index)).c_str()))
                    SendUiCommand(wxl_video_shared::UiCommand::RemoveQueueItem,
                                  static_cast<LONG>(index));
            }
        }
    }

    void VideoSurface::DrawPlexSettings()
    {
        if (api_->UiButton("Back"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(uiState_.partyRoomCode[0]
                              ? wxl_video_shared::UiView::Party : wxl_video_shared::UiView::Home));
        api_->UiText("Watch Together defaults");
        const bool mayEditRoomSettings = !uiState_.partyRoomCode[0] || uiState_.partyRole == 1;
        if (mayEditRoomSettings)
        {
            int mode = static_cast<int>(uiState_.partyPermissionMode);
            if (api_->UiSliderInt("Control mode (0 host, 1 shared, 2 request)", &mode, 0, 2))
                SendUiCommand(wxl_video_shared::UiCommand::SetPartyPermissionMode, -1, mode);
            int queueMode = static_cast<int>(uiState_.partyQueueMode);
            if (api_->UiSliderInt("Queue mode (0 host managed, 1 everyone may add)", &queueMode, 0, 1))
                SendUiCommand(wxl_video_shared::UiCommand::SetPartyQueueMode, -1, queueMode);
            int syncAudio = uiState_.partySyncAudio ? 1 : 0;
            if (api_->UiCheckbox("Synchronize audio selection", &syncAudio))
                SendUiCommand(wxl_video_shared::UiCommand::SetPartySyncAudio, -1, syncAudio);
            int syncSubtitles = uiState_.partySyncSubtitles ? 1 : 0;
            if (api_->UiCheckbox("Synchronize subtitle selection", &syncSubtitles))
                SendUiCommand(wxl_video_shared::UiCommand::SetPartySyncSubtitles, -1, syncSubtitles);
            int disconnectPolicy = static_cast<int>(uiState_.partyDisconnectPolicy);
            if (api_->UiSliderInt("Disconnect policy (0 keep, 1 pause 60s, 2 pause until return, 3 ask)",
                                  &disconnectPolicy, 0, 3))
                SendUiCommand(wxl_video_shared::UiCommand::SetDisconnectPolicy, -1, disconnectPolicy);
        }
        else
        {
            api_->UiText("Only the watch-party host can change active room settings.");
        }
    }

    void VideoSurface::DrawMiniPlayer()
    {
        if (!RefreshUiState() || !uiState_.playingItem.title[0]) return;
        api_->UiText(uiState_.playingItem.title);
        if (uiState_.playbackDurationMs)
        {
            char current[32]{}, total[32]{}, line[80]{};
            FormatTime(current, sizeof(current), uiState_.playbackPositionMs);
            FormatTime(total, sizeof(total), uiState_.playbackDurationMs);
            std::snprintf(line, sizeof(line), "%s / %s", current, total);
            api_->UiText(line);
        }
        if (api_->UiButton("-10s##mini") && uiState_.playbackDurationMs)
        {
            const LONG target = static_cast<LONG>(uiState_.playbackPositionMs > 10000
                ? uiState_.playbackPositionMs - 10000 : 0);
            SendUiCommand(wxl_video_shared::UiCommand::Seek, -1, target);
        }
        api_->UiSameLine();
        if (uiState_.playbackPaused)
        {
            if (api_->UiButton("Play##mini")) SendUiCommand(wxl_video_shared::UiCommand::Play);
        }
        else
        {
            if (api_->UiButton("Pause##mini")) SendUiCommand(wxl_video_shared::UiCommand::Pause);
        }
        api_->UiSameLine();
        if (api_->UiButton("+10s##mini") && uiState_.playbackDurationMs)
        {
            const uint64_t desired = static_cast<uint64_t>(uiState_.playbackPositionMs) + 10000u;
            const LONG target = static_cast<LONG>(std::min<uint64_t>(desired, uiState_.playbackDurationMs));
            SendUiCommand(wxl_video_shared::UiCommand::Seek, -1, target);
        }
        api_->UiSameLine();
        if (api_->UiButton("Stop##mini")) SendUiCommand(wxl_video_shared::UiCommand::Stop);
        api_->UiSameLine();
        if (api_->UiButton("Open full player"))
        {
            plexPanelOpen_ = true;
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Player));
        }
    }

    void VideoSurface::DrawCytubeSection()
    {
        const bool hostUp = FindWindowW(kHostWindowClass, nullptr) && RefreshUiState();
        const auto& cy = uiState_.cytube;
        if (!hostUp || !cy.active)
        {
            api_->UiInputText("CyTube channel (cytu.be/r/...)", cytubeChannel_, sizeof(cytubeChannel_));
            if (api_->UiButton("Join CyTube channel") && cytubeChannel_[0])
            {
                SaveUiPreferences();
                SendUiCommand(wxl_video_shared::UiCommand::JoinCytube, -1, 0, cytubeChannel_);
            }
            return;
        }

        static constexpr const char* kStates[] = {"idle", "connecting", "joined", "reconnecting", "stopped"};
        const char* state = cy.state >= 0 && cy.state < 5 ? kStates[cy.state] : "?";
        char line[320]{};
        std::snprintf(line, sizeof(line), "CyTube: %s (%s)", cy.channel, state);
        api_->UiText(line);
        if (cy.title[0])
        {
            if (cy.live)
                std::snprintf(line, sizeof(line), "%s (live)", cy.title);
            else if (cy.durationMs > 0)
            {
                char current[32]{}, total[32]{};
                FormatTime(current, sizeof(current), static_cast<uint32_t>(std::max<LONG>(0, cy.positionMs)));
                FormatTime(total, sizeof(total), static_cast<uint32_t>(cy.durationMs));
                std::snprintf(line, sizeof(line), "%s  %s / %s", cy.title, current, total);
            }
            else
                std::snprintf(line, sizeof(line), "%s", cy.title);
            api_->UiText(line);
        }
        if (cy.status[0]) api_->UiText(cy.status);
        if (cy.playable && !cy.live && !cy.loading && !cy.paused)
        {
            const LONG drift = cy.driftMs < 0 ? -cy.driftMs : cy.driftMs;
            if (drift <= 500)
                std::snprintf(line, sizeof(line), "In sync with the channel (%.1f s).", drift / 1000.0);
            else
                std::snprintf(line, sizeof(line), "Catching up: %.1f s %s the channel.", drift / 1000.0,
                              cy.driftMs > 0 ? "behind" : "ahead of");
            api_->UiText(line);
        }
        if (cy.needPassword)
        {
            api_->UiInputText("Channel password", cytubePassword_, sizeof(cytubePassword_));
            if (api_->UiButton("Send password") && cytubePassword_[0])
            {
                SendUiCommand(wxl_video_shared::UiCommand::CytubePassword, -1, 0, cytubePassword_);
                SecureZeroMemory(cytubePassword_, sizeof(cytubePassword_));
            }
        }
        if (api_->UiButton("Leave CyTube channel"))
            SendUiCommand(wxl_video_shared::UiCommand::LeaveCytube);
    }

    void VideoSurface::DrawPlexPanel()
    {
        if (!FindWindowW(kHostWindowClass, nullptr))
        {
            api_->UiText("Plex playback worker is not running.");
            if (api_->UiButton("Start Plex playback worker")) OpenHost();
            return;
        }
        if (!RefreshUiState())
        {
            api_->UiText("Plex player is starting.");
            return;
        }
        if (uiState_.status[0]) api_->UiText(uiState_.status);
        if (uiState_.error[0]) api_->UiText(uiState_.error);

        switch (uiState_.view)
        {
        case wxl_video_shared::UiView::Login: DrawPlexLogin(); break;
        case wxl_video_shared::UiView::Servers: DrawPlexServers(); break;
        case wxl_video_shared::UiView::Home: DrawPlexHome(); break;
        case wxl_video_shared::UiView::Library: DrawPlexLibrary(); break;
        case wxl_video_shared::UiView::Search: DrawPlexSearch(); break;
        case wxl_video_shared::UiView::Details: DrawPlexDetails(); break;
        case wxl_video_shared::UiView::Player: DrawPlexPlayer(); break;
        case wxl_video_shared::UiView::Party: DrawPlexParty(); break;
        case wxl_video_shared::UiView::Queue: DrawPlexQueue(); break;
        case wxl_video_shared::UiView::Settings: DrawPlexSettings(); break;
        default: api_->UiText("Plex player returned an invalid view."); break;
        }
        if (uiState_.view == wxl_video_shared::UiView::Player &&
            !uiState_.item.ratingKey[0] && api_->UiButton("Back to Plex sign-in"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Login));
    }

    void VideoSurface::DrawPanel()
    {
        if (!api_) return;
        api_->UiText("Native Plex player with a physical world-space screen.");
        if (api_->UiCheckbox("Show mini player when Plex UI is hidden", &miniPlayerEnabled_))
            SaveUiPreferences();
        if (plexPanelOpen_)
        {
            if (api_->UiButton("Hide Plex player UI"))
                HideHost();
            else
            {
                DrawPlexPanel();
                api_->UiSeparator();
            }
        }
        else if (api_->UiButton("Open Plex player inside WoW"))
        {
            if (OpenHost()) plexPanelOpen_ = true;
        }

        DrawCytubeSection();
        api_->UiSeparator();

        if (!plexPanelOpen_)
        {
            if (miniPlayerEnabled_) DrawMiniPlayer();
            if (!uiState_.playingItem.title[0] && !uiState_.cytube.active)
            {
                if (api_->UiButton("Play"))
                    SendCommand(static_cast<LONG>(wxl_video_shared::Command::Play));
                if (api_->UiButton("Pause"))
                    SendCommand(static_cast<LONG>(wxl_video_shared::Command::Pause));
                if (api_->UiButton("Stop"))
                    SendCommand(static_cast<LONG>(wxl_video_shared::Command::Stop));
            }
        }
        api_->UiSeparator();

        if (api_->UiSliderInt("Master volume", &masterVolume_, 0, 100))
            SendVolume(masterVolume_);
        api_->UiCheckbox("Distance-based audio", &spatialAudio_);
        if (spatialAudio_)
        {
            api_->UiSliderFloat("Full volume distance (yards)", &fullVolumeDistance_, 1.0f, 30.0f);
            api_->UiSliderFloat("Silent distance (yards)", &silentDistance_, 10.0f, 120.0f);
            if (silentDistance_ < fullVolumeDistance_ + 1.0f)
                silentDistance_ = fullVolumeDistance_ + 1.0f;
        }
        char volumeStatus[96]{};
        std::snprintf(volumeStatus, sizeof(volumeStatus), "Current screen audio: %d%%", effectiveVolume_);
        api_->UiText(volumeStatus);
        if (api_->UiButton("Close background Plex player")) CloseHost();
        api_->UiSeparator();

        api_->UiText("Cinema presets:");
        for (size_t index = 0; index < cinemaPresets_.size(); ++index)
        {
            char label[128]{};
            std::snprintf(label, sizeof(label), "%s%s##preset%zu",
                          index == static_cast<size_t>(std::max(0, selectedCinemaPreset_)) ? "* " : "",
                          cinemaPresets_[index].name.c_str(), index);
            if (api_->UiButton(label)) ApplyCinemaPreset(index);
            if ((index + 1) % 3 != 0 && index + 1 < cinemaPresets_.size()) api_->UiSameLine();
        }
        api_->UiInputText("Preset name", cinemaPresetName_, sizeof(cinemaPresetName_));
        if (api_->UiButton("Save current layout as preset")) SaveCurrentCinemaPreset();
        if (selectedCinemaPreset_ >= 0 &&
            static_cast<size_t>(selectedCinemaPreset_) < cinemaPresets_.size() &&
            !cinemaPresets_[static_cast<size_t>(selectedCinemaPreset_)].builtIn)
        {
            api_->UiSameLine();
            if (api_->UiButton("Rename selected preset"))
            {
                cinemaPresets_[static_cast<size_t>(selectedCinemaPreset_)].name =
                    cinemaPresetName_[0] ? cinemaPresetName_ : "Custom Cinema";
                SaveCinemaPresets();
            }
            api_->UiSameLine();
            if (api_->UiButton("Delete selected preset"))
            {
                cinemaPresets_.erase(cinemaPresets_.begin() + selectedCinemaPreset_);
                selectedCinemaPreset_ = 0;
                SaveCinemaPresets();
            }
        }
        if (uiState_.partyRoomCode[0] && uiState_.partyRole == 1)
        {
            if (api_->UiButton("Share current cinema layout with party")) ShareCurrentCinema();
        }

        bool cinemaVisualChanged = api_->UiSliderInt(
            "Aspect ratio (0 original, 1 16:9, 2 16:10, 3 4:3, 4 21:9)",
            &aspectRatioMode_, 0, 4) != 0;
        cinemaVisualChanged = api_->UiSliderInt(
            "Video fit (0 fit, 1 fill, 2 stretch, 3 crop)", &fitMode_, 0, 3) != 0 || cinemaVisualChanged;
        cinemaVisualChanged = api_->UiSliderInt(
            "Screen frame (0 none, 1 TV, 2 Goblin, 3 Gnome, 4 Orc, 5 Blood Elf, 6 Titan, 7 Tavern, 8 Outdoor)",
            &frameStyle_, 0, 8) != 0 || cinemaVisualChanged;
        cinemaVisualChanged = api_->UiCheckbox("Ambient screen glow", &ambientLighting_) != 0 || cinemaVisualChanged;
        cinemaVisualChanged = api_->UiCheckbox("Stonewall TV: neon sign with a glittery rainbow", &stonewall_) != 0 ||
                              cinemaVisualChanged;
        if (stonewall_)
            cinemaVisualChanged = api_->UiSliderFloat("Stonewall sign width (x screen)", &signSize_, 0.4f, 1.6f) != 0 ||
                                  cinemaVisualChanged;
        if (cinemaVisualChanged && placed_) SavePlacement();
        if (api_->UiCheckbox("Cinema mode (compact viewing controls)", &cinemaMode_))
        {
            if (cinemaMode_) outline_ = 0;
        }
        if (cinemaMode_)
            api_->UiText("WarcraftXL does not expose safe action-bar/minimap hiding here; cinema mode keeps AzerothPlex controls compact without mutating WoW UI state.");
        api_->UiSeparator();

        if (api_->UiCheckbox("Pin Plex screen to display", &pinned_))
        {
            if (placed_) SavePlacement();
            else status_ = "Pinned Plex screen enabled; place it later to save the setting.";
        }
        if (pinned_)
        {
            api_->UiText("Pinned Plex screen stays fixed while your character moves.");
            api_->UiText("Use these controls while the WoW overlay is open; drag when it is closed.");
            bool pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned size", &pinnedWidth_, 0.15f, 0.80f) != 0;
            pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned X (left to right)", &pinnedLeft_, 0.0f,
                std::max(0.0f, 1.0f - pinnedWidth_)) != 0 || pinnedLayoutChanged;
            pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned Y (top to bottom)", &pinnedTop_, 0.0f,
                std::max(0.0f, 1.0f - pinnedWidth_ * 9.0f / 16.0f)) != 0 || pinnedLayoutChanged;
            wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
            if (pinnedLayoutChanged) SavePlacement();
        }

        if (api_->UiSliderFloat("Screen width (yards)", &width_, 2.0f, 30.0f) && placed_)
            SavePlacement();
        api_->UiSliderFloat("Place distance (yards)", &placeDistance_, 4.0f, 30.0f);
        api_->UiSliderFloat("Bottom height (yards)", &bottomOffset_, -3.0f, 10.0f);
        if (api_->UiButton("Place / move screen in front of me")) PlaceInFront();

        if (placed_)
        {
            if (api_->UiCheckbox("Show physical screen", &visible_)) SavePlacement();
            if (api_->UiCheckbox("Characters and world appear in front", &depthTest_)) SavePlacement();
            if (api_->UiSliderFloat("Depth offset (more negative brings screen forward)",
                                    &depthBias_, -0.030f, 0.005f))
                SavePlacement();
            api_->UiCheckbox("Show yellow placement outline", &outline_);
            if (api_->UiButton("Rotate screen left 5 degrees")) Rotate(5.0f);
            if (api_->UiButton("Rotate screen right 5 degrees")) Rotate(-5.0f);
            if (api_->UiButton("Raise screen 0.5 yard")) { center_[2] += 0.5f; SavePlacement(); }
            if (api_->UiButton("Lower screen 0.5 yard")) { center_[2] -= 0.5f; SavePlacement(); }

            char placement[192]{};
            std::snprintf(placement, sizeof(placement),
                          "Saved screen: map %d at %.2f, %.2f, %.2f (%.2f x %.2f yards)",
                          mapId_, center_[0], center_[1], center_[2], width_, width_ / ScreenAspectRatio());
            api_->UiText(placement);
        }
        else
        {
            api_->UiText("No physical screen has been placed yet.");
        }
        if (!cinemaMode_)
        {
            char diagnostics[256]{};
            std::snprintf(diagnostics, sizeof(diagnostics),
                          "World draw: scenes %llu | attempts %llu | success %llu | hr 0x%08lX | frame %ld | helper %s",
                          static_cast<unsigned long long>(worldSceneCalls_),
                          static_cast<unsigned long long>(drawAttempts_),
                          static_cast<unsigned long long>(drawSuccesses_),
                          static_cast<unsigned long>(lastDrawResult_), lastFrameSequence_,
                          FindWindowW(kHostWindowClass, nullptr) ? "open" : "closed");
            api_->UiText(diagnostics);
            char projection[192]{};
            if (projectedCenterValid_)
                std::snprintf(projection, sizeof(projection),
                              "Projected center NDC: %.3f, %.3f, %.3f | clip W %.3f (visible XY is -1 to +1)",
                              projectedCenter_[0], projectedCenter_[1], projectedCenter_[2], projectedClipW_);
            else
                std::snprintf(projection, sizeof(projection),
                              "Projected center unavailable | clip W %.3f", projectedClipW_);
            api_->UiText(projection);
            if (!texture_)
                api_->UiText("No video frame yet: a bright four-color test card should still be visible in the world.");
        }
        api_->UiText(status_.c_str());
    }
}
