"""Checks that vista-sync.exe can load on Windows XP SP3 (32-bit).

  python -I tests/check_xp_pe.py build/v141_xp/Release/vista-sync.exe

Checks, using only the standard library (no dumpbin needed):
  * PE32 for i386, OS and subsystem version 5.01 or lower;
  * every imported DLL is one XP ships (no api-ms-win-*, bcrypt, vcruntime, ...);
  * every function imported by name is on the allowlist below. Each entry was checked
    against Microsoft's "Minimum supported client" (Windows 2000 / XP or earlier). A new
    import fails the check until someone verifies it and adds it here.
Exits 1 on any problem.
"""
import struct
import sys

XP_FUNCTIONS = {
    "KERNEL32.DLL": """
        CloseHandle CompareStringW CopyFileW CreateDirectoryW CreateEventW CreateFileW DecodePointer
        DeleteCriticalSection DeleteFileW EncodePointer EnterCriticalSection ExitProcess FileTimeToSystemTime
        FindClose FindFirstFileExW FindFirstFileW FindNextFileW FlushFileBuffers FormatMessageW
        FreeEnvironmentStringsW FreeLibrary GetACP GetCPInfo GetCommandLineA GetCommandLineW GetConsoleCP
        GetConsoleMode GetConsoleOutputCP GetCurrentProcess GetCurrentProcessId GetCurrentThread
        GetCurrentThreadId GetDateFormatW GetEnvironmentStringsW GetEnvironmentVariableW GetExitCodeThread
        GetFileAttributesExW GetFileAttributesW GetFileSizeEx GetFileType GetFullPathNameW GetLastError
        GetLocaleInfoW GetLocalTime GetModuleFileNameA GetModuleFileNameW GetModuleHandleA GetModuleHandleExW
        GetModuleHandleW GetOEMCP GetPrivateProfileIntW GetPrivateProfileStringW GetProcAddress GetProcessHeap
        GetStartupInfoA GetStartupInfoW GetStdHandle GetStringTypeW GetSystemInfo GetSystemTimeAsFileTime
        GetTempPathW GetTickCount GetTimeFormatW GetTimeZoneInformation GetUserDefaultLCID GetVersionExW
        HeapAlloc HeapFree HeapReAlloc HeapSize HeapValidate InitializeCriticalSection
        InitializeCriticalSectionAndSpinCount InitializeSListHead InterlockedCompareExchange
        InterlockedDecrement InterlockedExchange InterlockedIncrement InterlockedFlushSList
        InterlockedPushEntrySList IsDebuggerPresent IsProcessorFeaturePresent IsValidCodePage IsValidLocale
        LCMapStringW LeaveCriticalSection LoadLibraryExW LoadLibraryW LocalFree MoveFileExW MultiByteToWideChar
        OutputDebugStringW PeekNamedPipe QueryPerformanceCounter QueryPerformanceFrequency RaiseException
        ReadConsoleW ReadFile ResetEvent RtlUnwind SetConsoleCtrlHandler SetEndOfFile SetEnvironmentVariableW
        SetEvent SetFileAttributesW SetFilePointer SetFilePointerEx SetLastError SetStdHandle
        SetUnhandledExceptionFilter Sleep SystemTimeToFileTime TerminateProcess TlsAlloc TlsFree TlsGetValue
        TlsSetValue UnhandledExceptionFilter VirtualProtect VirtualQuery WaitForSingleObject
        WaitForSingleObjectEx WideCharToMultiByte WriteConsoleW WriteFile
    """,
    "ADVAPI32.DLL": """
        ChangeServiceConfig2W CloseServiceHandle ControlService CreateServiceW CryptAcquireContextW
        CryptGenRandom CryptReleaseContext DeleteService DeregisterEventSource OpenSCManagerW OpenServiceW
        QueryServiceStatus RegCloseKey RegCreateKeyExW RegDeleteKeyW RegSetValueExW RegisterEventSourceW
        RegisterServiceCtrlHandlerW ReportEventW SetServiceStatus StartServiceCtrlDispatcherW
    """,
    "CRYPT32.DLL": "CryptProtectData CryptUnprotectData",
    "WS2_32.DLL": """
        WSACleanup WSAGetLastError WSAStartup closesocket connect freeaddrinfo getaddrinfo recv send
        setsockopt socket
    """,
}
XP_FUNCTIONS = {dll: set(names.split()) for dll, names in XP_FUNCTIONS.items()}


def rva_to_off(sections, rva):
    for va, vsize, raw, rsize in sections:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    raise ValueError("RVA 0x%x not in any section" % rva)


def cstr(d, off):
    return d[off:d.index(b"\0", off)].decode("ascii")


def main(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        print("not a PE file")
        return 1
    machine, nsec, _, _, _, optsize, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    os_major, os_minor = struct.unpack_from("<HH", d, opt + 40)
    sub_major, sub_minor = struct.unpack_from("<HH", d, opt + 48)
    subsystem = struct.unpack_from("<H", d, opt + 68)[0]
    import_rva = struct.unpack_from("<I", d, opt + 96 + 8)[0]
    sections = []
    for i in range(nsec):
        s = opt + optsize + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 8)
        sections.append((va, vsize, raw, rsize))

    problems = []
    print("machine 0x%x, PE magic 0x%x, OS %d.%02d, subsystem %d version %d.%02d"
          % (machine, magic, os_major, os_minor, subsystem, sub_major, sub_minor))
    if machine != 0x14C or magic != 0x10B:
        problems.append("not a 32-bit x86 (PE32) image")
    if (os_major, os_minor) > (5, 1) or (sub_major, sub_minor) > (5, 1):
        problems.append("OS/subsystem version above 5.01: XP will refuse to start it")

    off = rva_to_off(sections, import_rva)
    total = 0
    while True:
        ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", d, off)
        if name_rva == 0:
            break
        dll = cstr(d, rva_to_off(sections, name_rva)).upper()
        allowed = XP_FUNCTIONS.get(dll)
        if allowed is None:
            problems.append("imports %s, which is not on the XP DLL list" % dll)
            allowed = set()
        t = rva_to_off(sections, ilt or iat)
        names, ordinals = [], []
        while True:
            entry = struct.unpack_from("<I", d, t)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                ordinals.append(entry & 0xFFFF)
            else:
                names.append(cstr(d, rva_to_off(sections, entry) + 2))
            t += 4
        total += len(names) + len(ordinals)
        print("  %-13s %3d by name, %2d by ordinal" % (dll, len(names), len(ordinals)))
        for n in sorted(names):
            if n not in allowed:
                problems.append("%s!%s is not on the verified-for-XP list" % (dll, n))
        if ordinals and dll != "WS2_32.DLL":
            problems.append("%s imported by ordinal %s" % (dll, ordinals))
        off += 20
    print("%d imports checked" % total)
    for p in problems:
        print("PROBLEM:", p)
    print("OK: loads on Windows XP SP3" if not problems else "FAILED")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
