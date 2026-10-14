#include "../CytubeProtocol.hpp"
#include "../SyncController.hpp"

#include <cstdio>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); return __LINE__; } } while (false)

int main()
{
    using namespace wxl_cytube;
    std::string error;

    // Channel links.
    ChannelRef ref;
    CHECK(ParseChannelRef("https://cytu.be/r/MovieNight", ref, error) && ref.channel == "movienight" && ref.origin == "https://cytu.be");
    CHECK(ParseChannelRef("  cytu.be/r/stonewall_tv/ ", ref, error) && ref.channel == "stonewall_tv");
    CHECK(ParseChannelRef("https://www.cytu.be/r/abc?x=1#y", ref, error) && ref.channel == "abc" && ref.origin == "https://cytu.be");
    CHECK(ParseChannelRef("stonewall-tv", ref, error) && ref.channel == "stonewall-tv" && ref.origin == "https://cytu.be");
    CHECK(ParseChannelRef("https://example.org/r/room1", ref, error) && ref.origin == "https://example.org" && ref.channel == "room1");
    CHECK(!ParseChannelRef("https://cytu.be/", ref, error));
    CHECK(!ParseChannelRef("https://reddit.com/u/x", ref, error));
    CHECK(!ParseChannelRef("bad name!", ref, error));
    CHECK(!ParseChannelRef("", ref, error));
    CHECK(!ParseChannelRef("this_channel_name_is_far_too_long_x", ref, error));
    CHECK(SocketConfigPath(ChannelRef{"https://cytu.be", "abc"}) == "/socketconfig/abc.json");

    // Socket servers (cytu.be's own answer for /socketconfig/test.json, 2026-10-14).
    std::string url;
    CHECK(PickSocketServer(R"({"servers":[{"url":"https://zip.cytu.be:2087","secure":true},{"url":"http://zip.cytu.be:2086","secure":false}]})",
                           url, error) && url == "https://zip.cytu.be:2087");
    CHECK(PickSocketServer(R"({"servers":[{"url":"https://v6.x:1","secure":true,"ipv6":true},{"url":"https://v4.x:2","secure":true}]})",
                           url, error) && url == "https://v4.x:2");
    CHECK(PickSocketServer(R"({"servers":[{"url":"https://v6.x:1","secure":true,"ipv6":true}]})", url, error) && url == "https://v6.x:1");
    CHECK(!PickSocketServer(R"({"servers":[{"url":"http://a:1","secure":false}]})", url, error));
    CHECK(!PickSocketServer("not json", url, error));
    CHECK(WebSocketUrl("https://zip.cytu.be:2087") == "wss://zip.cytu.be:2087/socket.io/?EIO=4&transport=websocket");

    // engine.io / socket.io framing.
    Packet packet;
    CHECK(ParsePacket(R"(0{"sid":"abc","upgrades":[],"pingInterval":25000,"pingTimeout":120000,"maxPayload":1048576})", packet) &&
          packet.engine == '0' && packet.data.find("pingInterval") != std::string::npos);
    CHECK(ParsePacket("2", packet) && packet.engine == '2');
    CHECK(ParsePacket(R"(40{"sid":"xyz"})", packet) && packet.engine == '4' && packet.socket == '0');
    CHECK(ParsePacket(R"(44{"message":"Rate limit exceeded"})", packet) && packet.socket == '4' && packet.data.find("Rate limit") != std::string::npos);
    CHECK(ParsePacket(R"(42["mediaUpdate",{"currentTime":12.5,"paused":false}])", packet) && packet.event == "mediaUpdate");
    CHECK(ParsePacket(R"(4217["setCurrent",42])", packet) && packet.event == "setCurrent" && packet.data == "42");
    CHECK(ParsePacket(R"(42["usercount"])", packet) && packet.event == "usercount" && packet.data.empty());
    CHECK(!ParsePacket(R"(42{"broken")", packet));
    CHECK(EventPacket("playerReady", "") == R"(42["playerReady"])");
    CHECK(JoinChannelPacket({"https://cytu.be", "abc"}) == R"(42["joinChannel",{"name":"abc"}])");
    CHECK(JoinChannelPacket({"https://cytu.be", "abc"}, "pw1") == R"(42["joinChannel",{"name":"abc","pw":"pw1"}])");

    // changeMedia / mediaUpdate.
    Media media;
    CHECK(ParseChangeMedia(R"({"id":"jNQXAC9IVRw","title":"Me at the zoo","seconds":19,"duration":"00:19","type":"yt",
                              "meta":{"restricted":false},"currentTime":-3,"paused":true})", media, error));
    CHECK(media.type == "yt" && media.seconds == 19.0 && media.currentTime == -3.0 && media.paused);
    double time = 0.0;
    bool paused = true;
    CHECK(ParseMediaUpdate(R"({"currentTime":7.25,"paused":false})", time, paused) && time == 7.25 && !paused);
    CHECK(!ParseMediaUpdate(R"({"paused":false})", time, paused));
    CHECK(!ParseChangeMedia(R"({"title":"no id"})", media, error));

    // What mpv opens.
    const auto resolve = [&](const char* json) {
        Media m;
        std::string e;
        ParseChangeMedia(json, m, e);
        return ResolvePlayable(m);
    };
    Playable p = resolve(R"({"id":"jNQXAC9IVRw","type":"yt","seconds":19})");
    CHECK(p.kind == Playable::Kind::Ytdl && p.url == "https://www.youtube.com/watch?v=jNQXAC9IVRw" && !p.live);
    p = resolve(R"({"id":"x7tgad0","type":"dm","seconds":60})");
    CHECK(p.kind == Playable::Kind::Ytdl && p.url == "ytdl://https://www.dailymotion.com/video/x7tgad0");
    p = resolve(R"({"id":"https://files.example/movie.mp4","type":"fi","seconds":5400})");
    CHECK(p.kind == Playable::Kind::Direct && p.url == "https://files.example/movie.mp4");
    p = resolve(R"({"id":"https://live.example/a.m3u8","type":"hl","seconds":0})");
    CHECK(p.kind == Playable::Kind::Direct && p.live);
    p = resolve(R"({"id":"somestreamer","type":"tw","seconds":0})");
    CHECK(p.kind == Playable::Kind::Ytdl && p.url == "https://www.twitch.tv/somestreamer" && p.live);
    p = resolve(R"({"id":"v123456","type":"tv","seconds":3600})");
    CHECK(p.url == "https://www.twitch.tv/videos/123456");
    p = resolve(R"({"id":"framatube.org;abc-def","type":"pt","seconds":100})");
    CHECK(p.url == "ytdl://https://framatube.org/videos/watch/abc-def");
    p = resolve(R"({"id":"https://m.example/manifest.json","type":"cm","seconds":120,"meta":{"direct":{
                   "1080":[{"link":"https://m.example/1080.mp4","contentType":"video/mp4","quality":1080}],
                   "480":[{"link":"https://m.example/480.mp4","contentType":"video/mp4","quality":480}],
                   "720":[{"link":"https://m.example/720.mp4","contentType":"video/mp4","quality":720}]}}})");
    CHECK(p.kind == Playable::Kind::Direct && p.url == "https://m.example/480.mp4");
    p = resolve(R"({"id":"https://m.example/manifest.json","type":"cm","seconds":120,"meta":{"direct":{
                   "1080":[{"link":"https://m.example/1080.mp4"}],"720":[{"link":"https://m.example/720.mp4"}]}}})");
    CHECK(p.kind == Playable::Kind::Direct && p.url == "https://m.example/720.mp4");
    p = resolve(R"({"id":"https://embed.example/x","type":"cu","seconds":0})");
    CHECK(p.kind == Playable::Kind::Unsupported && !p.reason.empty());
    p = resolve(R"x({"id":"javascript:alert(1)","type":"fi","seconds":10})x");
    CHECK(p.kind == Playable::Kind::Unsupported);

    // The channel's clock into the Watch Together sync: lead-in, drift, a hard seek past 2.5 s.
    ChannelRef channel{"https://cytu.be", "movienight"};
    Media movie;
    ParseChangeMedia(R"({"id":"https://f.example/m.mp4","type":"fi","title":"M","seconds":5400,"currentTime":-3,"paused":true})", movie, error);
    auto leadIn = ToPlaybackState(channel, movie, -3.0, false, 1000, 1, 1);
    CHECK(leadIn.paused && !leadIn.playing && leadIn.positionMs == 0 && leadIn.durationMs == 5400000);
    CHECK(leadIn.media.serverIdentifier == "cytube:movienight" && leadIn.media.ratingKey == "fi:https://f.example/m.mp4");

    wxl_party::SyncController sync;
    sync.SetThresholds(500, 2500);
    const auto local = IdentityFor(channel, movie);
    auto state = ToPlaybackState(channel, movie, 100.0, false, 10000, 1, 2);
    auto decision = sync.EvaluateRemoteState(state, 10000, 99700, false, local);   // 0.3 s behind: leave it
    CHECK(decision.accepted && !decision.shouldSeek && decision.correction == wxl_party::SyncCorrection::None);
    state = ToPlaybackState(channel, movie, 105.0, false, 15000, 1, 3);
    decision = sync.EvaluateRemoteState(state, 15000, 103800, false, local);        // 1.2 s behind: nudge faster
    CHECK(decision.accepted && !decision.shouldSeek && decision.correction == wxl_party::SyncCorrection::SpeedUp);
    state = ToPlaybackState(channel, movie, 110.0, false, 20000, 1, 4);
    decision = sync.EvaluateRemoteState(state, 21000, 100000, false, local);        // 11 s behind (1 s since sent): seek
    CHECK(decision.accepted && decision.shouldSeek && decision.targetPositionMs == 111000);
    state = ToPlaybackState(channel, movie, 111.0, true, 22000, 1, 5);
    decision = sync.EvaluateRemoteState(state, 22000, 111000, false, local);        // the channel paused
    CHECK(decision.accepted && decision.shouldPause);
    CHECK(!sync.EvaluateRemoteState(state, 22000, 111000, true, local).accepted);   // not newer: ignored

    std::printf("cytube_tests: all passed\n");
    return 0;
}
