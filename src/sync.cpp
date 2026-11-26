#include "sync.h"

#include <windows.h>
#include <cstdio>
#include <vector>

#include "mbedtls/sha256.h"

#include "dbf.h"
#include "http.h"
#include "log.h"
#include "mapping.h"
#include "secret.h"
#include "util.h"

static const wchar_t kStateFile[] = L"item-state.txt";

std::string Sha256Hex(const std::string& data)
{
    unsigned char digest[32];
    mbedtls_sha256((const unsigned char*)data.data(), data.size(), digest, 0);
    std::string hex;
    for (int i = 0; i < 32; ++i) hex += Format("%02x", digest[i]);
    return hex;
}

bool LoadState(const std::wstring& path, std::map<std::string, std::string>& state)
{
    state.clear();
    std::string raw;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true; /* first run */
    if (!ReadWholeFile(path, raw)) return false;
    size_t p = 0;
    while (p < raw.size()) {
        size_t e = raw.find('\n', p);
        if (e == std::string::npos) e = raw.size();
        std::string line = Trim(raw.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        state[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return true;
}

bool SaveState(const std::wstring& path, const std::map<std::string, std::string>& state)
{
    std::string out = "# SolastaVistaSync item state v1: SKU<TAB>sha256 of the item JSON last accepted by the API\r\n";
    for (std::map<std::string, std::string>::const_iterator it = state.begin(); it != state.end(); ++it)
        out += it->first + "\t" + it->second + "\r\n";
    return WriteFileAtomic(path, out);
}

SyncEngine::SyncEngine(const Config& cfg) : cfg_(cfg), tokenObtainedTick_(0), tokenLifetimeMs_(0) {}

bool SyncEngine::GetToken(std::string& token, std::string& err)
{
    if (!token_.empty() && GetTickCount() - tokenObtainedTick_ < tokenLifetimeMs_) {
        token = token_;
        return true;
    }
    token_.clear();
    std::string secret;
    if (!LoadClientSecret(cfg_, secret, err)) return false;

    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    headers.push_back("Accept: application/json");
    std::string form = "grant_type=client_credentials";
    if (!cfg_.scope.empty()) form += "&scope=" + UrlEncode(cfg_.scope);
    if (cfg_.tokenAuth == "basic") {
        headers.push_back("Authorization: Basic " + Base64Encode(UrlEncode(cfg_.clientId) + ":" + UrlEncode(secret)));
    } else {
        form += "&client_id=" + UrlEncode(cfg_.clientId) + "&client_secret=" + UrlEncode(secret);
    }
    SecureZeroMemory(&secret[0], secret.size());

    TlsOptions tls;
    tls.caFile = cfg_.caFile;
    tls.timeoutSeconds = cfg_.timeoutSeconds;
    HttpResponse resp;
    bool ok = HttpsRequest(cfg_.tokenUrl, "POST", headers, form, tls, resp, err);
    SecureZeroMemory(&form[0], form.size());
    if (!headers.empty()) SecureZeroMemory(&headers.back()[0], headers.back().size());
    if (!ok) {
        err = "token request failed: " + err;
        return false;
    }
    if (resp.status != 200) {
        std::string oauthErr, desc;
        JsonGetString(resp.body, "error", oauthErr);
        JsonGetString(resp.body, "error_description", desc);
        err = Format("token request returned HTTP %d %s %s", resp.status, oauthErr.c_str(), desc.c_str());
        return false;
    }
    double expiresIn = 3600;
    if (!JsonGetString(resp.body, "access_token", token_) || token_.empty()) {
        err = "token response has no access_token";
        return false;
    }
    JsonGetNumber(resp.body, "expires_in", expiresIn);
    /* Renew a minute early; never cache for less than 30 s or longer than a day. */
    double lifetime = expiresIn - 60;
    if (lifetime < 30) lifetime = 30;
    if (lifetime > 86400) lifetime = 86400;
    tokenLifetimeMs_ = (unsigned long)(lifetime * 1000);
    tokenObtainedTick_ = GetTickCount();
    LOG_INFO("Obtained access token for client %s (expires in %.0f s)", cfg_.clientId.c_str(), expiresIn);
    token = token_;
    return true;
}

bool SyncEngine::PostBatch(const std::string& body, std::string& err)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string token;
        if (!GetToken(token, err)) return false;
        std::vector<std::string> headers;
        headers.push_back("Authorization: Bearer " + token);
        headers.push_back("Content-Type: application/json; charset=utf-8");
        headers.push_back("Accept: application/json");
        TlsOptions tls;
        tls.caFile = cfg_.caFile;
        tls.timeoutSeconds = cfg_.timeoutSeconds;
        HttpResponse resp;
        DWORD start = GetTickCount();
        if (!HttpsRequest(cfg_.importUrl, "POST", headers, body, tls, resp, err)) {
            err = "import request failed: " + err;
            return false;
        }
        std::string snippet = resp.body.substr(0, 1000);
        LOG_INFO("Import POST returned HTTP %d in %lu ms: %s", resp.status, GetTickCount() - start, snippet.c_str());
        if (resp.status == 401 && attempt == 0) {
            LOG_WARN("Import returned 401; requesting a new token and retrying once");
            token_.clear();
            continue;
        }
        if (resp.status >= 200 && resp.status < 300) return true;
        err = Format("import returned HTTP %d: %s", resp.status, snippet.c_str());
        return false;
    }
    return false;
}

bool SyncEngine::RunOnce(const SyncOptions& opt)
{
    DWORD started = GetTickCount();
    std::wstring source = JoinPath(cfg_.vistaDataDir, cfg_.itemTable);
    std::wstring snapDir = JoinPath(cfg_.workDir, L"snapshot");
    std::wstring snapshot = JoinPath(snapDir, cfg_.itemTable);
    if (!EnsureDirectory(snapDir)) {
        LOG_ERROR("Cannot create work folder %s", WideToUtf8(snapDir).c_str());
        return false;
    }
    /* Read a private copy so Vista's own locks and writes are never touched. */
    if (!CopyFileW(source.c_str(), snapshot.c_str(), FALSE)) {
        LOG_ERROR("Cannot copy %s to the snapshot folder: %s", WideToUtf8(source).c_str(), LastErrorText(GetLastError()).c_str());
        return false;
    }
    SetFileAttributesW(snapshot.c_str(), FILE_ATTRIBUTE_NORMAL); /* drop read-only copied from the source */

    DbfTable table;
    std::string err;
    bool opened = table.Open(snapshot, err);
    if (!opened) {
        LOG_ERROR("Cannot read %s: %s", WideToUtf8(source).c_str(), err.c_str());
        return false;
    }
    if (table.CodePageMark() != 0x03 && table.CodePageMark() != 0)
        LOG_WARN("Table code page mark is 0x%02X, not 0x03 (Windows-1252); text is decoded as Windows-1252 anyway",
                 table.CodePageMark());

    MapResult mapped;
    if (!MapParts(table, cfg_, mapped, err)) {
        LOG_ERROR("Cannot map %s: %s", WideToUtf8(source).c_str(), err.c_str());
        return false;
    }
    DeleteFileW(snapshot.c_str());

    LOG_INFO("Read %s: %u live records, %u deleted, %u items mapped, %u skipped", WideToUtf8(source).c_str(),
             mapped.liveRecords, mapped.deletedRecords, (unsigned)mapped.items.size(), (unsigned)mapped.skipped.size());
    for (size_t i = 0; i < mapped.skipped.size(); ++i)
        LOG_INFO("  skipped %s: %s", mapped.skipped[i].part.c_str(), mapped.skipped[i].reason.c_str());
    for (size_t i = 0; i < mapped.warnings.size(); ++i) LOG_WARN("  %s", mapped.warnings[i].c_str());

    std::wstring statePath = JoinPath(cfg_.workDir, kStateFile);
    std::map<std::string, std::string> state;
    if (!LoadState(statePath, state)) {
        LOG_ERROR("Cannot read state file %s", WideToUtf8(statePath).c_str());
        return false;
    }

    std::vector<Item> changed;
    std::vector<std::string> hashes;
    for (size_t i = 0; i < mapped.items.size(); ++i) {
        std::string h = Sha256Hex(mapped.items[i].ToJson());
        std::map<std::string, std::string>::const_iterator it = state.find(mapped.items[i].sku);
        if (opt.sendAll || it == state.end() || it->second != h) {
            changed.push_back(mapped.items[i]);
            hashes.push_back(h);
        }
    }
    unsigned unchanged = (unsigned)(mapped.items.size() - changed.size());
    unsigned goneFromVista = 0;
    for (std::map<std::string, std::string>::const_iterator it = state.begin(); it != state.end(); ++it) {
        bool found = false;
        for (size_t i = 0; i < mapped.items.size() && !found; ++i) found = mapped.items[i].sku == it->first;
        if (!found) ++goneFromVista;
    }
    if (goneFromVista)
        LOG_WARN("%u previously synced SKU(s) are no longer in Vista (or are now skipped); v1 does not deactivate them",
                 goneFromVista);

    if (opt.dryRun) {
        std::string body = BuildBatchJson(cfg_, changed, 0, changed.size(), true);
        fwrite(body.data(), 1, body.size(), stdout);
        fflush(stdout);
        LOG_NOTICE("Dry run: %u items would be sent (%u unchanged, %u skipped, %u live records). Nothing was sent.",
                   (unsigned)changed.size(), unchanged, (unsigned)mapped.skipped.size(), mapped.liveRecords);
        return true;
    }

    if (changed.empty()) {
        LOG_INFO("No item changes since the last sync (%u items checked)", (unsigned)mapped.items.size());
        return true;
    }

    std::string cfgErr;
    if (!ValidateApiConfig(cfg_, cfgErr)) {
        LOG_ERROR("Config error: %s", cfgErr.c_str());
        return false;
    }

    unsigned sentOk = 0;
    size_t per = (size_t)cfg_.maxItemsPerRequest;
    for (size_t begin = 0; begin < changed.size(); begin += per) {
        size_t end = begin + per < changed.size() ? begin + per : changed.size();
        std::string body = BuildBatchJson(cfg_, changed, begin, end, false);
        if (!PostBatch(body, err)) {
            LOG_ERROR("Sync failed after %u of %u changed items: %s", sentOk, (unsigned)changed.size(), err.c_str());
            return false;
        }
        for (size_t i = begin; i < end; ++i) state[changed[i].sku] = hashes[i];
        if (!SaveState(statePath, state)) {
            LOG_ERROR("Items were sent but the state file %s could not be written; they will be sent again next time",
                      WideToUtf8(statePath).c_str());
            return false;
        }
        sentOk += (unsigned)(end - begin);
    }
    LOG_NOTICE("Sync complete: %u items sent, %u unchanged, %u skipped, in %lu ms", sentOk, unchanged,
               (unsigned)mapped.skipped.size(), GetTickCount() - started);
    return true;
}
