#include "util.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string Cp1252ToUtf8(const char* data, size_t len)
{
    if (len == 0) return std::string();
    int n = MultiByteToWideChar(1252, 0, data, (int)len, NULL, 0);
    if (n <= 0) return std::string();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(1252, 0, data, (int)len, &w[0], n);
    return WideToUtf8(w);
}

static bool IsSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && IsSpaceChar(s[b])) ++b;
    while (e > b && IsSpaceChar(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring TrimW(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::string ToUpperAscii(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] >= 'a' && r[i] <= 'z') r[i] = (char)(r[i] - 'a' + 'A');
    return r;
}

bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> SplitList(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) pos = s.size();
        std::string part = Trim(s.substr(start, pos - start));
        if (!part.empty()) out.push_back(part);
        start = pos + 1;
    }
    return out;
}

std::string FormatV(const char* fmt, va_list args)
{
    char stackBuf[1024];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, copy);
    va_end(copy);
    if (n < 0) return std::string(fmt);
    if (n < (int)sizeof(stackBuf)) return std::string(stackBuf, n);
    std::string big(n + 1, '\0');
    vsnprintf(&big[0], big.size(), fmt, args);
    big.resize(n);
    return big;
}

std::string Format(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string s = FormatV(fmt, args);
    va_end(args);
    return s;
}

std::string LastErrorText(unsigned long err)
{
    wchar_t* buf = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, (LPWSTR)&buf, 0, NULL);
    std::string msg;
    if (n && buf) {
        msg = Trim(WideToUtf8(std::wstring(buf, n)));
        LocalFree(buf);
    }
    return Format("error %lu%s%s", err, msg.empty() ? "" : ": ", msg.c_str());
}

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

std::wstring ExeDirectory()
{
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring(L".") : p.substr(0, pos);
}

bool IsAbsolutePath(const std::wstring& p)
{
    if (p.size() >= 2 && p[1] == L':') return true;          /* C:\... */
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true; /* \\server\share */
    return false;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring ResolvePath(const std::wstring& base, const std::wstring& p)
{
    if (p.empty() || IsAbsolutePath(p)) return p;
    return JoinPath(base, p);
}

bool EnsureDirectory(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 0) {
        std::wstring parent = path.substr(0, pos);
        if (!(parent.size() == 2 && parent[1] == L':')) EnsureDirectory(parent);
    }
    return CreateDirectoryW(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.assign((size_t)size.QuadPart, '\0');
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = (DWORD)((out.size() - done) > 0x100000 ? 0x100000 : (out.size() - done));
        DWORD got = 0;
        if (!ReadFile(h, &out[done], chunk, &got, NULL) || got == 0) break;
        done += got;
    }
    CloseHandle(h);
    out.resize(done);
    return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data)
{
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = data.empty() || WriteFile(h, data.data(), (DWORD)data.size(), &written, NULL);
    ok = ok && written == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string JsonQuote(const std::string& s)
{
    std::string r;
    r.reserve(s.size() + 2);
    r += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\b': r += "\\b"; break;
        case '\f': r += "\\f"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default:
            if (c < 0x20) r += Format("\\u%04x", c);
            else r += (char)c;
        }
    }
    r += '"';
    return r;
}

/* Finds the position just after `"key"` followed by optional spaces and a colon. */
static size_t JsonFindValue(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        size_t p = pos + needle.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
            return p;
        }
        pos += needle.size();
    }
    return std::string::npos;
}

bool JsonGetString(const std::string& json, const std::string& key, std::string& out)
{
    size_t p = JsonFindValue(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '"') return false;
    out.clear();
    for (++p; p < json.size(); ++p) {
        char c = json[p];
        if (c == '"') return true;
        if (c == '\\' && p + 1 < json.size()) {
            char e = json[++p];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
                /* Tokens and error texts are ASCII; keep \uXXXX as '?' for anything else. */
                if (p + 4 < json.size()) {
                    unsigned v = (unsigned)strtoul(json.substr(p + 1, 4).c_str(), NULL, 16);
                    out += (v < 0x80) ? (char)v : '?';
                    p += 4;
                }
                break;
            default: out += e;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool JsonGetNumber(const std::string& json, const std::string& key, double& out)
{
    size_t p = JsonFindValue(json, key);
    if (p == std::string::npos) return false;
    const char* start = json.c_str() + p;
    if (*start == '"') ++start; /* some servers quote numbers */
    char* end = NULL;
    out = strtod(start, &end);
    return end != start;
}

std::string UrlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            r += (char)c;
        } else {
            r += '%';
            r += hex[c >> 4];
            r += hex[c & 15];
        }
    }
    return r;
}
