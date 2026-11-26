#ifndef VISTA_SYNC_SECRET_H
#define VISTA_SYNC_SECRET_H

#include <string>

#include "config.h"

/* Reads the OAuth client secret: ClientSecretDpapiFile (preferred) or ClientSecretFile. */
bool LoadClientSecret(const Config& cfg, std::string& secret, std::string& err);

/* Encrypts the first line of plainFile with machine-scope DPAPI into outFile. */
bool ProtectSecretFile(const std::wstring& plainFile, const std::wstring& outFile, std::string& err);

#endif
