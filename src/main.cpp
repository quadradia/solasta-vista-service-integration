/*
 * SolastaVistaSync: Windows service that sends Vista ERP item changes to the
 * Solasta API. Built for Windows XP SP3 (32-bit, no SSE2) and later.
 */
#include <windows.h>
#include <cstdio>
#include <string>

#include "config.h"
#include "http.h"
#include "log.h"
#include "secret.h"
#include "service.h"
#include "sync.h"
#include "util.h"

static void PrintUsage(FILE* out)
{
    fputs(
        "SolastaVistaSync - sends Vista ERP items (IPART.dbf) to the Solasta API\n"
        "\n"
        "Usage:\n"
        "  vista-sync --run-once [--dry-run] [--all] [--config <ini>]\n"
        "        Run one sync in this console. --dry-run prints the JSON body instead of\n"
        "        calling the API and leaves the state file alone. --all ignores the state\n"
        "        file and includes every item.\n"
        "  vista-sync --install [--config <ini>]   Install the Windows service (Administrator).\n"
        "  vista-sync --uninstall                  Stop and remove the Windows service.\n"
        "  vista-sync --protect-secret <plain.txt> <secret.bin>\n"
        "        Encrypt the client secret with machine-scope DPAPI for ClientSecretDpapiFile.\n"
        "        Run it on the machine that runs the service, then delete plain.txt.\n"
        "\n"
        "The default config is vista-sync.ini next to the .exe.\n",
        out);
}

static std::wstring FullPath(const std::wstring& p)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, NULL);
    return (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? std::wstring(buf, n) : p;
}

int wmain(int argc, wchar_t** argv)
{
    bool runOnce = false, dryRun = false, sendAll = false, install = false, uninstall = false, service = false;
    bool protect = false;
    std::wstring configPath = JoinPath(ExeDirectory(), L"vista-sync.ini");
    std::wstring protectIn, protectOut;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--run-once") runOnce = true;
        else if (a == L"--dry-run") dryRun = true;
        else if (a == L"--all") sendAll = true;
        else if (a == L"--install") install = true;
        else if (a == L"--uninstall") uninstall = true;
        else if (a == L"--service") service = true;
        else if (a == L"--config" && i + 1 < argc) configPath = FullPath(argv[++i]);
        else if (a == L"--protect-secret" && i + 2 < argc) {
            protect = true;
            protectIn = argv[++i];
            protectOut = argv[++i];
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            PrintUsage(stdout);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n\n", WideToUtf8(a).c_str());
            PrintUsage(stderr);
            return 2;
        }
    }

    std::string err;
    if (service || (!runOnce && !install && !uninstall && !protect)) {
        if (RunAsService(configPath)) return 0;
        if (service) return 1;
        PrintUsage(stderr); /* started from a console without arguments */
        return 2;
    }
    if (protect) {
        if (!ProtectSecretFile(protectIn, protectOut, err)) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        fprintf(stdout, "Wrote %s. Set ClientSecretDpapiFile to it and delete the plain-text file.\n",
                WideToUtf8(protectOut).c_str());
        return 0;
    }
    if (install) {
        if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            fprintf(stderr, "Warning: %s does not exist yet; create it before starting the service.\n",
                    WideToUtf8(configPath).c_str());
        if (!InstallService(configPath, err)) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        fprintf(stdout, "Installed service %s (automatic start). Start it with: sc start %s\n",
                WideToUtf8(kServiceName).c_str(), WideToUtf8(kServiceName).c_str());
        return 0;
    }
    if (uninstall) {
        if (!UninstallService(err)) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
        fprintf(stdout, "Removed service %s.\n", WideToUtf8(kServiceName).c_str());
        return 0;
    }

    /* --run-once */
    Config cfg;
    if (!LoadConfig(configPath, cfg, err)) {
        fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }
    Log::Init(cfg.logDir, cfg.logRetentionDays, false, true);
    /* --dry-run writes the JSON body to stdout. When stdout is a file or a pipe (`> dryrun.json`), keep the
     * progress lines off it; in a plain console they can share it. */
    if (dryRun && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) Log::SetConsoleInfoStream(stderr);
    if (!HttpGlobalInit(err)) {
        LOG_ERROR("%s", err.c_str());
        return 1;
    }
    SyncEngine engine(cfg);
    SyncOptions opt;
    opt.dryRun = dryRun;
    opt.sendAll = sendAll;
    bool ok = engine.RunOnce(opt);
    HttpGlobalCleanup();
    Log::Shutdown();
    return ok ? 0 : 1;
}
