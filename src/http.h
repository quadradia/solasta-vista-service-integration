#ifndef VISTA_SYNC_HTTP_H
#define VISTA_SYNC_HTTP_H

#include <string>
#include <vector>

/* Minimal HTTPS/1.1 client: Winsock + embedded mbedTLS, TLS 1.2 only, server
 * certificate and host name always verified against a PEM CA file. Windows XP
 * SChannel cannot do TLS 1.2, so the OS TLS stack is not used. */

struct HttpResponse {
    int status;
    std::string headers;
    std::string body;
    HttpResponse() : status(0) {}
};

struct TlsOptions {
    std::wstring caFile;
    int timeoutSeconds;
    TlsOptions() : timeoutSeconds(60) {}
};

/* Call once per process before any request (WSAStartup + PSA crypto init). */
bool HttpGlobalInit(std::string& err);
void HttpGlobalCleanup();

/* headers: full "Name: value" lines without CRLF. */
bool HttpsRequest(const std::string& url, const std::string& method, const std::vector<std::string>& headers,
                  const std::string& body, const TlsOptions& opt, HttpResponse& resp, std::string& err);

/* Exposed for tests. */
bool ParseHttpsUrl(const std::string& url, std::string& host, int& port, std::string& path);
bool DecodeChunked(const std::string& in, std::string& out);
std::string Base64Encode(const std::string& in);

#endif
