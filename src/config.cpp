#include "config.h"

#include <windows.h>

#include "util.h"

static std::wstring ReadW(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, def, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), ini.c_str());
    return TrimW(buf);
}

static std::string ReadA(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    return WideToUtf8(ReadW(ini, section, key, def));
}

static int ReadInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def)
{
    return (int)GetPrivateProfileIntW(section, key, def, ini.c_str());
}

bool LoadConfig(const std::wstring& iniPath, Config& cfg, std::string& err)
{
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "config file not found: " + WideToUtf8(iniPath);
        return false;
    }
    std::wstring base = ExeDirectory();
    const std::wstring& ini = iniPath;
    cfg.iniPath = iniPath;

    cfg.vistaDataDir = ResolvePath(base, ReadW(ini, L"Vista", L"DataDir", L""));
    cfg.itemTable = ReadW(ini, L"Vista", L"ItemTable", L"IPART.dbf");
    cfg.source = ReadA(ini, L"Vista", L"Source", L"VISTA");
    cfg.company = ReadA(ini, L"Vista", L"Company", L"SO");
    cfg.skipClasses = SplitList(ToUpperAscii(ReadA(ini, L"Vista", L"SkipClasses", L"TEST")), ',');
    cfg.skipSkuPrefixes = SplitList(ToUpperAscii(ReadA(ini, L"Vista", L"SkipSkuPrefixes", L"SHIP-")), ',');

    cfg.intervalMinutes = ReadInt(ini, L"Sync", L"IntervalMinutes", 60);
    if (cfg.intervalMinutes < 1) cfg.intervalMinutes = 1;
    cfg.startupDelaySeconds = ReadInt(ini, L"Sync", L"StartupDelaySeconds", 60);
    if (cfg.startupDelaySeconds < 0) cfg.startupDelaySeconds = 0;
    cfg.maxItemsPerRequest = ReadInt(ini, L"Sync", L"MaxItemsPerRequest", 500);
    if (cfg.maxItemsPerRequest < 1) cfg.maxItemsPerRequest = 500;
    cfg.workDir = ResolvePath(base, ReadW(ini, L"Sync", L"WorkDir", L"work"));

    cfg.tokenUrl = ReadA(ini, L"Api", L"TokenUrl", L"");
    cfg.clientId = ReadA(ini, L"Api", L"ClientId", L"");
    cfg.scope = ReadA(ini, L"Api", L"Scope", L"");
    cfg.tokenAuth = ToUpperAscii(ReadA(ini, L"Api", L"TokenAuth", L"post")) == "BASIC" ? "basic" : "post";
    std::wstring secretFile = ReadW(ini, L"Api", L"ClientSecretFile", L"");
    cfg.clientSecretFile = secretFile.empty() ? secretFile : ResolvePath(base, secretFile);
    std::wstring dpapiFile = ReadW(ini, L"Api", L"ClientSecretDpapiFile", L"");
    cfg.clientSecretDpapiFile = dpapiFile.empty() ? dpapiFile : ResolvePath(base, dpapiFile);
    cfg.importUrl = ReadA(ini, L"Api", L"ImportUrl", L"");
    std::wstring ca = ReadW(ini, L"Api", L"CaFile", L"ca-bundle.pem");
    cfg.caFile = ResolvePath(base, ca);
    cfg.timeoutSeconds = ReadInt(ini, L"Api", L"TimeoutSeconds", 120);
    if (cfg.timeoutSeconds < 5) cfg.timeoutSeconds = 5;

    cfg.logDir = ResolvePath(base, ReadW(ini, L"Log", L"Dir", L"logs"));
    cfg.logRetentionDays = ReadInt(ini, L"Log", L"RetentionDays", 30);
    cfg.eventLog = ReadInt(ini, L"Log", L"EventLog", 1) != 0;

    if (cfg.vistaDataDir.empty()) {
        err = "[Vista] DataDir is required";
        return false;
    }
    return true;
}

bool ValidateApiConfig(const Config& cfg, std::string& err)
{
    if (!StartsWith(cfg.tokenUrl, "https://")) err = "[Api] TokenUrl must be an https:// URL";
    else if (!StartsWith(cfg.importUrl, "https://")) err = "[Api] ImportUrl must be an https:// URL";
    else if (cfg.clientId.empty()) err = "[Api] ClientId is required";
    else if (cfg.clientSecretFile.empty() && cfg.clientSecretDpapiFile.empty())
        err = "[Api] ClientSecretDpapiFile or ClientSecretFile is required";
    else if (GetFileAttributesW(cfg.caFile.c_str()) == INVALID_FILE_ATTRIBUTES)
        err = "[Api] CaFile not found: " + WideToUtf8(cfg.caFile);
    else return true;
    return false;
}
