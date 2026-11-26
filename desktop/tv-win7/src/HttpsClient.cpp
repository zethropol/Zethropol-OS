#include "HttpsClient.h"
#include "StreamSecurity.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace zethropol::tvwin7 {

namespace {

std::string getOpenSslErrorText()
{
    const unsigned long err = ERR_get_error();
    if (err == 0) {
        return "TLS bağlantı hatası";
    }
    char buf[256] = {};
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

SOCKET connectTcpWithTimeout(const std::string &host, int port, int timeoutMs, std::string &errOut)
{
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string portStr = std::to_string(port);
    struct addrinfo *resList = nullptr;
    const int gai = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &resList);
    if (gai != 0 || !resList) {
        errOut = "Sunucu adresi çözümlenemedi (" + host + ").";
        return INVALID_SOCKET;
    }

    SOCKET connectedSock = INVALID_SOCKET;
    for (struct addrinfo *p = resList; p != nullptr; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }

        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);

        int rc = connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen));
        if (rc == SOCKET_ERROR) {
            const int wsaErr = WSAGetLastError();
            if (wsaErr != WSAEWOULDBLOCK && wsaErr != WSAEINPROGRESS) {
                closesocket(s);
                continue;
            }
            fd_set writeSet;
            fd_set errSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errSet);
            FD_SET(s, &writeSet);
            FD_SET(s, &errSet);

            struct timeval tv {};
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            const int sel = select(0, nullptr, &writeSet, &errSet, &tv);
            if (sel <= 0 || FD_ISSET(s, &errSet) || !FD_ISSET(s, &writeSet)) {
                closesocket(s);
                continue;
            }
            int soErr = 0;
            int soLen = sizeof(soErr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soErr), &soLen) != 0 || soErr != 0) {
                closesocket(s);
                continue;
            }
        }

        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);

        const DWORD rwTimeout = static_cast<DWORD>(std::max(timeoutMs, 2000));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&rwTimeout), sizeof(rwTimeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&rwTimeout), sizeof(rwTimeout));

        connectedSock = s;
        break;
    }

    freeaddrinfo(resList);
    if (connectedSock == INVALID_SOCKET) {
        errOut = "Sunucuya bağlantı zaman aşımına uğradı (" + host + ").";
    }
    return connectedSock;
}

bool decodeChunkedBody(const std::string &raw, std::size_t maxBytes, std::string &out, bool &overflow)
{
    out.clear();
    overflow = false;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            break;
        }
        std::string sizeHex = raw.substr(pos, lineEnd - pos);
        const std::size_t semi = sizeHex.find(';');
        if (semi != std::string::npos) {
            sizeHex.resize(semi);
        }
        sizeHex = StreamSecurity::trimAscii(sizeHex);
        if (sizeHex.empty()) {
            return false;
        }
        std::size_t chunkSize = 0;
        try {
            chunkSize = std::stoull(sizeHex, nullptr, 16);
        } catch (...) {
            return false;
        }
        pos = lineEnd + 2;
        if (chunkSize == 0) {
            return true;
        }
        if (out.size() + chunkSize > maxBytes) {
            overflow = true;
            return false;
        }
        if (pos + chunkSize > raw.size()) {
            // Partial trailing chunk — append what arrived
            out.append(raw.data() + pos, raw.size() - pos);
            return true;
        }
        out.append(raw.data() + pos, chunkSize);
        pos += chunkSize;
        if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') {
            pos += 2;
        }
    }
    return true;
}

} // namespace

HttpsClient::HttpsClient() = default;

HttpsClient::~HttpsClient()
{
    if (m_sslCtx) {
        SSL_CTX_free(static_cast<SSL_CTX *>(m_sslCtx));
        m_sslCtx = nullptr;
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
}

bool HttpsClient::initialize(const std::string &caBundlePathUtf8, std::string *errorOut)
{
    WSADATA wsaData {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        if (errorOut) *errorOut = "Winsock2 başlatılamadı.";
        return false;
    }
    m_wsaStarted = true;

    OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr);

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        if (errorOut) *errorOut = "OpenSSL TLS bağlamı oluşturulamadı: " + getOpenSslErrorText();
        return false;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

    if (caBundlePathUtf8.empty()
        || SSL_CTX_load_verify_locations(ctx, caBundlePathUtf8.c_str(), nullptr) != 1) {
        if (errorOut) {
            *errorOut = "Kök sertifika paketi (cacert.pem) yüklenemedi: " + getOpenSslErrorText();
        }
        SSL_CTX_free(ctx);
        return false;
    }

    m_sslCtx = ctx;
    m_initialized = true;
    return true;
}

