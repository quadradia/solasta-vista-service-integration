#ifndef VISTA_SYNC_SYNC_H
#define VISTA_SYNC_SYNC_H

#include <map>
#include <string>

#include "config.h"

struct SyncOptions {
    bool dryRun;   /* print the JSON instead of calling the API; state is not changed */
    bool sendAll;  /* ignore the state file and send every item */
    SyncOptions() : dryRun(false), sendAll(false) {}
};

/* One sync pass: snapshot IPART.dbf, map the parts, compare each item's hash
 * with the state file, send the changed ones, then record the new hashes. */
class SyncEngine {
public:
    explicit SyncEngine(const Config& cfg);
    /* Returns false when the pass failed (nothing or only part of it was sent). */
    bool RunOnce(const SyncOptions& opt);

private:
    bool GetToken(std::string& token, std::string& err);
    bool PostBatch(const std::string& body, std::string& err);

    Config cfg_;
    std::string token_;
    unsigned long tokenObtainedTick_;
    unsigned long tokenLifetimeMs_;
};

/* State file: one "SKU<TAB>sha256-of-item-json" line per item already accepted by the API. */
bool LoadState(const std::wstring& path, std::map<std::string, std::string>& state);
bool SaveState(const std::wstring& path, const std::map<std::string, std::string>& state);
std::string Sha256Hex(const std::string& data);

#endif
