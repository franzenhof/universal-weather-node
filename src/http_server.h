#pragma once
// The node's HTTP server on ESP-IDF's esp_http_server, with the part of the
// Arduino WebServer API that web_api.cpp uses (on, arg, header, send,
// sendHeader, setContentLength + sendContent, authenticate, upload ...), so
// the page handlers stay as they are. What it adds over WebServer:
//   - several connections open at once (keep-alive, LRU purge), its own task
//   - limits: request headers 2048 B, URI 1024 B (sdkconfig), form bodies
//     HTTP_FORM_MAX, at most HTTP_ARGS_MAX arguments in HTTP_ARG_BYTES_MAX,
//     multipart bodies HTTP_FORM_MAX unless the route takes an upload, a
//     time limit per body - an oversized request is refused (413 / 408)
//     before any handler or login check runs, not a crash
//   - Digest login with several valid nonces (no re-challenge of parallel
//     requests), each nonce count accepted once (no replay of a captured
//     request), and a brake on wrong passwords per client (429, no sleep)
//   - multipart uploads streamed through http_util.h (firmware, restore)
// Handlers run one after the other on the server's task (one request at a
// time), so the per-request state below is plain members.
//
// The WiFiManager config portal keeps the Arduino WebServer (its own
// server): helpers used by both are templates (gz_send.h, portal_pages.h).
//
// HTTPS (optional, https_cert.h): beginTls() adds a second listener on the
// same routes through esp_https_server - its own server task, the TLS
// handshake in that task, the mbedTLS buffers in the PSRAM. The handlers of
// both listeners are serialized by one lock (reqLock_), so the per-request
// state below stays plain members and the handlers need not know which
// listener a request came on (requestTls() tells, for the port-80 redirect).
#include <Arduino.h>
// both define HTTP_ANY (esp_http_server: INT_MAX, WebServer: 255); the
// WebServer value is the one handlers pass (on() translates it) - kept
// whichever header a file includes first
#pragma push_macro("HTTP_ANY")
#undef HTTP_ANY
#include <esp_http_server.h>
#undef HTTP_ANY
#pragma pop_macro("HTTP_ANY")
#include <WebServer.h>   // HTTPMethod, HTTPUpload, HTTPAuthMethod, CONTENT_LENGTH_*
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <functional>
#include <vector>
#include "http_util.h"

constexpr size_t HTTP_FORM_MAX = 128 * 1024;   // urlencoded / JSON / multipart bodies (PSRAM); uploads stream
// Arguments of one request (query + form): the settings form, the largest,
// has well under a thousand fields. Their bytes in one buffer (PSRAM once
// >= 4 KB): every body that fits HTTP_FORM_MAX, plus the terminators.
constexpr size_t HTTP_ARGS_MAX = 2048;
constexpr size_t HTTP_ARG_BYTES_MAX = HTTP_FORM_MAX + 2 * HTTP_ARGS_MAX;
// Time to receive a whole body: a form, and a route that takes an upload
// (a firmware image of ~2 MB over a slow Wi-Fi). Both listeners share one
// lock, so a client trickling a body holds HTTPS too until then.
constexpr uint32_t HTTP_BODY_MAX_MS   = 30000;
constexpr uint32_t HTTP_UPLOAD_MAX_MS = 300000;

class HttpServer {
public:
    typedef std::function<void()> THandler;

    explicit HttpServer(uint16_t port);
    // maxSockets: open client connections (keep-alive, LRU purge)
    void begin(int maxSockets = 7);
    // HTTPS listener on `port` with the PEM pair (0-terminated, lengths
    // without the 0; copied by the server). ESP_OK or why it did not start.
    esp_err_t beginTls(uint16_t port, const char* cert, size_t certLen, const char* key, size_t keyLen, int maxSockets);
    void stop();   // both listeners
    bool running() const { return plain_.h != nullptr; }
    bool tlsRunning() const { return tls_.h != nullptr; }
    // The current request came over the HTTPS listener.
    bool requestTls() const { return req_ && tls_.h && req_->handle == tls_.h; }

