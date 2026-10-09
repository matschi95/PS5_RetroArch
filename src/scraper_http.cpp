/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later */
/* The scraper's HTTP client (src/scraper_http.h). */
#include "scraper_http.h"

#if defined(PS5_SCRAPER_CURL)
#include <curl/curl.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <vector>

#if defined(__PROSPERO__) && !defined(PS5_SCRAPER_CURL)
extern "C"
{
    int sceNetInit();
    int sceNetPoolCreate(const char *, int, int);
    int sceSslInit(size_t);
    int sceHttpInit(int, int, size_t);
    int sceHttpCreateTemplate(int, const char *, int, int);
    int sceHttpDeleteTemplate(int);
    int sceHttpCreateConnectionWithURL(int, const char *, int);
    int sceHttpDeleteConnection(int);
    int sceHttpCreateRequestWithURL(int, int, const char *, uint64_t);
    int sceHttpDeleteRequest(int);
    int sceHttpSendRequest(int, const void *, size_t);
    int sceHttpGetStatusCode(int, int *);
    int sceHttpReadData(int, void *, size_t);
    int sceHttpSetAutoRedirect(int, int);
    int sceHttpSetConnectTimeOut(int, unsigned);
    int sceHttpSetRecvTimeOut(int, unsigned);
    int sceHttpAddRequestHeader(int, const char *, const char *, uint32_t);
}
#include <map>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace ps5_scraper
{
namespace
{
bool write_all(int fd, const char *data, size_t size)
{
    while (size)
    {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}
/* Takes a piece of the body: to the request's sink, written to fd, or kept. */
bool take(Response &response, const Request &request, const char *data, size_t size, int fd)
{
    if (request.sink)
    {
        response.bytes += size;
        if (!request.sink(data, size))
        {
            response.cancelled = true;
            return false;
        }
        return true;
    }
    if (response.bytes + size > request.limit)
    {
        response.too_large = true;
        return false;
    }
    response.bytes += size;
    if (fd >= 0)
        return write_all(fd, data, size);
    response.body.append(data, size);
    return true;
}
/* Whether the request is to stop now. */
bool stopping(const Request &request)
{
    return (request.cancel && request.cancel->load()) || (request.stopped && request.stopped());
}
/* The status arrived: whether the body is wanted. */
bool begun(Response &response, const Request &request)
{
    if (request.begin && !request.begin(response.status))
    {
        response.cancelled = true;
        return false;
    }
    return true;
}
#if defined(__PROSPERO__) && !defined(PS5_SCRAPER_CURL)
/* One library context for the process: the net pool, SSL and HTTP, never torn down.
 * Sony's classic HTTP library (HTTP/1.1, keep-alive), not its HTTP/2 one: media hosts
 * such as thumbnails.libretro.com answer HTTP/1.1 only. */
int library_context()
{
    static std::once_flag once;
    static int http = -1;
    std::call_once(once,
                   []
                   {
                       (void)sceNetInit();
                       const int pool = sceNetPoolCreate("PS5 RetroArch scraper", 4 << 20, 0);
                       const int ssl = pool >= 0 ? sceSslInit(2 << 20) : -1;
                       http = ssl >= 0 ? sceHttpInit(pool, ssl, 4 << 20) : -1;
                   });
    return http;
}
std::string code(const char *call, int result)
{
    char text[64];
    std::snprintf(text, sizeof text, "%s failed (0x%08x).", call, unsigned(result));
    return text;
}
/* scheme://host[:port], the key of a kept-alive connection. */
std::string origin(const std::string &url)
{
    const size_t start = url.find("://");
    const size_t end = start == std::string::npos ? url.size() : url.find('/', start + 3);
    return url.substr(0, end);
}
#endif
} // namespace

#if defined(PS5_SCRAPER_CURL)
/* libcurl over mbedTLS (tools/build-curl.sh): the daemon's HTTP and HTTPS. Sony's SSL
 * failed every handshake from the payload (sceHttpSendRequest 0x8095f00c, 2026-10-07);
 * curl verifies against the bundled certificates (set_certificates). One easy handle a
 * worker: its connections are kept alive across requests to the same host. */
namespace
{
std::string certificates;
struct Sink
{
    Response *response;
    const Request *request;
    int fd;
    const std::function<void(uint64_t)> *progress;
    CURL *curl;
    bool begun;
};
size_t on_data(char *data, size_t size, size_t count, void *context)
{
    auto *sink = static_cast<Sink *>(context);
    const size_t bytes = size * count;
    if (stopping(*sink->request))
    {
        sink->response->cancelled = true;
        return 0;
    }
    if (!sink->begun)
    {
        sink->begun = true;
        long status = 0;
        curl_easy_getinfo(sink->curl, CURLINFO_RESPONSE_CODE, &status);
        sink->response->status = int(status);
        if (!begun(*sink->response, *sink->request))
            return 0;
    }
    if (!take(*sink->response, *sink->request, data, bytes, sink->fd))
        return 0;
    if (*sink->progress)
        (*sink->progress)(sink->response->bytes);
    return bytes;
}
int on_progress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto *sink = static_cast<Sink *>(context);
    if (stopping(*sink->request))
    {
        sink->response->cancelled = true;
        return 1;
    }
    return 0;
}
} // namespace

void set_certificates(const std::string &path)
{
    certificates = path;
}

struct Http::State
{
    std::string agent;
    CURL *handle = nullptr;
};

Http::Http(const char *agent) : state_(new State)
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    state_->agent = agent;
    state_->handle = curl_easy_init();
}

