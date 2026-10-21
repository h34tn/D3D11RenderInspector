#define WIN32_LEAN_AND_MEAN
#include "config.h"
#include "exports.inc"
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <intrin.h>
#include <windows.h>
extern "C" void InstallObservers(ID3D11Device *, ID3D11DeviceContext *);

static HMODULE selfModule = nullptr, realModule = nullptr;
static INIT_ONCE initOnce = INIT_ONCE_STATIC_INIT;
static PVOID volatile cached[exportCount] = {};
static std::wstring logPath;
static SRWLOCK logLock = SRWLOCK_INIT;

static void Log(const char *message)
{
    DWORD saved = GetLastError();
    if (!logPath.empty())
    {
        AcquireSRWLockExclusive(&logLock);
        HANDLE f = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            char line[1800];
            int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "tick=%llu pid=%lu tid=%lu %s\r\n", GetTickCount64(),
                                  GetCurrentProcessId(), GetCurrentThreadId(), message);
            DWORD n;
            if (len > 0)
                WriteFile(f, line, (DWORD)len, &n, nullptr);
            CloseHandle(f);
        }
        ReleaseSRWLockExclusive(&logLock);
    }
    SetLastError(saved);
}
extern "C" void ObserverInfo(const char *m)
{
    Log(m);
}
static BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID *)
{
    wchar_t systemPath[MAX_PATH];
    const auto &directory = InspectorSessionDirectory();
    if (!directory.empty())
        logPath = directory + L"\\proxy.log";
    UINT length = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!length || length >= MAX_PATH - 11)
    {
        Log("ERROR system path unavailable");
        return FALSE;
    }
    wcscat_s(systemPath, L"\\d3d11.dll");
    HMODULE module = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module || module == selfModule)
    {
        Log("ERROR real runtime unavailable or recursive load");
        return FALSE;
    }
    realModule = module;
    wchar_t actual[MAX_PATH];
    GetModuleFileNameW(realModule, actual, MAX_PATH);
    char message[1500];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
                "initialized real=%ls self=%p realModule=%p build=D3D11RenderInspector-0.1-private-tail", actual,
                selfModule, realModule);
    Log(message);
    // Keep the real runtime loaded: returned COM objects may outlive this DLL's callers.
    return TRUE;
}
extern "C" FARPROC Resolve(unsigned index)
{
    DWORD saved = GetLastError();
    if (index >= exportCount)
    {
        SetLastError(saved);
        return nullptr;
    }
    if (!InitOnceExecuteOnce(&initOnce, Initialize, nullptr, nullptr))
        return nullptr;
    auto address = (FARPROC)InterlockedCompareExchangePointer(&cached[index], nullptr, nullptr);
    if (!address)
    {
        address = GetProcAddress(realModule, exportNames[index]);
        if (!address)
            return nullptr;
        if (InterlockedCompareExchangePointer(&cached[index], (PVOID)address, nullptr) == nullptr)
        {
            char message[300];
            _snprintf_s(message, sizeof(message), _TRUNCATE, "resolved %s", exportNames[index]);
            Log(message);
        }
    }
    SetLastError(saved);
    return address;
}
extern "C" FARPROC ResolveRequired(unsigned index)
{
    auto p = Resolve(index);
    if (!p)
    {
        Log("FATAL required export missing");
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    return p;
}
extern "C" HRESULT WINAPI ProxyCreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                            const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk,
                                            ID3D11Device **device, D3D_FEATURE_LEVEL *selected,
                                            ID3D11DeviceContext **context)
{
    using Fn = decltype(&D3D11CreateDevice);
    auto fn = (Fn)Resolve(indexCreateDevice);
    if (!fn)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    Log("D3D11CreateDevice enter");
    HRESULT hr = fn(adapter, type, software, flags, levels, count, sdk, device, selected, context);
    DWORD apiError = GetLastError();
    char message[300];
    _snprintf_s(message, sizeof(message), _TRUNCATE,
                "D3D11CreateDevice result=0x%08lX type=%u flags=0x%X device=%p context=%p", (ULONG)hr, (UINT)type,
                flags, SUCCEEDED(hr) && device ? *device : nullptr, SUCCEEDED(hr) && context ? *context : nullptr);
    Log(message);
    if (SUCCEEDED(hr))
        InstallObservers(device ? *device : nullptr, context ? *context : nullptr);
    SetLastError(apiError);
    return hr;
}
extern "C" HRESULT WINAPI ProxyCreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software,
                                                        UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count,
                                                        UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc,
                                                        IDXGISwapChain **swap, ID3D11Device **device,
                                                        D3D_FEATURE_LEVEL *selected, ID3D11DeviceContext **context)
{
    using Fn = decltype(&D3D11CreateDeviceAndSwapChain);
    auto fn = (Fn)Resolve(indexCreateSwap);
    if (!fn)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    Log("D3D11CreateDeviceAndSwapChain enter");
    HRESULT hr = fn(adapter, type, software, flags, levels, count, sdk, desc, swap, device, selected, context);
    DWORD apiError = GetLastError();
    char message[200];
    _snprintf_s(message, sizeof(message), _TRUNCATE, "D3D11CreateDeviceAndSwapChain result=0x%08lX", (ULONG)hr);
    Log(message);
    if (SUCCEEDED(hr))
        InstallObservers(device ? *device : nullptr, context ? *context : nullptr);
    SetLastError(apiError);
    return hr;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        selfModule = module;
    return TRUE;
}