    void on(const char* uri, HTTPMethod method, THandler fn) { on(uri, method, fn, nullptr); }
    void on(const char* uri, HTTPMethod method, THandler fn, THandler upload);
    void onNotFound(THandler fn) { notFound_ = fn; }
    // Runs before every route (and the not-found handler), before the body is
    // read (head and query only); true = it answered the request, the route
    // is skipped and an unread body closes the connection (the own access
    // point's captive redirects, the port-80 redirect to HTTPS, web_api.cpp).
    typedef bool (*PreRoute)();
    void onPreRoute(PreRoute fn) { preRoute_ = fn; }

    // ---- the current request ----
    String arg(const char* name) const { const char* v = args_.find(name); return v ? String(v) : String(); }
    String arg(const String& name) const { return arg(name.c_str()); }
    String arg(int i) const { const char* v = i >= 0 ? args_.value((size_t)i) : nullptr; return v ? String(v) : String(); }
    bool   hasArg(const char* name) const { return args_.find(name) != nullptr; }
    bool   hasArg(const String& name) const { return hasArg(name.c_str()); }
    int    args() const { return (int)args_.count(); }
    String argName(int i) const { const char* n = i >= 0 ? args_.name((size_t)i) : nullptr; return n ? String(n) : String(); }
    String header(const char* name) const;
    String header(const String& name) const { return header(name.c_str()); }
    String hostHeader() const { return header("Host"); }
    String uri() const;
    // The URI as requested, query included (redirects).
    const char* rawUri() const { return req_ ? req_->uri : ""; }
    HTTPMethod method() const;
    // The local IPv4 address the request came in on (also from an IPv4-mapped
    // IPv6 socket): tells the own access point (192.168.4.1) from the
    // station LAN. false = unknown.
    bool localIPv4(uint8_t out[4]) const;
    HTTPUpload& upload() { return upload_; }

    // ---- the response ----
    void sendHeader(const String& name, const String& value, bool first = false);
    void setContentLength(size_t len) { contentLength_ = len; }
    void send(int code, const char* type = nullptr, const String& content = String());
    void send(int code, const String& type, const String& content) { send(code, type.c_str(), content); }
    void send(int code, const char* type, const char* content);
    void send_P(int code, const char* type, const char* data, size_t len);
    void send_P(int code, const char* type, const char* data) { send_P(code, type, data, strlen(data)); }
    // false = the chunk did not go out (client gone): the response is
    // aborted - no closing chunk, the connection is closed
    bool sendContent(const char* data, size_t len);
    bool sendContent(const String& s) { return sendContent(s.c_str(), s.length()); }
    // Ends a started response as failed: no closing chunk (a chunked body
    // cut short never looks complete), the connection is closed.
    void abortResponse() { aborted_ = true; }

    // ---- Server-Sent Events (text/event-stream) ----
    // In a GET handler: answer with the event-stream headers and keep the
    // connection as a subscriber; false = SSE_MAX subscribers already (the
    // handler then answers 503). Subscribers are dropped when they close, or
    // when a send fails or would block (a slow client never stalls the server).
    // Over HTTPS at most SSE_MAX_TLS: each holds a TLS session (~33 KB PSRAM)
    // and one of the few HTTPS connections. A TLS send cannot be non-blocking
    // (esp_https_server ignores MSG_DONTWAIT): a subscriber whose socket is
    // not writable is dropped, a send that blocks anyway ends after the
    // listener's send timeout (5 s).
    static constexpr int SSE_MAX = 3;
    static constexpr int SSE_MAX_TLS = 2;
    bool sseAccept();
    int  sseClients() const { return plain_.sseCount + tls_.sseCount; }
    // From any task: build(out) runs on the server's task (like a handler, so it
    // may read what handlers read) and its text goes to every subscriber as one
    // "data:" event; nullptr sends a keep-alive "ping" event. false = not queued.
    typedef void (*SseBuild)(String& out);
    bool ssePublish(SseBuild build);

