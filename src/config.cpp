#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include <filesystem>
#include <mutex>
#include <windows.h>

namespace
{
std::wstring Environment(const wchar_t *name)
{
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (!written || written >= size)
        return {};
    value.resize(written);
    return value;
}
} // namespace

const std::wstring &InspectorSessionDirectory()
{
    static std::once_flag once;
    static std::wstring path;
    std::call_once(once, [] {
        auto root = Environment(L"D3D11_PROXY_OUTPUT");
        if (root.empty())
        {
            root = Environment(L"LOCALAPPDATA");
            if (root.empty())
                root = Environment(L"TEMP");
            root += L"\\D3D11RenderInspector\\captures";
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        ULARGE_INTEGER stamp{};
        stamp.LowPart = created.dwLowDateTime;
        stamp.HighPart = created.dwHighDateTime;
        auto directory = std::filesystem::absolute(root) /
                         (L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(stamp.QuadPart));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!error)
            path = directory.wstring();
    });
    return path;
}

std::wstring InspectorMode()
{
    auto value = Environment(L"D3D11_PROXY_MODE");
    if (value == L"forward" || value == L"device-only")
        return value;
    return L"observe";
}