HttpResponse HttpsClient::fetchSingleHop(const HttpRequest &request, std::string &redirectLocationOut) const
{
    HttpResponse resp;
    redirectLocationOut.clear();

    if (request.cancelFlag && request.cancelFlag->load()) {
        resp.canceled = true;
        return resp;
    }
    if (!m_initialized || !m_sslCtx) {
        resp.errorMessage = "HTTPS istemcisi hazır değil.";
        return resp;
    }

    const ParsedUrl parsed = StreamSecurity::parseHttpUrl(request.url);
    if (!parsed.valid) {
        resp.errorMessage = "Güvenlik engeli: Geçersiz URL.";
        return resp;
    }

    const int connectTimeout = std::min(request.timeoutMs, 6000);
    std::string connErr;
    SOCKET sock = connectTcpWithTimeout(parsed.host, parsed.port, connectTimeout, connErr);
    if (sock == INVALID_SOCKET) {
        resp.errorMessage = connErr;
        return resp;
    }

    const bool useTls = (parsed.scheme == "https");
    SSL *ssl = nullptr;

    auto cleanup = [&]() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    };

    if (useTls) {
        ssl = SSL_new(static_cast<SSL_CTX *>(m_sslCtx));
        if (!ssl) {
            resp.errorMessage = "OpenSSL oturumu oluşturulamadı.";
            cleanup();
            return resp;
        }
        SSL_set_tlsext_host_name(ssl, parsed.host.c_str());
        X509_VERIFY_PARAM *vparam = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(vparam, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        X509_VERIFY_PARAM_set1_host(vparam, parsed.host.c_str(), parsed.host.size());
        SSL_set_fd(ssl, static_cast<int>(sock));

        if (SSL_connect(ssl) != 1) {
            resp.errorMessage = "TLS el sıkışması başarısız: " + getOpenSslErrorText();
            cleanup();
            return resp;
        }
    }

    std::string hostHeader = parsed.host;
    if (parsed.explicitPort) {
        hostHeader += ":" + std::to_string(parsed.port);
    }

    const std::string ua = StreamSecurity::sanitizeUserAgent(request.userAgent, StreamResolver::kDesktopBrowserUa);
    std::ostringstream reqBuf;
    reqBuf << "GET " << parsed.pathAndQuery << " HTTP/1.1\r\n"
           << "Host: " << hostHeader << "\r\n"
           << "User-Agent: " << ua << "\r\n"
           << "Accept: text/html,application/xhtml+xml,application/vnd.apple.mpegurl,application/json;q=0.9,*/*;q=0.5\r\n"
           << "Accept-Language: tr-TR,tr;q=0.9,en;q=0.5\r\n"
           << "Accept-Encoding: identity\r\n"
           << "Connection: close\r\n";
    if (!request.referer.empty() && StreamSecurity::isAllowedMediaUrl(request.referer)) {
        reqBuf << "Referer: " << request.referer << "\r\n";
    }
    if (!request.origin.empty() && StreamSecurity::isAllowedMediaUrl(request.origin + "/")) {
        reqBuf << "Origin: " << request.origin << "\r\n";
    }
    if (!request.cookie.empty()) {
        reqBuf << "Cookie: " << request.cookie << "\r\n";
    }
    reqBuf << "\r\n";

    const std::string reqStr = reqBuf.str();
    std::size_t sentTotal = 0;
    while (sentTotal < reqStr.size()) {
        if (request.cancelFlag && request.cancelFlag->load()) {
            resp.canceled = true;
            cleanup();
            return resp;
        }
        const int chunk = static_cast<int>(reqStr.size() - sentTotal);
        const int n = useTls
            ? SSL_write(ssl, reqStr.data() + sentTotal, chunk)
            : send(sock, reqStr.data() + sentTotal, chunk, 0);
        if (n <= 0) {
            resp.errorMessage = "HTTP isteği gönderilemedi.";
            cleanup();
            return resp;
        }
        sentTotal += static_cast<std::size_t>(n);
    }

    std::string rawResp;
    rawResp.reserve(16384);
    const std::size_t hardLimit = request.maxBytes + 65536; // allow headers + body up to cap
    char buf[8192];

    while (true) {
        if (request.cancelFlag && request.cancelFlag->load()) {
            resp.canceled = true;
            cleanup();
            return resp;
        }
        const int n = useTls
            ? SSL_read(ssl, buf, sizeof(buf))
            : recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            if (rawResp.size() + static_cast<std::size_t>(n) > hardLimit) {
                resp.overflow = true;
                resp.errorMessage = "Yayın sayfası 2 MiB güvenlik sınırını aştı.";
                cleanup();
                return resp;
            }
            rawResp.append(buf, static_cast<std::size_t>(n));
        } else {
            break;
        }
    }

    cleanup();

    const std::size_t hdrEnd = rawResp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        resp.errorMessage = "Geçersiz veya eksik HTTP yanıtı.";
        return resp;
    }

    const std::string hdrBlock = rawResp.substr(0, hdrEnd);
    const std::string bodyRaw = rawResp.substr(hdrEnd + 4);

    // Parse status line: HTTP/1.1 200 OK
    const std::size_t firstLineEnd = hdrBlock.find("\r\n");
    const std::string statusLine = (firstLineEnd == std::string::npos) ? hdrBlock : hdrBlock.substr(0, firstLineEnd);
    const std::size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos || sp1 + 4 > statusLine.size()) {
        resp.errorMessage = "HTTP durum satırı okunamadı.";
        return resp;
    }
    try {
        resp.httpStatus = std::stoi(statusLine.substr(sp1 + 1, 3));
    } catch (...) {
        resp.errorMessage = "HTTP durum kodu geçersiz.";
        return resp;
    }

    bool isChunked = false;
    std::size_t pos = (firstLineEnd == std::string::npos) ? hdrBlock.size() : (firstLineEnd + 2);
    while (pos < hdrBlock.size()) {
        std::size_t nextEnd = hdrBlock.find("\r\n", pos);
        if (nextEnd == std::string::npos) {
            nextEnd = hdrBlock.size();
        }
        const std::string line = hdrBlock.substr(pos, nextEnd - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string nameLower = StreamSecurity::toLowerAscii(StreamSecurity::trimAscii(line.substr(0, colon)));
            const std::string val = StreamSecurity::trimAscii(line.substr(colon + 1));
            if (nameLower == "content-type") {
                resp.contentType = val;
            } else if (nameLower == "location") {
                redirectLocationOut = val;
            } else if (nameLower == "transfer-encoding"
                       && StreamSecurity::toLowerAscii(val).find("chunked") != std::string::npos) {
                isChunked = true;
            }
        }
        pos = nextEnd + 2;
    }

    if (resp.httpStatus == 301 || resp.httpStatus == 302 || resp.httpStatus == 303
        || resp.httpStatus == 307 || resp.httpStatus == 308) {
        resp.finalUrl = request.url;
        return resp;
    }

    if (resp.httpStatus < 200 || resp.httpStatus >= 300) {
        resp.errorMessage = "HTTP hata kodu: " + std::to_string(resp.httpStatus);
        resp.finalUrl = request.url;
        return resp;
    }

    if (isChunked) {
        bool chunkOverflow = false;
        if (!decodeChunkedBody(bodyRaw, request.maxBytes, resp.body, chunkOverflow)) {
            if (chunkOverflow) {
                resp.overflow = true;
                resp.errorMessage = "Yayın sayfası 2 MiB güvenlik sınırını aştı.";
                return resp;
            }
            resp.body = bodyRaw.substr(0, std::min(bodyRaw.size(), request.maxBytes));
        }
    } else {
        if (bodyRaw.size() > request.maxBytes) {
            resp.overflow = true;
            resp.errorMessage = "Yayın sayfası 2 MiB güvenlik sınırını aştı.";
            return resp;
        }
        resp.body = bodyRaw;
    }

    resp.ok = true;
    resp.finalUrl = request.url;
    return resp;
}

