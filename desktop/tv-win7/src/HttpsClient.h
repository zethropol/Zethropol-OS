#pragma once

#include "StreamResolver.h"

#include <string>

namespace zethropol::tvwin7 {

class HttpsClient
{
public:
    HttpsClient();
    ~HttpsClient();

    HttpsClient(const HttpsClient &) = delete;
    HttpsClient &operator=(const HttpsClient &) = delete;

    bool initialize(const std::string &caBundlePathUtf8, std::string *errorOut = nullptr);
    HttpResponse fetch(const HttpRequest &request) const;

    bool isReady() const { return m_initialized; }

private:
    HttpResponse fetchSingleHop(const HttpRequest &request, std::string &redirectLocationOut) const;

    void *m_sslCtx = nullptr; // SSL_CTX*
    bool m_wsaStarted = false;
    bool m_initialized = false;
};

} // namespace zethropol::tvwin7
