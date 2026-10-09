/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later */
/* The scraper's HTTP client (src/scraper.h), which the download sources use too
 * (src/remote/: their servers want a sign-in, a body, part of a file).
 *
 * On the console, Sony's classic HTTP library (HTTP/1.1, which every media host
 * speaks; its HTTP/2 library failed every request to thumbnails.libretro.com) over its
 * SSL: one library context for the process, one template per worker, one kept-alive
 * connection a host, so each worker reuses its connection across thousands of small
 * requests instead of connecting for each. The platform's certificate and hostname
 * checks stay on.
 *
 * On the host (the tests), plain http:// over a socket, enough for the fake servers
 * tests/test_scraper.py plays. */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ps5_scraper
{
struct Response
{
    int status = 0;     /* the HTTP status, 0 when none arrived */
    std::string body;   /* up to the limit, when no file was given */
    uint64_t bytes = 0; /* the body's length */
    bool cancelled = false, too_large = false;
    std::string error; /* why there is no status */
};

/* A request of any method, its answer kept or streamed. */
struct Request
{
    std::string method = "GET"; /* GET, HEAD, POST, PUT, DELETE */
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers; /* "Authorization", "Range"... */
    std::string body;           /* sent as it is; its "Content-Type" is among the headers */
    uint64_t limit = 64u << 20; /* the most of the body kept, when no sink takes it */
    const std::atomic<bool> *cancel = nullptr;
    std::function<bool()> stopped; /* asked between pieces too: true stops the transfer */
    /* Told the status before the body's first byte: false refuses the body. */
    std::function<bool(int status)> begin;
    /* Takes the body piece by piece instead of keeping it: false stops. */
    std::function<bool(const char *data, size_t size)> sink;
    unsigned timeout = 0; /* seconds for the connection and for each piece; 0: 15 and 30 */
};

class Http
{
  public:
    explicit Http(const char *agent);
    ~Http();
    Http(const Http &) = delete;
    Http &operator=(const Http &) = delete;
    /* GET url: the body kept (at most limit bytes), or written to fd when fd >= 0.
     * cancel, when set, stops the transfer between reads. */
    Response get(const std::string &url, uint64_t limit, const std::atomic<bool> *cancel,
                 int fd = -1, std::function<void(uint64_t)> progress = {});
    /* HEAD url: the status alone, no body (whether a source has a file). */
    Response head(const std::string &url, const std::atomic<bool> *cancel);
    /* Any request: the response's body as request.sink takes it, or kept. cancelled is
     * set when cancel, stopped, begin or sink ended it. */
    Response send(const Request &request);

  private:
    Response request(const char *method, const std::string &url, uint64_t limit,
                     const std::atomic<bool> *cancel, int fd,
                     const std::function<void(uint64_t)> &progress = {});
    Response perform(const Request &request, int fd, const std::function<void(uint64_t)> &progress);
    struct State;
    std::unique_ptr<State> state_;
};

/* The certificates HTTPS is verified against (the daemon's curl build; a no-op where
 * the platform's own store is used). */
void set_certificates(const std::string &path);
/* %-encodes everything but unreserved characters (RFC 3986). */
std::string url_encode(const std::string &text);
/* Writes a URL with its query's credentials masked, for logs and status. */
std::string redact(const std::string &url);
/* The Authorization header's value for a user name and password: "Basic <base64>". */
std::string basic_authorization(const std::string &user, const std::string &password);
/* A form (multipart/form-data) of one file as a request's body; *content_type is its
 * Content-Type header, with the boundary. */
std::string form_file(const std::string &field, const std::string &file_name,
                      const std::string &data, std::string *content_type);
} // namespace ps5_scraper
