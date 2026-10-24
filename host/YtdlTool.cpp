#include "YtdlTool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>
#include <vector>

namespace wxl_ytdl
{
    namespace
    {
        constexpr size_t kMaxOutputBytes = 64 * 1024;

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            return text;
        }

        // Calls `line` for each line of `text` (\n or \r\n), trimmed, until it returns false.
        template <typename Function>
        void ForEachLine(std::string_view text, Function line)
        {
            while (!text.empty())
            {
                const size_t end = text.find_first_of("\r\n");
                const std::string_view current = Trim(text.substr(0, end));
                if (!current.empty() && !line(current)) return;
                if (end == std::string_view::npos) return;
                text.remove_prefix(end + 1);
            }
        }

        // Digits only, 1..maxDigits of them.
        bool ReadNumber(std::string_view field, size_t maxDigits, int& value)
        {
            if (field.empty() || field.size() > maxDigits) return false;
            value = 0;
            for (const char c : field)
            {
                if (c < '0' || c > '9') return false;
                value = value * 10 + (c - '0');
            }
            return true;
        }

        std::string ToUtf8(const std::wstring& wide)
        {
            if (wide.empty()) return {};
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                                  nullptr, nullptr);
            if (bytes <= 0) return {};
            std::string text(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), bytes, nullptr,
                                nullptr);
            return text;
        }

        std::string WindowsError(DWORD code)
        {
            wchar_t* buffer = nullptr;
            const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                                nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::string text = length && buffer ? ToUtf8(std::wstring(buffer, length)) : std::string{};
            if (buffer) LocalFree(buffer);
            text = std::string(Trim(text));
            char suffix[32]{};
            std::snprintf(suffix, sizeof(suffix), " (Windows error %lu)", static_cast<unsigned long>(code));
            return text + suffix;
        }

        bool StartsWith(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        struct Handle
        {
            HANDLE value = nullptr;
            Handle() = default;
            explicit Handle(HANDLE handle) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            ~Handle() { Reset(); }
            void Reset()
            {
                if (value) CloseHandle(value);
                value = nullptr;
            }
        };
    }

    bool ValidDate(const Date& date)
    {
        if (date.year < 1900 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1) return false;
        static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = (date.year % 4 == 0 && date.year % 100 != 0) || date.year % 400 == 0;
        const int last = date.month == 2 && leap ? 29 : kDays[date.month - 1];
        return date.day <= last;
    }

    bool ParseVersion(std::string_view output, Date& date, std::string* text)
    {
        bool found = false;
        ForEachLine(output, [&](std::string_view line) {
            // YYYY.MM.DD, then optionally .N (a nightly's build time); nothing else on the line.
            std::vector<std::string_view> fields;
            size_t start = 0;
            while (true)
            {
                const size_t dot = line.find('.', start);
                fields.push_back(line.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
                if (dot == std::string_view::npos) break;
                start = dot + 1;
            }
            if (fields.size() < 3 || fields.size() > 4) return true;
            Date candidate;
            int build = 0;
            if (fields[0].size() != 4 || !ReadNumber(fields[0], 4, candidate.year) ||
                !ReadNumber(fields[1], 2, candidate.month) || !ReadNumber(fields[2], 2, candidate.day) ||
                (fields.size() == 4 && !ReadNumber(fields[3], 9, build)) || !ValidDate(candidate))
                return true;
            date = candidate;
            if (text) *text = std::string(line);
            found = true;
            return false;
        });
        return found;
    }

    int64_t DaysFromCivil(const Date& date)
    {
        // Howard Hinnant's days_from_civil.
        int64_t year = date.year;
        const unsigned month = static_cast<unsigned>(date.month);
        const unsigned day = static_cast<unsigned>(date.day);
        year -= month <= 2 ? 1 : 0;
        const int64_t era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
        const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
        const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
    }

    int AgeDays(const Date& version, const Date& today)
    {
        return static_cast<int>(DaysFromCivil(today) - DaysFromCivil(version));
    }

    Date TodayUtc()
    {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        return {now.wYear, now.wMonth, now.wDay};
    }

    Location Locate(const std::filesystem::path& helperDirectory, const FileExists& exists, const SearchExe& search)
    {
        if (!helperDirectory.empty() && exists)
        {
            const auto bundled = helperDirectory / L"yt-dlp.exe";
            if (exists(bundled)) return {Source::Bundled, bundled};
        }
        if (search)
        {
            for (const wchar_t* name : kHookNames)
            {
                std::filesystem::path found = search(name);
                if (!found.empty()) return {Source::Search, std::move(found)};
            }
        }
        return {};
    }

    bool FileExistsOnDisk(const std::filesystem::path& path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    }

    std::filesystem::path SearchProcessPath(const std::wstring& name)
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const DWORD length = SearchPathW(nullptr, name.c_str(), L".exe", static_cast<DWORD>(buffer.size()),
                                             buffer.data(), nullptr);
            if (length == 0) return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                std::filesystem::path path(buffer);
                return FileExistsOnDisk(path) ? path : std::filesystem::path{};
            }
            buffer.assign(length + 1, L'\0'); // too small: length is what it needs
        }
        return {};
    }

    std::filesystem::path ThisExeDirectory()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        while (buffer.size() <= 32768)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) return {};
            if (length < buffer.size())
            {
                buffer.resize(length);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
        return {};
    }

    Location LocateOnThisMachine()
    {
        return Locate(ThisExeDirectory(), FileExistsOnDisk, SearchProcessPath);
    }

    std::string HookPathOption(const std::filesystem::path& exe)
    {
        if (exe.empty()) return {};
        const std::string path = ToUtf8(exe.wstring());
        if (path.empty() || path.find_first_of(",;") != std::string::npos) return {};
        return "ytdl_hook-ytdl_path=" + path;
    }

    std::string PanelText(std::string_view text, size_t maxBytes)
    {
        std::string out;
        out.reserve(std::min(text.size(), maxBytes));
        bool space = false;
        for (const char raw : text)
        {
            const unsigned char c = static_cast<unsigned char>(raw);
            if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
            {
                space = !out.empty();
                continue;
            }
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
        }
        if (out.size() > maxBytes)
        {
            out.resize(maxBytes > 3 ? maxBytes - 3 : 0);
            out += "...";
        }
        return out;
    }

    std::string PathText(const std::filesystem::path& path, size_t maxBytes)
    {
        return PanelText(ToUtf8(path.wstring()), maxBytes);
    }

    std::string SummarizeUpdate(std::string_view output, unsigned long exitCode, bool timedOut)
    {
        if (timedOut) return "yt-dlp -U didn't finish in time and was stopped; try again later.";
        std::string updated, upToDate, error, last;
        ForEachLine(output, [&](std::string_view line) {
            last = std::string(line);
            if (StartsWith(line, "Updated yt-dlp to")) updated = last;
            else if (line.find("is up to date") != std::string_view::npos) upToDate = last;
            else if (StartsWith(line, "ERROR:")) error = std::string(Trim(line.substr(6)));
            return true;
        });
        if (!error.empty()) return PanelText("yt-dlp couldn't update: " + error);
        if (!updated.empty()) return PanelText(updated + ".");
        if (!upToDate.empty()) return PanelText(upToDate + ".");
        if (exitCode != 0)
        {
            char code[48]{};
            std::snprintf(code, sizeof(code), "yt-dlp -U failed (exit code %lu)", exitCode);
            return PanelText(std::string(code) + (last.empty() ? "." : ": " + last));
        }
        return last.empty() ? std::string("yt-dlp -U finished.") : PanelText(last);
    }

    bool RunCaptured(const std::filesystem::path& exe, const std::wstring& arguments, unsigned long timeoutMs,
                     std::stop_token stop, RunResult& result)
    {
        result = {};
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0))
        {
            result.error = WindowsError(GetLastError());
            return false;
        }
        Handle output(readEnd), childOutput(writeEnd);
        SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0);
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                 0, nullptr));

        // Only these handles go to the child: anything else this process happens to have open as inheritable (mpv
        // starts yt-dlp too) stays here.
        std::vector<HANDLE> inherited{childOutput.value};
        if (input.value) inherited.push_back(input.value);
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributeStorage(attributeBytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        {
            result.error = WindowsError(GetLastError());
            return false;
        }
        const bool listed = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                                      inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = childOutput.value;
        startup.StartupInfo.hStdError = childOutput.value;
        startup.lpAttributeList = listed ? attributes : nullptr;

        std::wstring commandLine = L"\"" + exe.wstring() + L"\"";
        if (!arguments.empty()) commandLine += L" " + arguments;
        PROCESS_INFORMATION process{};
        const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
        const BOOL started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                           nullptr, &startup.StartupInfo, &process);
        const DWORD startError = GetLastError();
        DeleteProcThreadAttributeList(attributes);
        childOutput.Reset(); // the child has its own copy: the pipe ends when the child (and anything it started) exits
        input.Reset();
        if (!started)
        {
            result.error = WindowsError(startError);
            return false;
        }
        Handle processHandle(process.hProcess), thread(process.hThread);
        // Its own job, so a timeout stops a PyInstaller build's second process as well as the first. Not
        // kill-on-close: closing the job here leaves a finished run alone.
        Handle job(CreateJobObjectW(nullptr, nullptr));
        if (job.value && !AssignProcessToJobObject(job.value, processHandle.value)) job.Reset();
        ResumeThread(thread.value);
        thread.Reset();

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        char buffer[4096];
        for (;;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) break; // every writer is gone
            if (available > 0)
            {
                DWORD got = 0;
                if (!ReadFile(output.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
                    break;
                if (result.output.size() < kMaxOutputBytes)
                    result.output.append(buffer, std::min<size_t>(got, kMaxOutputBytes - result.output.size()));
                continue;
            }
            if (stop.stop_requested() || GetTickCount64() >= deadline)
            {
                result.timedOut = true;
                if (job.value) TerminateJobObject(job.value, 1);
                else TerminateProcess(processHandle.value, 1);
                break;
            }
            if (WaitForSingleObject(processHandle.value, 50) == WAIT_OBJECT_0)
            {
                // Exited: take what's left, without waiting on something it started that still holds the pipe.
                DWORD left = 0;
                while (PeekNamedPipe(output.value, nullptr, 0, nullptr, &left, nullptr) && left > 0)
                {
                    DWORD got = 0;
                    if (!ReadFile(output.value, buffer, std::min<DWORD>(left, sizeof(buffer)), &got, nullptr) || got == 0)
                        break;
                    if (result.output.size() < kMaxOutputBytes)
                        result.output.append(buffer, std::min<size_t>(got, kMaxOutputBytes - result.output.size()));
                }
                break;
            }
        }
        WaitForSingleObject(processHandle.value, result.timedOut ? 5000 : 2000);
        DWORD exitCode = 0;
        if (GetExitCodeProcess(processHandle.value, &exitCode)) result.exitCode = exitCode;
        return true;
    }

    YtdlManager::YtdlManager(std::filesystem::path helperDirectory, SearchExe search, TodayFn today)
        : helperDirectory_(std::move(helperDirectory)), search_(std::move(search)), today_(std::move(today)),
          worker_([this](std::stop_token stop) { Worker(stop); })
    {
    }

    YtdlManager::~YtdlManager()
    {
        worker_.request_stop();
        wake_.notify_all();
    }

    void YtdlManager::Check()
    {
        std::lock_guard lock(mutex_);
        checkWanted_ = true;
        wake_.notify_all();
    }

    bool YtdlManager::Update(std::string& why)
    {
        std::lock_guard lock(mutex_);
        if (status_.updating)
            why = "yt-dlp is already updating.";
        else if (status_.state == Status::State::Checking)
            why = "Still looking for yt-dlp; try again in a moment.";
        else if (status_.location.source == Source::None)
            why = kMissingHelp;
        else if (status_.location.source != Source::Bundled)
            why = std::string("This yt-dlp isn't the one beside wxl-video-host.exe; update it with: ") + kPipUpdateCommand;
        else
        {
            status_.updating = true;
            status_.message = "Updating yt-dlp...";
            updateWanted_ = true;
            wake_.notify_all();
            return true;
        }
        status_.message = why;
        return false;
    }

    Status YtdlManager::Current() const
    {
        std::lock_guard lock(mutex_);
        return status_;
    }

    void YtdlManager::Worker(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            bool update = false;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this] { return checkWanted_ || updateWanted_; })) return;
                update = updateWanted_;
                updateWanted_ = false;
                checkWanted_ = false;
            }
            if (update) RunUpdate(stop);
            else RunCheck(stop, {});
        }
    }

    void YtdlManager::RunCheck(std::stop_token stop, std::string message)
    {
        Status next;
        next.location = Locate(helperDirectory_, FileExistsOnDisk, search_);
        next.message = std::move(message);
        if (next.location.source == Source::None)
        {
            next.state = Status::State::Missing;
        }
        else
        {
            RunResult run;
            Date date;
            std::string text;
            // A first run of the standalone exe unpacks itself (and may be scanned) first: give it a while.
            if (RunCaptured(next.location.path, L"--version", 60000, stop, run) && !run.timedOut &&
                ParseVersion(run.output, date, &text))
            {
                next.state = Status::State::Found;
                next.version = PanelText(text, 31);
                next.ageDays = AgeDays(date, today_ ? today_() : TodayUtc());
                next.stale = IsStale(next.ageDays);
            }
            else
            {
                next.state = Status::State::Unreadable;
                if (next.message.empty())
                {
                    std::string said;
                    ForEachLine(run.output, [&](std::string_view line) { said = std::string(line); return false; });
                    next.message = !run.error.empty() ? PanelText("yt-dlp didn't start: " + run.error)
                                   : run.timedOut     ? std::string("yt-dlp didn't answer --version in time.")
                                   : said.empty()     ? std::string("yt-dlp --version printed nothing.")
                                                      : PanelText("yt-dlp --version said: " + said);
                }
            }
        }
        if (stop.stop_requested()) return;
        std::lock_guard lock(mutex_);
        next.checks = status_.checks + 1;
        next.updating = updateWanted_; // an update asked for while this ran is still to come
        if (next.updating) next.message = status_.message;
        status_ = std::move(next);
    }

    void YtdlManager::RunUpdate(std::stop_token stop)
    {
        const Location location = Locate(helperDirectory_, FileExistsOnDisk, search_);
        std::string summary;
        if (location.source != Source::Bundled)
        {
            summary = "There's no yt-dlp.exe beside wxl-video-host.exe to update.";
        }
        else
        {
            RunResult run;
            if (!RunCaptured(location.path, L"-U", 10 * 60 * 1000, stop, run))
                summary = PanelText("yt-dlp -U didn't start: " + run.error);
            else
                summary = SummarizeUpdate(run.output, run.exitCode, run.timedOut);
        }
        if (stop.stop_requested()) return;
        RunCheck(stop, std::move(summary));
    }
}
