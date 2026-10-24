#include "../VideoShared.hpp"

#include <cmath>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    using namespace wxl_video_shared;

    UiBridge bridge{};
    bridge.magic = kUiMagic;
    bridge.version = kUiVersion;
    bridge.structBytes = sizeof(bridge);
    bridge.activeIndex = 1;
    bridge.sequence = 7;
    bridge.slotSequence[1] = 7;
    bridge.slots[1].structBytes = sizeof(UiSnapshot);
    bridge.slots[1].view = UiView::Details;
    std::strcpy(bridge.slots[1].error, "metadata failed");
    std::memset(bridge.slots[1].status, 'x', sizeof(bridge.slots[1].status));
    bridge.slots[1].ytdl.state = UiYtdlState::Found;
    bridge.slots[1].ytdl.source = UiYtdlSource::Bundled;
    std::memset(bridge.slots[1].ytdl.version, '9', sizeof(bridge.slots[1].ytdl.version));
    std::memset(bridge.slots[1].ytdl.file, 'f', sizeof(bridge.slots[1].ytdl.file));
    std::memset(bridge.slots[1].ytdl.message, 'm', sizeof(bridge.slots[1].ytdl.message));

    UiSnapshot snapshot{};
    CHECK(ReadUiSnapshot(&bridge, snapshot));
    CHECK(snapshot.view == UiView::Details);
    CHECK(std::strcmp(snapshot.error, "metadata failed") == 0);
    CHECK(snapshot.status[kUiStatusBytes - 1] == '\0');
    CHECK(snapshot.ytdl.state == UiYtdlState::Found && snapshot.ytdl.source == UiYtdlSource::Bundled);
    CHECK(snapshot.ytdl.version[sizeof(snapshot.ytdl.version) - 1] == '\0');
    CHECK(snapshot.ytdl.file[sizeof(snapshot.ytdl.file) - 1] == '\0');
    CHECK(snapshot.ytdl.message[sizeof(snapshot.ytdl.message) - 1] == '\0');
    CHECK(static_cast<LONG>(UiCommand::UpdateYtdl) == static_cast<LONG>(UiCommand::CytubePassword) + 2);

    bridge.slotSequence[1] = 8;
    CHECK(!ReadUiSnapshot(&bridge, snapshot));

    float left = 0.9f;
    float top = 0.9f;
    float width = 0.9f;
    ClampPinnedLayout(left, top, width);
    CHECK(std::abs(width - 0.8f) < 0.0001f);
    CHECK(std::abs(left - 0.2f) < 0.0001f);
    CHECK(std::abs(top - 0.55f) < 0.0001f);
    return 0;
}