HttpResponse HttpsClient::fetch(const HttpRequest &request) const
{
    HttpRequest current = request;
    for (int hop = 0; hop <= StreamSecurity::kMaxRedirects; ++hop) {
        std::string reason;
        if (!StreamSecurity::isAllowedMediaUrl(current.url, &reason)) {
            HttpResponse err;
            err.errorMessage = "Güvenlik engeli: " + reason;
            return err;
        }
        std::string location;
        HttpResponse resp = fetchSingleHop(current, location);
        if (resp.canceled || resp.overflow) {
            return resp;
        }
        if ((resp.httpStatus == 301 || resp.httpStatus == 302 || resp.httpStatus == 303
             || resp.httpStatus == 307 || resp.httpStatus == 308)
            && !location.empty()) {
            if (hop == StreamSecurity::kMaxRedirects) {
                HttpResponse err;
                err.errorMessage = "Çok fazla yönlendirme nedeniyle akış çözümlenemedi.";
                return err;
            }
            const ParsedUrl base = StreamSecurity::parseHttpUrl(current.url);
            const std::string nextUrl = StreamSecurity::resolveRelativeUrl(location, base);
            if (nextUrl.empty()) {
                HttpResponse err;
                err.errorMessage = "Güvensiz yönlendirme engellendi.";
                return err;
            }
            current.url = nextUrl;
            continue;
        }
        return resp;
    }
    HttpResponse err;
    err.errorMessage = "Yönlendirme sınırı aşıldı.";
    return err;
}

} // namespace zethropol::tvwin7
