#include "service.h"

#include <windows.h>

#include "config.h"
#include "http.h"
#include "log.h"
#include "sync.h"
#include "util.h"

namespace {

std::wstring g_configPath;
SERVICE_STATUS_HANDLE g_statusHandle = NULL;
SERVICE_STATUS g_status;
HANDLE g_stopEvent = NULL;

void SetState(DWORD state, DWORD exitCode, DWORD waitHintMs)
{
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwWaitHint = waitHintMs;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkPoint++;
    SetServiceStatus(g_statusHandle, &g_status);
}

void WINAPI ControlHandler(DWORD control)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        /* A sync in progress finishes its current HTTP call (bounded by TimeoutSeconds). */
        SetState(SERVICE_STOP_PENDING, NO_ERROR, 180000);
        SetEvent(g_stopEvent);
    } else {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, ControlHandler);
    if (!g_statusHandle) return;
    SetState(SERVICE_START_PENDING, NO_ERROR, 10000);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    Config cfg;
    std::string err;
    if (!LoadConfig(g_configPath, cfg, err)) {
        Log::Init(JoinPath(ExeDirectory(), L"logs"), 30, true, false);
        LOG_ERROR("Service cannot start: %s", err.c_str());
        Log::Shutdown();
        SetState(SERVICE_STOPPED, ERROR_BAD_CONFIGURATION, 0);
        return;
    }
    Log::Init(cfg.logDir, cfg.logRetentionDays, cfg.eventLog, false);
    if (!HttpGlobalInit(err)) {
        LOG_ERROR("Service cannot start: %s", err.c_str());
        Log::Shutdown();
        SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
        return;
    }
    SetState(SERVICE_RUNNING, NO_ERROR, 0);
    LOG_NOTICE("Service started (config %s, every %d minute(s), first run in %d s)", WideToUtf8(g_configPath).c_str(),
               cfg.intervalMinutes, cfg.startupDelaySeconds);

    SyncEngine engine(cfg);
    DWORD wait = (DWORD)cfg.startupDelaySeconds * 1000;
    while (WaitForSingleObject(g_stopEvent, wait) == WAIT_TIMEOUT) {
        SyncOptions opt;
        engine.RunOnce(opt); /* failures are logged; the next interval retries */
        wait = (DWORD)cfg.intervalMinutes * 60000;
    }

    LOG_NOTICE("Service stopped");
    HttpGlobalCleanup();
    Log::Shutdown();
    CloseHandle(g_stopEvent);
    SetState(SERVICE_STOPPED, NO_ERROR, 0);
}

bool RegisterLogSource(const std::wstring& exePath, std::string& err)
{
    std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\") + kServiceName;
    HKEY h;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL);
    if (rc != ERROR_SUCCESS) {
        err = "cannot register the Event Log source: " + LastErrorText((unsigned long)rc);
        return false;
    }
    DWORD types = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
    RegSetValueExW(h, L"EventMessageFile", 0, REG_EXPAND_SZ, (const BYTE*)exePath.c_str(),
                   (DWORD)((exePath.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(h, L"TypesSupported", 0, REG_DWORD, (const BYTE*)&types, sizeof(types));
    RegCloseKey(h);
    return true;
}

}

bool RunAsService(const std::wstring& configPath)
{
    g_configPath = configPath;
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)kServiceName, ServiceMain }, { NULL, NULL } };
    return StartServiceCtrlDispatcherW(table) != 0;
}

bool InstallService(const std::wstring& configPath, std::string& err)
{
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --service --config \"" + configPath + L"\"";
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        err = "cannot open the Service Control Manager (run as Administrator): " + LastErrorText(GetLastError());
        return false;
    }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), NULL, NULL, L"Tcpip\0\0", NULL,
                                   NULL);
    if (!svc) {
        err = "CreateService failed: " + LastErrorText(GetLastError());
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_DESCRIPTIONW desc;
    desc.lpDescription = (LPWSTR)L"Sends Vista ERP item master changes (IPART) to the Solasta API.";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    /* Restart after 1 minute on the first two crashes, then after 10 minutes. */
    SC_ACTION actions[3] = { { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 600000 } };
    SERVICE_FAILURE_ACTIONSW fa;
    ZeroMemory(&fa, sizeof(fa));
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return RegisterLogSource(exe, err);
}

bool UninstallService(std::string& err)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        err = "cannot open the Service Control Manager (run as Administrator): " + LastErrorText(GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        err = "cannot open the service: " + LastErrorText(GetLastError());
        CloseServiceHandle(scm);
        return false;
    }
    SERVICE_STATUS st;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for (int i = 0; i < 60 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(1000);
    }
    BOOL ok = DeleteService(svc);
    if (!ok) err = "DeleteService failed: " + LastErrorText(GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::wstring key = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\") + kServiceName;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key.c_str());
    return ok != 0;
}
