#include "../YtdlTool.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED line %d: %s\n", __LINE__, #condition); return __LINE__; } } while (false)

namespace
{
    // Waits (at most 60 s) for a check that finished after `seen`, with no update running.
    bool WaitForCheck(const wxl_ytdl::YtdlManager& manager, uint64_t seen, wxl_ytdl::Status& status)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            status = manager.Current();
            if (status.checks > seen && !status.updating) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
}

// No network: the version, age, search and update logic on their own, then the manager against a stand-in yt-dlp
// (FakeYtdlp.cpp) whose path is the first argument.
int main(int argc, char** argv)
{
    using namespace wxl_ytdl;

    // Versions are release dates; a nightly adds a build number; warnings may come first.
    Date date;
    std::string text;
    CHECK(ParseVersion("2026.10.21\n", date, &text) && date.year == 2026 && date.month == 10 && date.day == 21 &&
          text == "2026.10.21");
    CHECK(ParseVersion("2026.10.21.232802\r\n", date, &text) && date.day == 21 && text == "2026.10.21.232802");
    CHECK(ParseVersion("Deprecated Feature: Support for Python version 3.9 has been deprecated\n2025.01.05\n", date, &text) &&
          date.year == 2025 && date.month == 1 && date.day == 5 && text == "2025.01.05");
    CHECK(ParseVersion("  2021.12.17  ", date, &text) && text == "2021.12.17"); // youtube-dl
    CHECK(ParseVersion("2026.7.4", date) && date.month == 7 && date.day == 4);   // pip's spelling
    CHECK(ParseVersion("2024.02.29", date));                                     // a leap day
    CHECK(!ParseVersion("", date));
    CHECK(!ParseVersion("yt-dlp 2026.10.21", date));
    CHECK(!ParseVersion("2026.13.01", date));
    CHECK(!ParseVersion("2026.02.30", date));
    CHECK(!ParseVersion("2025.02.29", date));
    CHECK(!ParseVersion("26.10.21", date));
    CHECK(!ParseVersion("2026.10", date));
    CHECK(!ParseVersion("2026.10.21.x", date));

    // Ages, across months and a year end, and the 30-day line.
    CHECK(DaysFromCivil({1970, 1, 1}) == 0);
    CHECK(DaysFromCivil({2000, 3, 1}) == 11017);
    CHECK(AgeDays({2026, 10, 21}, {2026, 10, 23}) == 2);
    CHECK(AgeDays({2026, 8, 1}, {2026, 10, 23}) == 83);
    CHECK(AgeDays({2025, 12, 31}, {2026, 1, 30}) == 30 && !IsStale(30));
    CHECK(AgeDays({2025, 12, 31}, {2026, 1, 31}) == 31 && IsStale(31));
    CHECK(AgeDays({2026, 10, 23}, {2026, 10, 21}) == -2 && !IsStale(-2)); // a clock behind the release
    const Date today = TodayUtc();
    CHECK(ValidDate(today) && today.year >= 2026);

    // Where the hook would find it: beside the helper first; else its own names, in its order; else nowhere.
    const std::filesystem::path helper = L"C:\\Games\\WoW\\Extensions\\wxl-azeroth-plex";
    std::vector<std::wstring> searched;
    const auto onlyBundled = [&](const std::filesystem::path& p) { return p == helper / L"yt-dlp.exe"; };
    const auto nothingBeside = [](const std::filesystem::path&) { return false; };
    const auto searchFinds = [&](std::wstring hit) {
        return [&searched, hit](const std::wstring& name) {
            searched.push_back(name);
            return name == hit ? std::filesystem::path(L"C:\\Python\\Scripts\\" + name + L".exe") : std::filesystem::path{};
        };
    };
    Location location = Locate(helper, onlyBundled, searchFinds(L"yt-dlp"));
    CHECK(location.source == Source::Bundled && location.path == helper / L"yt-dlp.exe" && searched.empty());
    location = Locate(helper, nothingBeside, searchFinds(L"yt-dlp"));
    CHECK(location.source == Source::Search && location.path.filename() == L"yt-dlp.exe" && searched.size() == 1);
    searched.clear();
    location = Locate(helper, nothingBeside, searchFinds(L"youtube-dl"));
    CHECK(location.source == Source::Search && location.path.filename() == L"youtube-dl.exe");
    CHECK(searched == (std::vector<std::wstring>{L"yt-dlp", L"yt-dlp_x86", L"youtube-dl"}));
    location = Locate(helper, nothingBeside, searchFinds(L"nothing"));
    CHECK(location.source == Source::None && location.path.empty());
    CHECK(Locate({}, onlyBundled, {}).source == Source::None);
    // The real search, on a name every Windows has and one none has.
    const std::filesystem::path cmd = SearchProcessPath(L"cmd");
    CHECK(!cmd.empty() && FileExistsOnDisk(cmd) && cmd.filename() == L"cmd.exe");
    CHECK(SearchProcessPath(L"no-such-ytdl-stand-in-4f1c").empty());
    CHECK(!ThisExeDirectory().empty() && FileExistsOnDisk(ThisExeDirectory() / L"ytdl_tests.exe"));

    // The hook's option: a path it can carry, or nothing (the hook then searches itself).
    CHECK(HookPathOption(L"D:\\WoW\\Extensions\\wxl-azeroth-plex\\yt-dlp.exe") ==
          "ytdl_hook-ytdl_path=D:\\WoW\\Extensions\\wxl-azeroth-plex\\yt-dlp.exe");
    CHECK(HookPathOption(L"D:\\a,b\\yt-dlp.exe").empty());
    CHECK(HookPathOption(L"D:\\a;b\\yt-dlp.exe").empty());
    CHECK(HookPathOption({}).empty());

    // What -U said, in one line.
    CHECK(SummarizeUpdate("Current version: stable@2026.10.01 from yt-dlp/yt-dlp\nLatest version: stable@2026.10.21 from yt-dlp/yt-dlp\n"
                          "Updating to stable@2026.10.21 from yt-dlp/yt-dlp ...\nUpdated yt-dlp to stable@2026.10.21 from yt-dlp/yt-dlp\n",
                          0, false) == "Updated yt-dlp to stable@2026.10.21 from yt-dlp/yt-dlp.");
    CHECK(SummarizeUpdate("Latest version: stable@2026.10.21 from yt-dlp/yt-dlp\r\n"
                          "yt-dlp is up to date (stable@2026.10.21 from yt-dlp/yt-dlp)\r\n", 0, false) ==
          "yt-dlp is up to date (stable@2026.10.21 from yt-dlp/yt-dlp).");
    CHECK(SummarizeUpdate("Current version: stable@2026.10.01\nERROR: Unable to write to C:\\x\\yt-dlp.exe; try running as administrator\n",
                          1, false) == "yt-dlp couldn't update: Unable to write to C:\\x\\yt-dlp.exe; try running as administrator");
    CHECK(SummarizeUpdate("something odd\n", 3, false) == "yt-dlp -U failed (exit code 3): something odd");
    CHECK(SummarizeUpdate("", 0, true).find("didn't finish") != std::string::npos);
    CHECK(SummarizeUpdate("", 0, false) == "yt-dlp -U finished.");
    CHECK(PanelText("a\r\nb\xC3\xA9  c") == "a b?? c");
    CHECK(PanelText(std::string(300, 'x'), 20) == std::string(17, 'x') + "...");

    // The rest runs the stand-in, copied into a scratch folder as the helper's yt-dlp.exe.
    if (argc < 2)
    {
        std::printf("FAILED: pass fake_ytdlp.exe's path\n");
        return 1;
    }
    const std::filesystem::path fake = argv[1];
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / (L"ytdl_tests_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    CHECK(std::filesystem::create_directories(scratch, error));
    const std::filesystem::path bundled = scratch / L"yt-dlp.exe";
    CHECK(std::filesystem::copy_file(fake, bundled, error));

    // A run that hangs is stopped at its timeout; one that can't start says why.
    RunResult run;
    const auto started = std::chrono::steady_clock::now();
    CHECK(RunCaptured(bundled, L"--hang", 500, {}, run) && run.timedOut);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    CHECK(!RunCaptured(scratch / L"missing.exe", L"--version", 1000, {}, run) && !run.error.empty());

    {
        const auto noSearch = [](const std::wstring&) { return std::filesystem::path{}; };
        YtdlManager manager(scratch, noSearch, [] { return Date{2026, 10, 23}; });
        Status status;
        manager.Check();
        CHECK(WaitForCheck(manager, 0, status));
        CHECK(status.state == Status::State::Found && status.location.source == Source::Bundled);
        CHECK(status.version == "2026.08.01" && status.ageDays == 83 && status.stale);

        // -U only when asked; then its result and the new version.
        std::string why;
        CHECK(manager.Update(why));
        CHECK(WaitForCheck(manager, status.checks, status));
        CHECK(status.message == "Updated yt-dlp to stable@2026.10.22 from yt-dlp/yt-dlp.");
        CHECK(status.version == "2026.10.22" && status.ageDays == 1 && !status.stale);
        CHECK(manager.Update(why));
        CHECK(WaitForCheck(manager, status.checks, status));
        CHECK(status.message.find("is up to date") != std::string::npos);

        // Gone: missing, and nothing to update.
        CHECK(std::filesystem::remove(bundled, error));
        manager.Check();
        CHECK(WaitForCheck(manager, status.checks, status));
        CHECK(status.state == Status::State::Missing && status.location.source == Source::None);
        CHECK(!manager.Update(why) && why == kMissingHelp);
    }
    {
        // Elsewhere (pip, say): found and its version read, but its update is pip's, not -U.
        const std::filesystem::path elsewhere = scratch / L"Scripts";
        CHECK(std::filesystem::create_directories(elsewhere, error));
        CHECK(std::filesystem::copy_file(fake, elsewhere / L"yt-dlp.exe", error));
        const auto search = [&](const std::wstring& name) {
            return name == L"yt-dlp" ? elsewhere / L"yt-dlp.exe" : std::filesystem::path{};
        };
        YtdlManager manager(scratch, search, [] { return Date{2026, 10, 23}; });
        Status status;
        manager.Check();
        CHECK(WaitForCheck(manager, 0, status));
        CHECK(status.state == Status::State::Found && status.location.source == Source::Search);
        CHECK(status.version == "2026.08.01" && status.stale);
        std::string why;
        CHECK(!manager.Update(why) && why.find(kPipUpdateCommand) != std::string::npos);
        CHECK(manager.Current().message == why);
    }
    std::filesystem::remove_all(scratch, error);
    std::printf("ytdl_tests: all passed\n");
    return 0;
}
