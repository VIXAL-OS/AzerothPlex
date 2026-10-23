#include <mpv/client.h>

#include <windows.h>

#include <cwchar>

namespace
{
    template <typename Function>
    Function Resolve(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }
}

// Exit code 77 is ctest's "skipped" (SKIP_RETURN_CODE in CMakeLists.txt): libmpv-2.dll is beside the probe but one of
// its own imports isn't on this machine. The x64 libmpv imports vulkan-1.dll, which comes with graphics drivers, so
// CI runners without a GPU can't load it while every player's machine can.
constexpr int kSkipped = 77;

int main()
{
    HMODULE module = LoadLibraryW(L"libmpv-2.dll");
    if (!module)
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash && wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"libmpv-2.dll") == 0 &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            {
                const HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
                if (!vulkan) return kSkipped;
                FreeLibrary(vulkan);
            }
        }
        return 1;
    }

    const auto create = Resolve<mpv_handle* (*)()>(module, "mpv_create");
    const auto setOption = Resolve<int (*)(mpv_handle*, const char*, const char*)>(
        module, "mpv_set_option_string");
    const auto initialize = Resolve<int (*)(mpv_handle*)>(module, "mpv_initialize");
    const auto destroy = Resolve<void (*)(mpv_handle*)>(module, "mpv_destroy");
    if (!create || !setOption || !initialize || !destroy)
    {
        FreeLibrary(module);
        return 2;
    }

    mpv_handle* handle = create();
    if (!handle)
    {
        FreeLibrary(module);
        return 3;
    }

    setOption(handle, "vo", "libmpv");
    const int result = initialize(handle);
    destroy(handle);
    FreeLibrary(module);
    return result >= 0 ? 0 : 4;
}