    // ---- login ----
    // true = this request carries a valid Digest login for user / password.
    // Checked once per request (a second call returns the first result: the
    // nonce count is spent by then). No Authorization header = false without
    // counting as a wrong login.
    bool authenticate(const char* user, const char* password);
    // After authenticate() said false: 401 with a new nonce (stale=TRUE when
    // only the nonce was old or replayed), or 429 + Retry-After while this
    // client must wait after wrong logins.
    void requestAuthentication(HTTPAuthMethod mode, const char* realm);
    // The request carries an Authorization header (an optional login, e.g.
    // /api/info's position for the admin, is only checked then).
    bool hasCredentials() const { return req_ && httpd_req_get_hdr_value_len(req_, "Authorization") > 0; }

private:
    struct Route { String uri; HTTPMethod method; THandler fn, upload; };
    // Idle keep-alive connections are closed while SSE subscribers exist
    // (closeIdle()): an SSE socket never receives, so the server's LRU purge
    // would pick it first whenever browsers park idle connections in the
    // free slots. Last request time per socket.
    static constexpr int ACT_MAX = 8;
    static constexpr uint32_t IDLE_CLOSE_MS = 15000;
    struct Act { int fd; uint32_t ms; };
    // One listening server (plain or TLS). Its SSE subscribers and activity
    // list are only touched on its own server task.
    struct Listener {
        httpd_handle_t h = nullptr;
        int sseMax = SSE_MAX;
        int sseFd[SSE_MAX] = { -1, -1, -1 };
        volatile int sseCount = 0;
        Act act[ACT_MAX] = {};
    };
    static esp_err_t dispatch(httpd_req_t* r);
    static esp_err_t notFoundCb(httpd_req_t* r, httpd_err_code_t err);
    httpd_config_t baseConfig(uint16_t port, int maxSockets) const;
    void registerRoutes(httpd_handle_t h);
    Listener* listenerOf(httpd_handle_t h) { return h && h == plain_.h ? &plain_ : h && h == tls_.h ? &tls_ : nullptr; }
    static void resetListener(Listener& l);
    void beginRequest(httpd_req_t* r);
    bool readBody(const Route* route);
    bool finishRequest();   // false = the response was aborted (abortResponse())
    void startResponse(int code, const char* type, bool chunked);
    bool clientAddress(uint8_t out[16]) const;   // IPv6 or IPv4-mapped peer address

    uint16_t port_;
    Listener plain_, tls_;
    SemaphoreHandle_t reqLock_;   // recursive: handlers and SSE pushes of both listeners, one at a time
    std::vector<Route*> routes_;
    THandler notFound_;
    PreRoute preRoute_ = nullptr;

    // per request
    httpd_req_t* req_ = nullptr;
    HttpArgs args_{ HTTP_ARGS_MAX, HTTP_ARG_BYTES_MAX };
    std::vector<String> hdrStore_;   // header strings must live until the response is sent
    String status_, type_;
    size_t contentLength_ = CONTENT_LENGTH_NOT_SET;
    bool   started_ = false, chunked_ = false, aborted_ = false;
    HTTPUpload upload_;

    // login: recently issued nonces (several pages / requests at once), the
    // nonce counts each one has seen
    struct Nonce { char v[33]; uint32_t ms; HttpNonceCount counts; };
    Nonce nonces_[8] = {};
    uint8_t nonceNext_ = 0;
    HttpLoginBrake brake_;
    enum class AuthResult : uint8_t { UNCHECKED, OK, FAILED };
    AuthResult authResult_ = AuthResult::UNCHECKED;   // this request's login (checked once)
    bool staleNonce_ = false;     // this login had a nonce we no longer know, or a spent count: stale=TRUE, no prompt
    uint32_t brakeWaitMs_ = 0;    // this client must wait this long after wrong logins: 429
    String realm_;

    // SSE: one work function per listener (httpd_queue_work runs it on that
    // listener's task)
    static void sseWorkPlain(void* arg);
    static void sseWorkTls(void* arg);
    void sseWork(Listener& l, SseBuild build);
    static void closeCb(httpd_handle_t hd, int fd);
    static void sseDrop(Listener& l, int fd);
    static void noteActivity(Listener& l, int fd);
    void closeIdle(Listener& l);
};