Http::~Http()
{
    if (state_->handle)
        curl_easy_cleanup(state_->handle);
}

Response Http::get(const std::string &url, uint64_t limit, const std::atomic<bool> *cancel, int fd,
                   std::function<void(uint64_t)> progress)
{
    return request("GET", url, limit, cancel, fd, progress);
}

Response Http::head(const std::string &url, const std::atomic<bool> *cancel)
{
    return request("HEAD", url, 0, cancel, -1);
}

Response Http::request(const char *method, const std::string &url, uint64_t limit,
                       const std::atomic<bool> *cancel, int fd,
                       const std::function<void(uint64_t)> &progress)
{
    Request request;
    request.method = method;
    request.url = url;
    request.limit = limit;
    request.cancel = cancel;
    return perform(request, fd, progress);
}

Response Http::send(const Request &request)
{
    return perform(request, -1, {});
}

Response Http::perform(const Request &request, int fd,
                       const std::function<void(uint64_t)> &progress)
{
    Response response;
    CURL *curl = state_->handle;
    if (!curl)
    {
        response.error = "The HTTP library did not start.";
        return response;
    }
    Sink sink{&response, &request, fd, &progress, curl, false};
    const bool head = request.method == "HEAD";
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, state_->agent.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, head ? 1L : 0L);
    if (request.method == "GET")
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    else if (request.method == "POST" || request.method == "PUT")
    {
        /* A POST stays one after a redirect, its body sent again. */
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(request.body.size()));
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTREDIR, long(CURL_REDIR_POST_ALL));
        if (request.method == "PUT")
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    else if (!head)
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    struct curl_slist *headers = nullptr;
    for (const auto &header : request.headers)
        headers = curl_slist_append(headers, (header.first + ": " + header.second).c_str());
    headers = curl_slist_append(headers, "Expect:"); /* no 100-continue round trip first */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* An https request is never redirected to http: its address may carry an account. */
    if (request.url.rfind("https://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, long(request.timeout ? request.timeout : 15));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, long(request.timeout ? request.timeout : 30));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    if (!certificates.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, certificates.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    const CURLcode result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = int(status);
    /* An answer without a body: its status is told all the same. */
    if (result == CURLE_OK && !sink.begun)
        (void)begun(response, request);
    if (result != CURLE_OK && !response.cancelled && !response.too_large)
    {
        char text[160];
        std::snprintf(text, sizeof text, "%s (curl %d).", curl_easy_strerror(result), int(result));
        response.error = text;
        if (result != CURLE_WRITE_ERROR)
            response.status = 0; /* no answer we can use */
    }
    return response;
}

#else
void set_certificates(const std::string &)
{
}
struct Http::State
{
    std::string agent;
#ifdef __PROSPERO__
    int templ = -1;
    std::map<std::string, int> connections; /* origin -> kept-alive connection */
#endif
};

Http::Http(const char *agent) : state_(new State)
{
    state_->agent = agent;
#ifdef __PROSPERO__
    const int http = library_context();
    if (http >= 0)
    {
        state_->templ = sceHttpCreateTemplate(http, agent, 2 /* HTTP/1.1 */, 1);
        if (state_->templ >= 0)
        {
            sceHttpSetAutoRedirect(state_->templ, 1);
            sceHttpSetConnectTimeOut(state_->templ, 15000000);
            sceHttpSetRecvTimeOut(state_->templ, 30000000);
        }
    }
#endif
}

Http::~Http()
{
#ifdef __PROSPERO__
    for (const auto &connection : state_->connections)
        sceHttpDeleteConnection(connection.second);
    if (state_->templ >= 0)
        sceHttpDeleteTemplate(state_->templ);
#endif
}

Response Http::get(const std::string &url, uint64_t limit, const std::atomic<bool> *cancel, int fd,
                   std::function<void(uint64_t)> progress)
{
    return request("GET", url, limit, cancel, fd, progress);
}

Response Http::head(const std::string &url, const std::atomic<bool> *cancel)
{
    return request("HEAD", url, 0, cancel, -1);
}

Response Http::request(const char *method, const std::string &url, uint64_t limit,
                       const std::atomic<bool> *cancel, int fd,
                       const std::function<void(uint64_t)> &progress)
{
    Request request;
    request.method = method;
    request.url = url;
    request.limit = limit;
    request.cancel = cancel;
    return perform(request, fd, progress);
}

Response Http::send(const Request &request)
{
    return perform(request, -1, {});
}

Response Http::perform(const Request &request, int fd,
                       const std::function<void(uint64_t)> &progress)
{
    Response response;
    const bool head = request.method == "HEAD";
#ifdef __PROSPERO__
    if (state_->templ < 0)
    {
        response.error = "The console's network library did not start.";
        return response;
    }
    /* The library's methods: GET 0, POST 1, HEAD 2, PUT 4, DELETE 5. */
    const int method = request.method == "POST"     ? 1
                       : head                       ? 2
                       : request.method == "PUT"    ? 4
                       : request.method == "DELETE" ? 5
                                                    : 0;
    // One kept-alive connection a host a worker; a request that fails on it is tried
    // once more on a fresh one (the server may have closed it meanwhile).
    const std::string host = origin(request.url);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        int &connection = state_->connections[host];
        if (connection <= 0)
            connection = sceHttpCreateConnectionWithURL(state_->templ, request.url.c_str(), 1);
        if (connection < 0)
        {
            response.error = code("sceHttpCreateConnectionWithURL", connection);
            state_->connections.erase(host);
            return response;
        }
        const int id = sceHttpCreateRequestWithURL(connection, method, request.url.c_str(),
                                                   uint64_t(request.body.size()));
        if (id < 0)
        {
            response.error = code("sceHttpCreateRequestWithURL", id);
            return response;
        }
        int result = 0;
        for (const auto &header : request.headers)
            if (result >= 0)
                result = sceHttpAddRequestHeader(id, header.first.c_str(), header.second.c_str(),
                                                 0 /* overwrite */);
        if (result >= 0 && request.timeout)
        {
            sceHttpSetConnectTimeOut(id, request.timeout * 1000000u);
            sceHttpSetRecvTimeOut(id, request.timeout * 1000000u);
        }
        if (result >= 0)
            result = sceHttpSendRequest(id, request.body.empty() ? nullptr : request.body.data(),
                                        request.body.size());
        if (result >= 0)
            result = sceHttpGetStatusCode(id, &response.status);
        if (result < 0)
        {
            response.status = 0;
            response.error = code("sceHttpSendRequest", result);
            sceHttpDeleteRequest(id);
            sceHttpDeleteConnection(connection);
            state_->connections.erase(host);
            continue;
        }
        // On the heap: a payload's threads have small stacks (a 64 KiB frame overflowed
        // one, 2026-10-07: SIGSEGV writing just below the stack).
        std::vector<char> buffer(65536);
        int n = 0;
        if (begun(response, request) && !head)
            while ((n = sceHttpReadData(id, buffer.data(), buffer.size())) > 0)
            {
                if (stopping(request))
                {
                    response.cancelled = true;
                    break;
                }
                if (!take(response, request, buffer.data(), size_t(n), fd))
                    break;
                if (progress)
                    progress(response.bytes);
            }
        if (n < 0 && !response.cancelled && !response.too_large)
            response.error = code("sceHttpReadData", n);
        sceHttpDeleteRequest(id);
        if (response.cancelled || response.too_large || n < 0)
        {
            /* Unread data left on it: the connection is not reused. */
            sceHttpDeleteConnection(connection);
            state_->connections.erase(host);
        }
        return response;
    }
    return response;
#else
    /* http://host[:port]/path, for the tests' fake servers and a server in Docker. */
    if (request.url.rfind("http://", 0) != 0)
    {
        response.error = "Only http:// on the host.";
        return response;
    }
    const std::string &url = request.url;
    const size_t host_start = 7, path_start = url.find('/', host_start);
    std::string host = url.substr(host_start, path_start - host_start), port = "80";
    const std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
    const std::string host_header = host;
    if (const size_t colon = host.find(':'); colon != std::string::npos)
    {
        port = host.substr(colon + 1);
        host.resize(colon);
    }
    addrinfo hints{}, *found = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) || !found)
    {
        response.error = "The server's name did not resolve.";
        return response;
    }
    const int sock = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    timeval timeout{long(request.timeout ? request.timeout : 30), 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    const bool connected = sock >= 0 && connect(sock, found->ai_addr, found->ai_addrlen) == 0;
    freeaddrinfo(found);
    if (!connected)
    {
        if (sock >= 0)
            close(sock);
        response.error = "The server did not answer (network or DNS).";
        return response;
    }
    std::string head_lines = request.method + ' ' + path + " HTTP/1.0\r\nHost: " + host_header +
                             "\r\nUser-Agent: " + state_->agent + "\r\n";
    for (const auto &header : request.headers)
        head_lines += header.first + ": " + header.second + "\r\n";
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT")
        head_lines += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    head_lines += "\r\n";
    write_all(sock, head_lines.data(), head_lines.size());
    write_all(sock, request.body.data(), request.body.size());
    std::string answer_head;
    std::vector<char> storage(65536);
    char *buffer = storage.data();
    bool in_body = false;
    ssize_t n;
    while ((n = read(sock, buffer, storage.size())) > 0)
    {
        if (stopping(request))
        {
            response.cancelled = true;
            break;
        }
        if (in_body)
        {
            if (!take(response, request, buffer, size_t(n), fd))
                break;
            if (progress)
                progress(response.bytes);
            continue;
        }
        answer_head.append(buffer, size_t(n));
        const size_t end = answer_head.find("\r\n\r\n");
        if (end == std::string::npos)
            continue;
        response.status = std::atoi(answer_head.c_str() + answer_head.find(' ') + 1);
        in_body = true;
        if (!begun(response, request) || head)
            break;
        if (end + 4 < answer_head.size() && !take(response, request, answer_head.data() + end + 4,
                                                  answer_head.size() - end - 4, fd))
            break;
    }
    close(sock);
    if (!in_body && response.error.empty() && !response.cancelled)
        response.error = "The server's answer was cut short.";
    return response;
#endif
}

