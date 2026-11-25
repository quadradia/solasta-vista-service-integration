#ifndef VISTA_SYNC_SERVICE_H
#define VISTA_SYNC_SERVICE_H

#include <string>

static const wchar_t kServiceName[] = L"SolastaVistaSync";
static const wchar_t kServiceDisplayName[] = L"Solasta Vista Item Sync";

/* Connects to the Service Control Manager; returns false when not started as a service. */
bool RunAsService(const std::wstring& configPath);
bool InstallService(const std::wstring& configPath, std::string& err);
bool UninstallService(std::string& err);

#endif
