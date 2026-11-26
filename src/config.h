#ifndef VISTA_SYNC_CONFIG_H
#define VISTA_SYNC_CONFIG_H

#include <string>
#include <vector>

/* Settings from the INI file (see config/vista-sync.example.ini).
 * Relative paths are resolved against the folder of the .exe. */
struct Config {
    std::wstring iniPath;

    /* [Vista] */
    std::wstring vistaDataDir;   /* folder holding IPART.dbf, e.g. C:\Vista\DB\SO */
    std::wstring itemTable;      /* IPART.dbf */
    std::string source;          /* VISTA */
    std::string company;         /* SO */
    std::vector<std::string> skipClasses;     /* TEST */
    std::vector<std::string> skipSkuPrefixes; /* SHIP- */

    /* [Sync] */
    int intervalMinutes;
    int startupDelaySeconds;
    int maxItemsPerRequest;
    std::wstring workDir;        /* snapshot copy + state file */

    /* [Api] */
    std::string tokenUrl;
    std::string clientId;
    std::string scope;
    std::string tokenAuth;       /* "post" (client_secret_post) or "basic" */
    std::wstring clientSecretFile;      /* plain text, ACL-protected */
    std::wstring clientSecretDpapiFile; /* DPAPI blob made by --protect-secret */
    std::string importUrl;
    std::wstring caFile;         /* PEM bundle of CAs trusted for the token and import hosts */
    int timeoutSeconds;

    /* [Log] */
    std::wstring logDir;
    int logRetentionDays;
    bool eventLog;
};

/* Loads iniPath. Returns false and sets err when a required value is missing. */
bool LoadConfig(const std::wstring& iniPath, Config& cfg, std::string& err);
/* Checks the values a live (non-dry-run) sync needs. */
bool ValidateApiConfig(const Config& cfg, std::string& err);

#endif
