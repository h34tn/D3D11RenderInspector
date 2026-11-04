#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tlhelp32.h>

static DWORD EntryRva(const std::wstring &executable)
{
    std::ifstream stream(executable, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    stream.read(reinterpret_cast<char *>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        throw std::runtime_error("invalid executable DOS header");
    stream.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    stream.read(reinterpret_cast<char *>(&nt), sizeof(nt));
    if (!stream || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.AddressOfEntryPoint >= nt.OptionalHeader.SizeOfImage)
        throw std::runtime_error("valid x64 executable required");
    return nt.OptionalHeader.AddressOfEntryPoint;
}

struct Handle
{
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h)
    {
    }
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
static void Require(bool ok, const char *stage)
{
    if (!ok)
    {
        std::fprintf(stderr, "%s failed: %lu\n", stage, GetLastError());
        throw std::runtime_error(stage);
    }
}
static std::wstring Quote(const wchar_t *text)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (; *text; ++text)
    {
        if (*text == L'\\')
        {
            ++slashes;
            continue;
        }
        result.append(*text == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += *text;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
static uintptr_t ModuleBase(DWORD pid, const std::wstring &path)
{
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (int retry = 0; retry < 3; ++retry)
    {
        raw = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (raw != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
    }
    Handle snapshot(raw);
    Require(raw != INVALID_HANDLE_VALUE, "module snapshot");
    MODULEENTRY32W item{sizeof(item)};
    if (Module32FirstW(raw, &item))
    {
        do
        {
            if (_wcsicmp(item.szExePath, path.c_str()) == 0)
                return reinterpret_cast<uintptr_t>(item.modBaseAddr);
        } while (Module32NextW(raw, &item));
    }
    SetLastError(ERROR_MOD_NOT_FOUND);
    Require(false, "locate remote module");
    return 0;
}
static DWORD RemoteCall(HANDLE process, uintptr_t address, void *argument)
{
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument,
                                     0, nullptr));
    Require(thread.value != nullptr, "create initialization thread");
    DWORD state = WaitForSingleObject(thread.value, 15000);
    if (state == WAIT_TIMEOUT)
        SetLastError(ERROR_TIMEOUT);
    Require(state == WAIT_OBJECT_0, "wait initialization thread");
    DWORD result = 0;
    Require(GetExitCodeThread(thread.value, &result) != FALSE, "initialization result");
    return result;
}
int wmain(int argc, wchar_t **argv)
{
    // No attach mode: this launcher only owns the process it creates.
    int offset = argc > 1 && wcscmp(argv[1], L"--wait") == 0 ? 2 : 1;
    if (argc < offset + 2)
    {
        std::fprintf(stderr, "usage: startup_launcher [--wait] <absolute-hook.dll> <absolute-exe> [arguments]\n");
        return 1;
    }
    const bool wait = offset == 2;
    std::wstring dll = argv[offset], executable = argv[offset + 1];
    if (dll.size() < 3 || dll[1] != L':' || executable.size() < 3 || executable[1] != L':')
    {
        std::fprintf(stderr, "DLL and executable must have absolute drive paths\n");
        return 1;
    }
    DWORD entryRva = 0;
    try
    {
        entryRva = EntryRva(executable);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
    // Map exports locally without running DllMain or resolving dependencies.
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local)
        return 2;
    auto initialize = GetProcAddress(local, "InspectorInitialize");
    uintptr_t initializeRva =
        initialize ? reinterpret_cast<uintptr_t>(initialize) - reinterpret_cast<uintptr_t>(local) : 0;
    FreeLibrary(local);
    if (!initializeRva)
        return 3;
    std::wstring command = Quote(executable.c_str());
    for (int i = offset + 2; i < argc; ++i)
        command += L" " + Quote(argv[i]);
    std::wstring directory = executable.substr(0, executable.find_last_of(L"\\/"));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS, nullptr,
                        directory.c_str(), &startup, &child))
    {
        std::fprintf(stderr, "create process failed: %lu\n", GetLastError());
        return 4;
    }
    Handle process(child.hProcess), primary(child.hThread);
    bool startupComplete = false;
    try
    {
        BOOL wow64 = FALSE;
        Require(IsWow64Process(child.hProcess, &wow64) && !wow64, "x64 target required");
        bool ready = false;
        uintptr_t entryAddress = 0;
        BYTE originalEntry = 0;
        bool breakpointSet = false;
        ULONGLONG deadline = GetTickCount64() + 30000;
        while (!ready && GetTickCount64() < deadline)
        {
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, 100))
            {
                if (GetLastError() == ERROR_SEM_TIMEOUT)
                    continue;
                Require(false, "wait initial debugger event");
            }
            DWORD disposition = DBG_CONTINUE;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
            {
                entryAddress = reinterpret_cast<uintptr_t>(event.u.CreateProcessInfo.lpBaseOfImage) + entryRva;
                if (event.u.CreateProcessInfo.hFile)
                    CloseHandle(event.u.CreateProcessInfo.hFile);
                // Process/thread debug-event handles are owned by the debug API until detach.
            }
            else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
                CloseHandle(event.u.LoadDll.hFile);
            else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            {
                ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
                SetLastError(ERROR_PROCESS_ABORTED);
                Require(false, "target exited before initial breakpoint");
            }
            else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
            {
                if (event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT &&
                    event.u.Exception.dwFirstChance && event.dwThreadId == child.dwThreadId)
                {
                    SIZE_T bytes = 0;
                    if (!breakpointSet)
                    {
                        // The initial loader breakpoint may still hold the loader lock. Run to the
                        // executable entry before loading a DLL. Restore this debugger breakpoint
                        // and RIP before detaching; the on-disk executable is never changed.
                        Require(entryAddress != 0, "executable entry address");
                        Require(ReadProcessMemory(child.hProcess, reinterpret_cast<void *>(entryAddress),
                                                  &originalEntry, 1, &bytes) &&
                                    bytes == 1,
                                "read entry byte");
                        BYTE breakpoint = 0xcc;
                        Require(WriteProcessMemory(child.hProcess, reinterpret_cast<void *>(entryAddress), &breakpoint,
                                                   1, &bytes) &&
                                    bytes == 1,
                                "set entry breakpoint");
                        Require(FlushInstructionCache(child.hProcess, reinterpret_cast<void *>(entryAddress), 1) !=
                                    FALSE,
                                "flush entry breakpoint");
                        breakpointSet = true;
                    }
                    else if (reinterpret_cast<uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress) ==
                             entryAddress)
                    {
                        Require(WriteProcessMemory(child.hProcess, reinterpret_cast<void *>(entryAddress),
                                                   &originalEntry, 1, &bytes) &&
                                    bytes == 1,
                                "restore entry byte");
                        Require(FlushInstructionCache(child.hProcess, reinterpret_cast<void *>(entryAddress), 1) !=
                                    FALSE,
                                "flush restored entry");
                        CONTEXT context{};
                        context.ContextFlags = CONTEXT_CONTROL;
                        Require(GetThreadContext(child.hThread, &context) != FALSE, "read entry context");
                        context.Rip = entryAddress;
                        Require(SetThreadContext(child.hThread, &context) != FALSE, "restore entry context");
                        Require(SuspendThread(child.hThread) != (DWORD)-1, "suspend entry thread");
                        ready = true;
                    }
                    else
                        disposition = DBG_EXCEPTION_NOT_HANDLED;
                }
                else
                    disposition = DBG_EXCEPTION_NOT_HANDLED;
            }
            Require(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition) != FALSE,
                    "continue debug event");
        }
        Require(ready, "initial breakpoint timeout");
        Require(DebugActiveProcessStop(child.dwProcessId) != FALSE, "detach startup debugger");
        HMODULE owner = nullptr;
        auto loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        Require(loader != nullptr, "resolve LoadLibraryW");
        Require(
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(loader), &owner) != FALSE,
            "loader owner");
        wchar_t ownerPath[32768];
        Require(GetModuleFileNameW(owner, ownerPath, 32768) != 0, "loader owner path");
        uintptr_t remoteLoader = ModuleBase(child.dwProcessId, ownerPath) + reinterpret_cast<uintptr_t>(loader) -
                                 reinterpret_cast<uintptr_t>(owner);
        SIZE_T size = (dll.size() + 1) * sizeof(wchar_t), written = 0;
        void *remotePath = VirtualAllocEx(child.hProcess, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        Require(remotePath != nullptr, "allocate DLL path");
        Require(WriteProcessMemory(child.hProcess, remotePath, dll.c_str(), size, &written) && written == size,
                "copy DLL path");
        RemoteCall(child.hProcess, remoteLoader, remotePath);
        Require(VirtualFreeEx(child.hProcess, remotePath, 0, MEM_RELEASE) != FALSE, "free DLL path");
        // A thread exit code is only DWORD, so never treat LoadLibrary's truncated return as HMODULE.
        uintptr_t base = ModuleBase(child.dwProcessId, dll);
        DWORD initialized = RemoteCall(child.hProcess, base + initializeRva, nullptr);
        if (initialized != ERROR_SUCCESS)
        {
            SetLastError(initialized);
            Require(false, "install startup hooks");
        }
        Require(ResumeThread(child.hThread) != (DWORD)-1, "resume entry thread");
        startupComplete = true;
        std::printf("Launched PID %lu; startup hooks initialized. Check proxy.log and observer state.\n",
                    child.dwProcessId);
        if (wait)
        {
            Require(WaitForSingleObject(child.hProcess, 90000) == WAIT_OBJECT_0, "wait target exit");
            DWORD exitCode = 0;
            Require(GetExitCodeProcess(child.hProcess, &exitCode) != FALSE, "target exit code");
            return (int)exitCode;
        }
        return 0;
    }
    catch (...)
    {
        // Only clean up our own failed startup, never an existing application.
        if (!startupComplete)
        {
            TerminateProcess(child.hProcess, ERROR_DLL_INIT_FAILED);
            WaitForSingleObject(child.hProcess, 5000);
        }
        return 5;
    }
}
