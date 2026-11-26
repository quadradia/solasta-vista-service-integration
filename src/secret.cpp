#include "secret.h"

#include <windows.h>
#include <wincrypt.h>

#include "util.h"

/* Extra entropy mixed into the DPAPI blob so another program calling
 * CryptUnprotectData on the file also needs to know this string. */
static const char kEntropy[] = "SolastaVistaSync.ClientSecret.v1";

static void SecureClear(std::string& s)
{
    if (!s.empty()) SecureZeroMemory(&s[0], s.size());
    s.clear();
}

bool LoadClientSecret(const Config& cfg, std::string& secret, std::string& err)
{
    std::string raw;
    if (!cfg.clientSecretDpapiFile.empty()) {
        if (!ReadWholeFile(cfg.clientSecretDpapiFile, raw) || raw.empty()) {
            err = "cannot read ClientSecretDpapiFile " + WideToUtf8(cfg.clientSecretDpapiFile);
            return false;
        }
        DATA_BLOB in, entropy, out;
        in.pbData = (BYTE*)&raw[0];
        in.cbData = (DWORD)raw.size();
        entropy.pbData = (BYTE*)kEntropy;
        entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);
        out.pbData = NULL;
        out.cbData = 0;
        if (!CryptUnprotectData(&in, NULL, &entropy, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            err = "cannot decrypt ClientSecretDpapiFile (it must be created on this machine with --protect-secret): " +
                  LastErrorText(GetLastError());
            return false;
        }
        secret.assign((const char*)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return !secret.empty();
    }
    if (!ReadWholeFile(cfg.clientSecretFile, raw)) {
        err = "cannot read ClientSecretFile " + WideToUtf8(cfg.clientSecretFile);
        return false;
    }
    size_t eol = raw.find_first_of("\r\n");
    secret = Trim(raw.substr(0, eol));
    SecureClear(raw);
    if (secret.empty()) {
        err = "ClientSecretFile is empty";
        return false;
    }
    return true;
}

bool ProtectSecretFile(const std::wstring& plainFile, const std::wstring& outFile, std::string& err)
{
    std::string raw;
    if (!ReadWholeFile(plainFile, raw)) {
        err = "cannot read " + WideToUtf8(plainFile);
        return false;
    }
    size_t eol = raw.find_first_of("\r\n");
    std::string secret = Trim(raw.substr(0, eol));
    SecureClear(raw);
    if (secret.empty()) {
        err = "the secret file is empty";
        return false;
    }
    DATA_BLOB in, entropy, out;
    in.pbData = (BYTE*)&secret[0];
    in.cbData = (DWORD)secret.size();
    entropy.pbData = (BYTE*)kEntropy;
    entropy.cbData = (DWORD)(sizeof(kEntropy) - 1);
    out.pbData = NULL;
    out.cbData = 0;
    /* Machine scope so the service (LocalSystem or any service account) can decrypt it. */
    BOOL ok = CryptProtectData(&in, L"SolastaVistaSync client secret", &entropy, NULL, NULL,
                               CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &out);
    SecureClear(secret);
    if (!ok) {
        err = "CryptProtectData failed: " + LastErrorText(GetLastError());
        return false;
    }
    std::string blob((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (!WriteFileAtomic(outFile, blob)) {
        err = "cannot write " + WideToUtf8(outFile);
        return false;
    }
    return true;
}
