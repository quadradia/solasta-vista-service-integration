#ifndef VISTA_SYNC_UTIL_H
#define VISTA_SYNC_UTIL_H

#include <cstdarg>
#include <string>
#include <vector>

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
/* Decodes Windows-1252 bytes (the code page Visual FoxPro tables use) to UTF-8. */
std::string Cp1252ToUtf8(const char* data, size_t len);

std::string Trim(const std::string& s);
std::wstring TrimW(const std::wstring& s);
std::string ToUpperAscii(const std::string& s);
bool StartsWith(const std::string& s, const std::string& prefix);
/* Splits on the separator, trims each part and drops empty parts. */
std::vector<std::string> SplitList(const std::string& s, char sep);

std::string FormatV(const char* fmt, va_list args);
std::string Format(const char* fmt, ...);
std::string LastErrorText(unsigned long err);

std::wstring ExeDirectory();
std::wstring ExePath();
bool IsAbsolutePath(const std::wstring& p);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
/* Makes a relative path absolute against base. */
std::wstring ResolvePath(const std::wstring& base, const std::wstring& p);
bool EnsureDirectory(const std::wstring& path);
bool ReadWholeFile(const std::wstring& path, std::string& out);
/* Writes to path.tmp and then replaces path, so a crash never leaves half a file. */
bool WriteFileAtomic(const std::wstring& path, const std::string& data);

/* JSON string literal (with quotes) for UTF-8 input. */
std::string JsonQuote(const std::string& s);
/* Minimal extraction of top-level "key": value from a flat JSON object. */
bool JsonGetString(const std::string& json, const std::string& key, std::string& out);
bool JsonGetNumber(const std::string& json, const std::string& key, double& out);

std::string UrlEncode(const std::string& s);

#endif
