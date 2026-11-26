#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include "http.h"

#include <cstdlib>
#include <cstring>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h" /* MBEDTLS_ERR_NET_* codes only */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "log.h"
#include "util.h"

/* Entropy source for mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT): CryptGenRandom is
 * available on Windows XP, unlike the BCryptGenRandom call mbedTLS uses by default. */
extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen)
{
    (void)data;
    *olen = 0;
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    BOOL ok = CryptGenRandom(prov, (DWORD)len, output);
    CryptReleaseContext(prov, 0);
    if (!ok) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

static bool g_httpInitialized = false;

bool HttpGlobalInit(std::string& err)
{
    if (g_httpInitialized) return true;
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        err = "WSAStartup failed: " + LastErrorText((unsigned long)rc);
        return false;
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        err = Format("psa_crypto_init failed: %d", (int)st);
        WSACleanup();
        return false;
    }
    g_httpInitialized = true;
    return true;
}

void HttpGlobalCleanup()
{
    if (!g_httpInitialized) return;
    mbedtls_psa_crypto_free();
    WSACleanup();
    g_httpInitialized = false;
}

static std::string MbedError(int rc)
{
    char buf[200];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return Format("%s (-0x%04X)", buf, (unsigned)-rc);
}

bool ParseHttpsUrl(const std::string& url, std::string& host, int& port, std::string& path)
{
    const std::string scheme = "https://";
    if (!StartsWith(ToUpperAscii(url.substr(0, scheme.size())), "HTTPS://")) return false;
    std::string rest = url.substr(scheme.size());
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    size_t colon = authority.rfind(':');
    port = 443;
    host = authority;
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        host = authority.substr(0, colon);
        port = atoi(authority.substr(colon + 1).c_str());
        if (port <= 0 || port > 65535) return false;
    }
    return !host.empty();
}

bool DecodeChunked(const std::string& in, std::string& out)
{
    out.clear();
    size_t p = 0;
    for (;;) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) return false;
        unsigned long size = strtoul(in.substr(p, eol - p).c_str(), NULL, 16); /* ignores ;extensions */
        p = eol + 2;
        if (size == 0) return true;
        if (p + size > in.size()) return false;
        out.append(in, p, size);
        p += size + 2; /* data + CRLF */
    }
}

std::string Base64Encode(const std::string& in)
{
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, (const unsigned char*)in.data(), in.size());
    std::string out(olen, '\0');
    if (mbedtls_base64_encode((unsigned char*)&out[0], out.size(), &olen, (const unsigned char*)in.data(), in.size()) != 0)
        return std::string();
    out.resize(olen);
    return out;
}

