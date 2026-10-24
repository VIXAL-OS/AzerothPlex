#pragma once

// yt-dlp, which mpv's ytdl hook runs for CyTube's YouTube, Vimeo, Twitch and similar media: where this helper finds it
// (the places the hook would look), its version and how old that is, and its own updater (`yt-dlp -U`, only for the
// copy beside the helper and only when asked). The parsing and the search take their I/O as arguments, so the tests
// run without yt-dlp or a network.

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace wxl_ytdl
{
    struct Date
    {
        int year = 0;
        int month = 0;
        int day = 0;
    };

    // yt-dlp's versions are its release dates: "2026.10.21", or "2026.10.21.232802" for a nightly. The first line of
    // `--version`'s output that is one (warnings may come first); `text` gets it as printed.
    bool ParseVersion(std::string_view output, Date& date, std::string* text = nullptr);
    int64_t DaysFromCivil(const Date& date); // days since 1970-01-01 (proleptic Gregorian)
    bool ValidDate(const Date& date);
    // Whole days from the version's date to today; negative if the clock is behind the release.
    int AgeDays(const Date& version, const Date& today);
    // YouTube changes break old yt-dlp versions within weeks; past this the panel asks for an update.
    constexpr int kStaleAfterDays = 30;
    inline bool IsStale(int ageDays) { return ageDays > kStaleAfterDays; }
    Date TodayUtc();

    enum class Source
    {
        None,
        Bundled, // yt-dlp.exe beside wxl-video-host.exe
        Search,  // found the way CreateProcess finds a bare name: a pip or other install, usually on PATH
    };

    struct Location
    {
        Source source = Source::None;
        std::filesystem::path path;
    };

    // The names mpv's ytdl hook tries, in this order, when its ytdl_path is empty.
    inline constexpr const wchar_t* kHookNames[] = {L"yt-dlp", L"yt-dlp_x86", L"youtube-dl"};

    using FileExists = std::function<bool(const std::filesystem::path&)>;
    using SearchExe = std::function<std::filesystem::path(const std::wstring& name)>; // "" when not found

    // What the hook would run: yt-dlp.exe beside the helper, which this helper hands the hook as its ytdl_path; else
    // the first of the hook's names that `search` finds (the hook runs the bare name, and CreateProcess looks for
    // <name>.exe in the helper's folder, the current folder, the system folders and PATH). With config=no the hook
    // has no config folders to look in first.
    Location Locate(const std::filesystem::path& helperDirectory, const FileExists& exists, const SearchExe& search);
    bool FileExistsOnDisk(const std::filesystem::path& path);
    std::filesystem::path SearchProcessPath(const std::wstring& name); // SearchPathW(<name>.exe)
    std::filesystem::path ThisExeDirectory();
    Location LocateOnThisMachine(); // from this exe's folder, on the real file system

    // mpv's script-opts value that points the hook at `exe`; "" (the hook searches itself) for no path or one the
    // option can't carry: script-opts are comma-separated and the hook splits ytdl_path at ';'.
    std::string HookPathOption(const std::filesystem::path& exe);

    // A one-line account of `yt-dlp -U` for the panel.
    std::string SummarizeUpdate(std::string_view output, unsigned long exitCode, bool timedOut);
    // Printable ASCII only (the in-game font has nothing else), one line, at most maxBytes.
    std::string PanelText(std::string_view text, size_t maxBytes = 160);
    std::string PathText(const std::filesystem::path& path, size_t maxBytes = 160); // PanelText of the path as UTF-8

    struct RunResult
    {
        std::string output;        // stdout and stderr together, at most 64 KiB
        unsigned long exitCode = 0;
        bool timedOut = false;     // stopped after the timeout (or when asked to stop)
        std::string error;         // why it couldn't start
    };
    // Runs `"exe" arguments` with no window, its own job (so a timeout stops all of it), stdin from NUL and only its
    // output pipe inherited. False if it couldn't start.
    bool RunCaptured(const std::filesystem::path& exe, const std::wstring& arguments, unsigned long timeoutMs,
                     std::stop_token stop, RunResult& result);

    struct Status
    {
        enum class State
        {
            Checking,
            Missing,
            Found,
            Unreadable, // found, but `--version` gave nothing usable
        };
        State state = State::Checking;
        Location location;
        std::string version;   // as it printed it
        int ageDays = -1;      // -1: unknown
        bool stale = false;
        bool updating = false;
        std::string message;   // the last check's or update's result, for the panel
        uint64_t checks = 0;   // finished checks, so a caller can tell a new result from one it has seen
    };

    // Checks and updates on a worker thread; the UI thread reads Current() each frame.
    class YtdlManager final
    {
    public:
        using TodayFn = std::function<Date()>;

        explicit YtdlManager(std::filesystem::path helperDirectory, SearchExe search = SearchProcessPath,
                             TodayFn today = TodayUtc);
        ~YtdlManager();
        YtdlManager(const YtdlManager&) = delete;
        YtdlManager& operator=(const YtdlManager&) = delete;

        void Check();  // where it is and its version, in the background
        // `yt-dlp -U` on the copy beside the helper, in the background, then a check. False (and why, in the
        // status's message as well) when there is none, or an update is already running.
        bool Update(std::string& why);
        Status Current() const;

    private:
        void Worker(std::stop_token stop);
        void RunCheck(std::stop_token stop, std::string message);
        void RunUpdate(std::stop_token stop);

        std::filesystem::path helperDirectory_;
        SearchExe search_;
        TodayFn today_;
        mutable std::mutex mutex_;
        std::condition_variable_any wake_;
        bool checkWanted_ = false;
        bool updateWanted_ = false;
        Status status_;
        std::jthread worker_; // last: it starts with everything above in place
    };

    // The panel's line when there is no yt-dlp.
    inline constexpr const char kMissingHelp[] =
        "YouTube (and Vimeo, Twitch, ...) needs yt-dlp: put yt-dlp.exe next to wxl-video-host.exe in "
        "Extensions\\wxl-azeroth-plex.";
    inline constexpr const char kPipUpdateCommand[] = "python -m pip install -U yt-dlp";
}