#endif

std::string url_encode(const std::string &text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string redact(const std::string &url)
{
    std::string out = url;
    for (const char *key : {"devpassword=", "sspassword=", "devid=", "ssid="})
    {
        size_t at = 0;
        while ((at = out.find(key, at)) != std::string::npos)
        {
            if (at > 0 && out[at - 1] != '?' && out[at - 1] != '&')
            {
                at += std::strlen(key);
                continue;
            }
            const size_t start = at + std::strlen(key), end = out.find('&', start);
            out.replace(start, (end == std::string::npos ? out.size() : end) - start, "***");
            at = start + 3;
        }
    }
    return out;
}
std::string basic_authorization(const std::string &user, const std::string &password)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string plain = user + ":" + password;
    std::string out = "Basic ";
    for (size_t i = 0; i < plain.size(); i += 3)
    {
        const size_t left = plain.size() - i;
        const unsigned bits = unsigned((unsigned char)plain[i]) << 16 |
                              (left > 1 ? unsigned((unsigned char)plain[i + 1]) << 8 : 0) |
                              (left > 2 ? unsigned((unsigned char)plain[i + 2]) : 0);
        out += digits[bits >> 18 & 63];
        out += digits[bits >> 12 & 63];
        out += left > 1 ? digits[bits >> 6 & 63] : '=';
        out += left > 2 ? digits[bits & 63] : '=';
    }
    return out;
}

std::string form_file(const std::string &field, const std::string &file_name,
                      const std::string &data, std::string *content_type)
{
    /* A boundary the file does not hold. */
    std::string boundary = "ps5-retroarch-form";
    for (unsigned n = 0; data.find(boundary) != std::string::npos; n++)
        boundary = "ps5-retroarch-form-" + std::to_string(n);
    std::string name;
    for (char c : file_name)
        name += c == '"' || c == '\r' || c == '\n' ? '_' : c;
    *content_type = "multipart/form-data; boundary=" + boundary;
    return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + field +
           "\"; filename=\"" + name + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
           data + "\r\n--" + boundary + "--\r\n";
}
} // namespace ps5_scraper
