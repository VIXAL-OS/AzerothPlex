// A stand-in for yt-dlp.exe in ytdl_tests: answers --version and -U the way yt-dlp does, with no network. Its
// "installed" version lives in fake-version.txt beside it (2026.08.01 when there's none); -U writes 2026.10.22 there.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace
{
    std::wstring VersionFile()
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring file(path, length);
        file.resize(file.find_last_of(L"\\/") + 1);
        return file + L"fake-version.txt";
    }

    std::string InstalledVersion()
    {
        std::ifstream input(VersionFile());
        std::string version;
        if (input && std::getline(input, version) && !version.empty()) return version;
        return "2026.08.01";
    }
}

int main(int argc, char** argv)
{
    const char* argument = argc > 1 ? argv[1] : "";
    if (std::strcmp(argument, "--version") == 0)
    {
        // A warning first, on stderr, as yt-dlp prints for an old Python: the version is still found.
        std::fprintf(stderr, "WARNING: this is ytdl_tests' stand-in, not yt-dlp\n");
        std::fflush(stderr);
        std::printf("%s\n", InstalledVersion().c_str());
        return 0;
    }
    if (std::strcmp(argument, "-U") == 0)
    {
        const std::string current = InstalledVersion();
        std::printf("Current version: stable@%s from yt-dlp/yt-dlp\n", current.c_str());
        std::printf("Latest version: stable@2026.10.22 from yt-dlp/yt-dlp\n");
        if (current == "2026.10.22")
        {
            std::printf("yt-dlp is up to date (stable@2026.10.22 from yt-dlp/yt-dlp)\n");
            return 0;
        }
        std::ofstream(VersionFile(), std::ios::trunc) << "2026.10.22\n";
        std::printf("Updating to stable@2026.10.22 from yt-dlp/yt-dlp ...\n");
        std::printf("Updated yt-dlp to stable@2026.10.22 from yt-dlp/yt-dlp\n");
        return 0;
    }
    if (std::strcmp(argument, "--hang") == 0)
    {
        std::printf("hanging\n");
        std::fflush(stdout);
        Sleep(30000);
        return 0;
    }
    std::fprintf(stderr, "ERROR: unexpected arguments\n");
    return 2;
}
