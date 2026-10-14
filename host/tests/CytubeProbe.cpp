// Joins a CyTube channel as a passive viewer and prints what arrives (manual run, not a ctest).
// Usage: cytube_probe <channel link or name> [seconds, default 60]
#include "../CytubeClient.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv)
{
    using namespace wxl_cytube;
    if (argc < 2) { std::printf("usage: cytube_probe <channel> [seconds]\n"); return 1; }
    ChannelRef channel;
    std::string error;
    if (!ParseChannelRef(argv[1], channel, error)) { std::printf("%s\n", error.c_str()); return 2; }
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 60;
    CytubeClient client;
    client.Join(channel);
    const int64_t start = CytubeClient::NowMs();
    int changes = 0, updates = 0;
    while (CytubeClient::NowMs() - start < seconds * 1000LL)
    {
        ClientEvent event;
        while (client.PopEvent(event))
        {
            const double at = (event.receivedAtMs - start) / 1000.0;
            switch (event.kind)
            {
            case ClientEvent::Kind::State:
                std::printf("%7.1f  state: %s %s\n", at, ToString(event.state), event.text.c_str());
                break;
            case ClientEvent::Kind::ChangeMedia:
            {
                ++changes;
                Media media;
                std::string e;
                if (ParseChangeMedia(event.text, media, e))
                {
                    const Playable playable = ResolvePlayable(media);
                    std::printf("%7.1f  changeMedia: [%s] %s (%.0f s) at %.1f%s -> %s %s\n", at, media.type.c_str(),
                                media.title.c_str(), media.seconds, media.currentTime, media.paused ? " paused" : "",
                                playable.kind == Playable::Kind::Unsupported ? "unsupported:" : playable.kind == Playable::Kind::Ytdl ? "yt-dlp" : "direct",
                                playable.kind == Playable::Kind::Unsupported ? playable.reason.c_str() : playable.url.c_str());
                }
                else std::printf("%7.1f  changeMedia (unreadable: %s)\n", at, e.c_str());
                break;
            }
            case ClientEvent::Kind::MediaUpdate:
            {
                ++updates;
                double time = 0.0;
                bool paused = false;
                if (ParseMediaUpdate(event.text, time, paused))
                    std::printf("%7.1f  mediaUpdate: %.2f%s\n", at, time, paused ? " paused" : "");
                break;
            }
            case ClientEvent::Kind::NeedPassword: std::printf("%7.1f  needs a password (wrong: %s)\n", at, event.text.c_str()); break;
            case ClientEvent::Kind::PasswordAccepted: std::printf("%7.1f  password accepted\n", at); break;
            case ClientEvent::Kind::Message: std::printf("%7.1f  message: %s\n", at, event.text.c_str()); break;
            case ClientEvent::Kind::Kicked: std::printf("%7.1f  kicked: %s\n", at, event.text.c_str()); break;
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    client.Leave();
    std::printf("done: %d changeMedia, %d mediaUpdate, final state %s\n", changes, updates, ToString(client.State()));
    return 0;
}