static int SendCb(void* ctx, const unsigned char* buf, size_t len)
{
    SOCKET s = *(SOCKET*)ctx;
    int n = send(s, (const char*)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (e == WSAECONNRESET || e == WSAECONNABORTED) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n;
}

static int RecvCb(void* ctx, unsigned char* buf, size_t len)
{
    SOCKET s = *(SOCKET*)ctx;
    int n = recv(s, (char*)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (e == WSAEWOULDBLOCK || e == WSAEINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        if (e == WSAECONNRESET || e == WSAECONNABORTED) return MBEDTLS_ERR_NET_CONN_RESET;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n;
}

static SOCKET ConnectTcp(const std::string& host, int port, int timeoutSeconds, std::string& err)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* list = NULL;
    int rc = getaddrinfo(host.c_str(), Format("%d", port).c_str(), &hints, &list);
    if (rc != 0) {
        err = "cannot resolve " + host + ": " + LastErrorText((unsigned long)rc);
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    int lastErr = 0;
    for (struct addrinfo* ai = list; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            lastErr = WSAGetLastError();
            continue;
        }
        DWORD ms = (DWORD)timeoutSeconds * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        lastErr = WSAGetLastError();
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(list);
    if (s == INVALID_SOCKET) err = Format("cannot connect to %s:%d: ", host.c_str(), port) + LastErrorText((unsigned long)lastErr);
    return s;
}

/* Finds a header value (case-insensitive name) in the raw header block. */
static bool FindHeader(const std::string& headers, const std::string& name, std::string& value)
{
    std::string upHeaders = ToUpperAscii(headers);
    std::string needle = "\r\n" + ToUpperAscii(name) + ":";
    size_t p = upHeaders.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    size_t e = headers.find("\r\n", p);
    value = Trim(headers.substr(p, e == std::string::npos ? std::string::npos : e - p));
    return true;
}

namespace {
/* Owns every mbedTLS object for one connection and frees them in the destructor. */
struct TlsSession {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    SOCKET sock;
    TlsSession() : sock(INVALID_SOCKET)
    {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
    }
    ~TlsSession()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }
};
}

static const size_t kMaxResponse = 8 * 1024 * 1024;

bool HttpsRequest(const std::string& url, const std::string& method, const std::vector<std::string>& headers,
                  const std::string& body, const TlsOptions& opt, HttpResponse& resp, std::string& err)
{
    resp = HttpResponse();
    std::string host, path;
    int port = 0;
    if (!ParseHttpsUrl(url, host, port, path)) {
        err = "not a valid https URL: " + url;
        return false;
    }

    TlsSession t;
    std::string caPem;
    if (!ReadWholeFile(opt.caFile, caPem) || caPem.empty()) {
        err = "cannot read CA file " + WideToUtf8(opt.caFile);
        return false;
    }
    int rc = mbedtls_x509_crt_parse(&t.ca, (const unsigned char*)caPem.c_str(), caPem.size() + 1);
    if (rc < 0) {
        err = "CA file has no usable certificates: " + MbedError(rc);
        return false;
    }
    if (rc > 0) LOG_WARN("%d certificate(s) in the CA file could not be parsed and were ignored", rc);

    const char* pers = "solasta-vista-sync";
    rc = mbedtls_ctr_drbg_seed(&t.drbg, mbedtls_entropy_func, &t.entropy, (const unsigned char*)pers, strlen(pers));
    if (rc != 0) {
        err = "random generator seed failed: " + MbedError(rc);
        return false;
    }
    rc = mbedtls_ssl_config_defaults(&t.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        err = "TLS config failed: " + MbedError(rc);
        return false;
    }
    mbedtls_ssl_conf_min_tls_version(&t.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&t.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t.conf, &t.ca, NULL);
    mbedtls_ssl_conf_rng(&t.conf, mbedtls_ctr_drbg_random, &t.drbg);
    if ((rc = mbedtls_ssl_setup(&t.ssl, &t.conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&t.ssl, host.c_str())) != 0) {
        err = "TLS setup failed: " + MbedError(rc);
        return false;
    }

    t.sock = ConnectTcp(host, port, opt.timeoutSeconds, err);
    if (t.sock == INVALID_SOCKET) return false;
    mbedtls_ssl_set_bio(&t.ssl, &t.sock, SendCb, RecvCb, NULL);

    DWORD start = GetTickCount();
    while ((rc = mbedtls_ssl_handshake(&t.ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        uint32_t flags = mbedtls_ssl_get_verify_result(&t.ssl);
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0) {
            char info[512];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            err = "server certificate rejected for " + host + ": " + Trim(info);
        } else {
            err = "TLS handshake with " + host + " failed: " + MbedError(rc);
        }
        return false;
    }
    LOG_DEBUG("TLS handshake with %s:%d took %lu ms (%s, %s)", host.c_str(), port, GetTickCount() - start,
              mbedtls_ssl_get_version(&t.ssl), mbedtls_ssl_get_ciphersuite(&t.ssl));

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + (port == 443 ? "" : Format(":%d", port)) +
                      "\r\nUser-Agent: SolastaVistaSync/1.0\r\nConnection: close\r\n";
    for (size_t i = 0; i < headers.size(); ++i) req += headers[i] + "\r\n";
    if (!body.empty() || method == "POST" || method == "PUT") req += Format("Content-Length: %u\r\n", (unsigned)body.size());
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        rc = mbedtls_ssl_write(&t.ssl, (const unsigned char*)req.data() + sent, req.size() - sent);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc < 0) {
            err = "sending request failed: " + MbedError(rc);
            return false;
        }
        sent += (size_t)rc;
    }

    std::string raw;
    size_t headerEnd = std::string::npos;
    long contentLength = -1;
    bool chunked = false;
    unsigned char buf[4096];
    for (;;) {
        rc = mbedtls_ssl_read(&t.ssl, buf, sizeof(buf));
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        if (rc == MBEDTLS_ERR_NET_CONN_RESET && headerEnd != std::string::npos && contentLength < 0 && !chunked) break;
        if (rc < 0) {
            err = "reading response failed: " + MbedError(rc);
            return false;
        }
        raw.append((const char*)buf, (size_t)rc);
        if (raw.size() > kMaxResponse) {
            err = "response too large";
            return false;
        }
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string h = raw.substr(0, headerEnd + 2), v;
                if (FindHeader(h, "Content-Length", v)) contentLength = atol(v.c_str());
                if (FindHeader(h, "Transfer-Encoding", v) && ToUpperAscii(v).find("CHUNKED") != std::string::npos) chunked = true;
            }
        }
        if (headerEnd != std::string::npos) {
            size_t bodyLen = raw.size() - headerEnd - 4;
            if (contentLength >= 0 && !chunked && bodyLen >= (size_t)contentLength) break;
            if (chunked && raw.size() >= 5 && raw.compare(raw.size() - 5, 5, "0\r\n\r\n") == 0) break;
        }
    }
    mbedtls_ssl_close_notify(&t.ssl);

    if (headerEnd == std::string::npos) {
        err = "incomplete HTTP response from " + host;
        return false;
    }
    resp.headers = raw.substr(0, headerEnd + 2);
    std::string rawBody = raw.substr(headerEnd + 4);
    if (chunked) {
        if (!DecodeChunked(rawBody, resp.body)) {
            err = "bad chunked response body";
            return false;
        }
    } else {
        resp.body = contentLength >= 0 && rawBody.size() > (size_t)contentLength ? rawBody.substr(0, contentLength) : rawBody;
    }
    /* Status line: HTTP/1.1 200 OK */
    size_t sp = resp.headers.find(' ');
    if (!StartsWith(resp.headers, "HTTP/") || sp == std::string::npos) {
        err = "malformed HTTP status line";
        return false;
    }
    resp.status = atoi(resp.headers.c_str() + sp + 1);
    return true;
}
