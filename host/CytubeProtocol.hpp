#pragma once

// CyTube (https://cytu.be, github.com/calzoneman/sync) for a passive viewer, as pure functions (no I/O): the channel a
// link names, the socket server its /socketconfig/<channel>.json picks, engine.io v4 / socket.io framing, the
// changeMedia and mediaUpdate events, and what mpv opens for each media type.

#include "PartyTypes.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace wxl_cytube
{
    struct ChannelRef
    {
        std::string origin = "https://cytu.be"; // the site (its socketconfig lives here)
        std::string channel;                    // lowercase, [A-Za-z0-9_-]{1,30}
    };

    // "https://cytu.be/r/<channel>", "cytu.be/r/<channel>" or a bare "<channel>".
    bool ParseChannelRef(std::string_view text, ChannelRef& out, std::string& error);
    std::string SocketConfigPath(const ChannelRef& ref); // "/socketconfig/<channel>.json"

    // The server to connect to, from socketconfig's {"servers":[{"url","secure","ipv6"?}...]}: the first secure one
    // that isn't IPv6-only (as CyTube's own client does), else the first secure one.
    bool PickSocketServer(std::string_view json, std::string& url, std::string& error);
    // wss://host:port/socket.io/?EIO=4&transport=websocket, from a server's https://host:port.
    std::string WebSocketUrl(const std::string& serverUrl);

    // An engine.io packet: type '0' open, '2' ping, '3' pong, '4' message; for messages, the socket.io type: '0'
    // connect, '1' disconnect, '2' event, '4' connect error. For events, `event` and the first argument (JSON text).
    struct Packet
    {
        char engine = 0;
        char socket = 0;
        std::string event;
        std::string data; // the first argument of an event, or the payload of open / connect / connect error
    };
    bool ParsePacket(std::string_view text, Packet& out);
    std::string EventPacket(std::string_view event, std::string_view jsonArgument); // 42["event",arg]
    std::string JoinChannelPacket(const ChannelRef& ref, std::string_view password = {});

    struct Media
    {
        std::string id;
        std::string title;
        std::string type;      // yt, vi, dm, fi, cm, hl, tw, tv, tc, gd, sc, sb, pt, bc, bn, od, nv, li, rt, cu, im, us
        double seconds = 0.0;  // length (0: live or unknown)
        double currentTime = 0.0;
        bool paused = false;
        std::string metaJson;  // its "meta" object, as JSON text
    };
    bool ParseChangeMedia(std::string_view json, Media& out, std::string& error);
    bool ParseMediaUpdate(std::string_view json, double& currentTime, bool& paused);

    struct Playable
    {
        enum class Kind { Direct, Ytdl, Unsupported };
        Kind kind = Kind::Unsupported;
        std::string url;
        bool live = false;
        std::string reason; // why it's unsupported
    };
    // What mpv opens for the media (Ytdl: through mpv's yt-dlp hook). maxHeight: the custom manifest's quality pick.
    Playable ResolvePlayable(const Media& media, int maxHeight = 480);

    // The channel's clock as a Watch Together playback state, for wxl_party::SyncController: the media identified as
    // {cytube:<channel>, <type>:<id>}; the lead-in (currentTime < 0) counts as paused at 0.
    wxl_party::MediaIdentity IdentityFor(const ChannelRef& ref, const Media& media);
    wxl_party::PartyPlaybackState ToPlaybackState(const ChannelRef& ref, const Media& media, double currentTime, bool paused,
                                                  int64_t receivedAtMs, uint64_t epoch, uint64_t sequence);
}
