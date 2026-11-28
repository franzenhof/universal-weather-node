#include "http_server.h"
#include <esp_https_server.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "http_util.h"
#include "serial_log.h"   // dbgSerial: a dropped SSE subscriber is logged
#include <errno.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <memory>
#include <new>

// Task watchdog over the request handlers (B11): the server task is
// subscribed only while a handler runs - idle between requests it waits in
// select(), which must not count as a hang. Every received or sent chunk
// feeds it, so a long upload or page is fine as long as data moves; a
// handler stuck for 120 s (the timeout main.cpp's setup() sets)
// resets the node with a core dump naming the server task.
namespace {
struct HandlerWatchdog {
    bool subscribed;
    HandlerWatchdog() : subscribed(esp_task_wdt_add(nullptr) == ESP_OK) {}
    ~HandlerWatchdog() { if (subscribed) esp_task_wdt_delete(nullptr); }
};
// Feeds the watchdog if this task is watched (the server task in a handler,
// the webApi task's SSE pushes); quiet otherwise (a reset of an unwatched task logs an error).
inline void feedWatchdog() { if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset(); }
} // namespace

namespace {

httpd_method_t idfMethod(HTTPMethod m) { return m == HTTP_ANY ? (httpd_method_t)INT_MAX : (httpd_method_t)m; }

HttpServer* gServer = nullptr;   // the one instance (dispatch has only the request)

// Handlers and SSE pushes of both listeners run one at a time (reqLock_).
// Taken before the handler watchdog is armed: a request waiting for a long
// one on the other listener (a firmware upload) is not a hang.
struct ReqLock {
    SemaphoreHandle_t m;
    explicit ReqLock(SemaphoreHandle_t x) : m(x) { xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~ReqLock() { xSemaphoreGiveRecursive(m); }
};

// A nonce stays valid this long (each of its counts once, see
// HttpNonceCount); then the browser gets stale=TRUE and retries with a new
// one by itself (no prompt).
constexpr uint32_t NONCE_MAX_AGE_MS = 3600UL * 1000UL;

const char* reason(int code)
{
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 204: return "204 No Content";
        case 301: return "301 Moved Permanently";
        case 302: return "302 Found";
        case 303: return "303 See Other";
        case 304: return "304 Not Modified";
        case 307: return "307 Temporary Redirect";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 408: return "408 Request Timeout";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        case 431: return "431 Request Header Fields Too Large";
        case 500: return "500 Internal Server Error";
        case 503: return "503 Service Unavailable";
        default:  return nullptr;
    }
}

// multipart: what the parser callbacks work on
struct MultipartCtx {
    HttpServer* srv;
    const HttpServer::THandler* upload;   // nullptr: files are skipped
    HTTPUpload* up;
    HttpArgs* args;
    bool file = false;
    bool uploadBegun = false;   // the upload handler saw UPLOAD_FILE_START
};

} // namespace

// ---- setup -------------------------------------------------------------------

HttpServer::HttpServer(uint16_t port) : port_(port), reqLock_(xSemaphoreCreateRecursiveMutex()) {}

void HttpServer::on(const char* uri, HTTPMethod method, THandler fn, THandler upload)
{
    Route* r = new Route{ String(uri), method, fn, upload };
    routes_.push_back(r);
    for (httpd_handle_t h : { plain_.h, tls_.h }) {
        if (!h) continue;
        httpd_uri_t u = {};
        u.uri = r->uri.c_str();
        u.method = idfMethod(method);
        u.handler = dispatch;
        u.user_ctx = r;
        httpd_register_uri_handler(h, &u);
    }
}

httpd_config_t HttpServer::baseConfig(uint16_t port, int maxSockets) const
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = port;
    c.max_uri_handlers = routes_.size() + 4;
    c.max_open_sockets = maxSockets; // of LWIP_MAX_SOCKETS (platformio.ini; 3 more used by each server itself)
    c.lru_purge_enable = true;       // a new client closes the least recently used idle connection
    c.max_resp_headers = 16;
    c.stack_size = 12288;            // the page handlers (JSON, settings page) run on it - internal RAM
    c.task_priority = 1;             // = the main loop task: neither starves the other
    c.core_id = 0;                   // the loop runs on core 1
    c.recv_wait_timeout = 10;        // s - a stalled client does not hold the server
    c.send_wait_timeout = 10;
    c.close_fn = closeCb;            // drops an SSE subscriber, then closes the socket
    return c;
}

void HttpServer::registerRoutes(httpd_handle_t h)
{
    for (Route* r : routes_) {
        httpd_uri_t u = {};
        u.uri = r->uri.c_str();
        u.method = idfMethod(r->method);
        u.handler = dispatch;
        u.user_ctx = r;
        httpd_register_uri_handler(h, &u);
    }
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, notFoundCb);
    httpd_register_err_handler(h, HTTPD_405_METHOD_NOT_ALLOWED, notFoundCb);   // WebServer: onNotFound too
}

void HttpServer::resetListener(Listener& l)
{
    for (int& fd : l.sseFd) fd = -1;
    l.sseCount = 0;
    for (Act& a : l.act) a = Act{ 0, 0 };
}

void HttpServer::begin(int maxSockets)
{
    if (plain_.h) return;
    gServer = this;
    httpd_config_t c = baseConfig(port_, maxSockets);
    resetListener(plain_);
    plain_.sseMax = SSE_MAX;
    if (httpd_start(&plain_.h, &c) != ESP_OK) { plain_.h = nullptr; return; }
    registerRoutes(plain_.h);
}

esp_err_t HttpServer::beginTls(uint16_t port, const char* cert, size_t certLen, const char* key, size_t keyLen, int maxSockets)
{
    if (tls_.h) return ESP_OK;
    gServer = this;
    httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
    sc.httpd = baseConfig(port, maxSockets);
    sc.httpd.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;   // the plain server has the default one
    sc.httpd.send_wait_timeout = 5;    // bounds a blocking TLS send to a stalled SSE subscriber
    sc.servercert = (const uint8_t*)cert;
    sc.servercert_len = certLen + 1;   // PEM: with the terminating 0
    sc.prvtkey_pem = (const uint8_t*)key;
    sc.prvtkey_len = keyLen + 1;
    sc.port_secure = port;
    sc.tls_handshake_timeout_ms = 8000;
    resetListener(tls_);
    tls_.sseMax = SSE_MAX_TLS;
    const esp_err_t e = httpd_ssl_start(&tls_.h, &sc);
    if (e != ESP_OK) { tls_.h = nullptr; return e; }
    registerRoutes(tls_.h);
    return ESP_OK;
}

void HttpServer::stop()
{
    if (tls_.h) { httpd_ssl_stop(tls_.h); tls_.h = nullptr; }
    if (plain_.h) { httpd_stop(plain_.h); plain_.h = nullptr; }
    resetListener(tls_);
    resetListener(plain_);
}

// ---- Server-Sent Events ----

void HttpServer::closeCb(httpd_handle_t hd, int fd)
{
    if (gServer) {
        if (Listener* l = gServer->listenerOf(hd)) {   // runs on that listener's task
            sseDrop(*l, fd);
            for (Act& x : l->act) if (x.fd == fd) x.ms = 0;   // the fd number may come back for a new client
        }
    }
    close(fd);   // with a close_fn set, the server leaves closing the socket to it
}

void HttpServer::sseDrop(Listener& l, int fd)
{
    for (int& x : l.sseFd) if (x == fd) { x = -1; l.sseCount = l.sseCount - 1; }
}

bool HttpServer::sseAccept()
{
    Listener* l = req_ ? listenerOf(req_->handle) : nullptr;
    if (!l || started_ || l->sseCount >= l->sseMax) return false;
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n"
        "retry: 5000\n\n";
    if (httpd_send(req_, head, sizeof(head) - 1) != (int)(sizeof(head) - 1)) return false;
    const int fd = httpd_req_to_sockfd(req_);
    for (int& x : l->sseFd) if (x < 0) { x = fd; l->sseCount = l->sseCount + 1; break; }
    started_ = true;    // answered: finishRequest() sends nothing more
    chunked_ = false;
    return true;
}

bool HttpServer::ssePublish(SseBuild build)
{
    bool queued = false;
    if (plain_.h && plain_.sseCount > 0) queued |= httpd_queue_work(plain_.h, sseWorkPlain, (void*)build) == ESP_OK;
    if (tls_.h && tls_.sseCount > 0) queued |= httpd_queue_work(tls_.h, sseWorkTls, (void*)build) == ESP_OK;
    return queued;
}

void HttpServer::sseWorkPlain(void* arg) { if (gServer) gServer->sseWork(gServer->plain_, (SseBuild)arg); }
void HttpServer::sseWorkTls(void* arg)   { if (gServer) gServer->sseWork(gServer->tls_, (SseBuild)arg); }

namespace {
// The socket takes data now (TLS subscribers: a TLS send would block).
bool writableNow(int fd)
{
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = { 0, 0 };
    return select(fd + 1, nullptr, &w, nullptr, &tv) > 0;
}
} // namespace

void HttpServer::sseWork(Listener& l, SseBuild build)
{
    ReqLock lock(reqLock_);   // build() reads what handlers read
    if (!l.h || l.sseCount <= 0) return;
    const bool tls = &l == &tls_;
    closeIdle(l);   // keep slots free for the subscribers (see Act)
    String msg;
    if (build) {
        String data;
        build(data);                     // one line of JSON (no newline inside)
        msg.reserve(data.length() + 10);
        msg = "data: "; msg += data; msg += "\n\n";
    } else {
        msg = "event: ping\ndata: 1\n\n";   // a named event: EventSource sees it (a comment would fire nothing)
    }
    for (int i = 0; i < SSE_MAX; i++) {
        const int fd = l.sseFd[i];
        if (fd < 0) continue;
        // non-blocking: a client that can't take it at once (stalled, slow) is dropped -
        // a partial event would corrupt the stream, a blocking send would stall every page
        const int n = tls && !writableNow(fd) ? -1 : httpd_socket_send(l.h, fd, msg.c_str(), msg.length(), MSG_DONTWAIT);
        if (n != (int)msg.length()) {
            dbgSerial.printf("SSE%s: send to fd %d failed (%d of %u, errno %d) - dropped\n", tls ? " (TLS)" : "", fd, n, (unsigned)msg.length(), errno);
            sseDrop(l, fd); httpd_sess_trigger_close(l.h, fd);
        }
    }
}

// ---- request -------------------------------------------------------------------

void HttpServer::beginRequest(httpd_req_t* r)
{
    req_ = r;
    args_.clear();   // wipes the last request's fields (passwords) too
    hdrStore_.clear();
    status_ = String();
    type_ = String();
    contentLength_ = CONTENT_LENGTH_NOT_SET;
    started_ = chunked_ = aborted_ = false;
    staleNonce_ = false;
    authResult_ = AuthResult::UNCHECKED;
    brakeWaitMs_ = 0;
    upload_.status = UPLOAD_FILE_START;
    upload_.filename = upload_.name = upload_.type = String();
    upload_.totalSize = upload_.currentSize = 0;
    const size_t ql = httpd_req_get_url_query_len(r);
    if (ql) {
        std::unique_ptr<char[]> q(new (std::nothrow) char[ql + 2]);
        if (q && httpd_req_get_url_query_str(r, q.get(), ql + 1) == ESP_OK)
            httpParseForm(q.get(), ql, [](const char* n, const char* v, void* c) { static_cast<HttpArgs*>(c)->add(n, v); }, &args_);
    }
}

// Runs before the handler and so before any login check: everything a
// client can make it allocate is bounded (HTTP_FORM_MAX, HTTP_ARGS_MAX,
// HTTP_ARG_BYTES_MAX, the time limits); over a limit the request is
// answered here and the connection closed.
bool HttpServer::readBody(const Route* route)
{
    const size_t total = req_->content_len;
    if (args_.overLimit()) { send(413, "text/plain", "too many arguments"); return false; }   // the query already
    if (!total) return true;
    const String ct = header("Content-Type");
    const bool takesUpload = route && route->upload;
    const uint32_t startMs = millis();
    const uint32_t maxMs = takesUpload ? HTTP_UPLOAD_MAX_MS : HTTP_BODY_MAX_MS;
    char tmp[1024];

    char boundary[72];
    if (HttpMultipart::boundaryOf(ct.c_str(), boundary, sizeof(boundary))) {
        // only a file streamed to an upload handler may be larger than a form
        if (!takesUpload && total > HTTP_FORM_MAX) { send(413, "text/plain", "request too large"); return false; }
        MultipartCtx mc{ this, takesUpload ? &route->upload : nullptr, &upload_, &args_ };
        HttpMultipart mp;
        HttpMultipart::Callbacks cb;
        cb.ctx = &mc;
        cb.partBegin = [](const char* name, const char* filename, const char* type, void* c) {
            MultipartCtx& m = *static_cast<MultipartCtx*>(c);
            m.file = filename && filename[0];
            if (m.file) {
                if (!m.upload) return;
                m.up->status = UPLOAD_FILE_START;
                m.up->name = name; m.up->filename = filename; m.up->type = type;
                m.up->totalSize = m.up->currentSize = 0;
                m.uploadBegun = true;
                (*m.upload)();
            } else {
                m.args->beginField(name);
            }
        };
        cb.partData = [](const uint8_t* d, size_t n, void* c) {
            MultipartCtx& m = *static_cast<MultipartCtx*>(c);
            if (!m.file) { m.args->appendField(d, n); return; }
            if (!m.upload) return;
            while (n) {   // HTTPUpload::buf pieces, as WebServer delivers them
                const size_t k = n < HTTP_UPLOAD_BUFLEN ? n : HTTP_UPLOAD_BUFLEN;
                memcpy(m.up->buf, d, k);
                m.up->status = UPLOAD_FILE_WRITE;
                m.up->currentSize = k;
                m.up->totalSize += k;
                (*m.upload)();
                d += k; n -= k;
            }
        };
        cb.partEnd = [](void* c) {
            MultipartCtx& m = *static_cast<MultipartCtx*>(c);
            if (m.file) {
                if (!m.upload) return;
                // the file is complete; whatever it does with it, the upload
                // handler commits nothing before the whole request was read
                // (ota_upload.h: the route's handler commits)
                m.up->status = UPLOAD_FILE_END;
                m.up->currentSize = 0;
                (*m.upload)();
                m.file = false;
            } else {
                m.args->endField();
            }
        };
        mp.begin(ct.c_str(), cb);
        size_t left = total;
        int timeouts = 0;
        int code = 0;   // why the body was not read completely
        while (left) {
            if (millis() - startMs > maxMs) { code = 408; break; }
            const int n = httpd_req_recv(req_, tmp, left < sizeof(tmp) ? left : sizeof(tmp));
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            if (n <= 0 || !mp.feed((const uint8_t*)tmp, (size_t)n)) break;
            if (args_.overLimit()) { code = 413; break; }
            left -= (size_t)n;
            feedWatchdog();
        }
        if (left || !mp.done()) {
            // a file handed over (even completely) belongs to a request that failed: abort it
            if (mc.uploadBegun) { upload_.status = UPLOAD_FILE_ABORTED; upload_.currentSize = 0; (*mc.upload)(); }
            if (code == 413)      send(413, "text/plain", "too many or too large fields");
            else if (code == 408) send(408, "text/plain", "request body too slow");
            else                  send(400, "text/plain", "upload broken off");
            return false;
        }
        return true;
    }

    if (total > HTTP_FORM_MAX) { send(413, "text/plain", "request too large"); return false; }
    // >= 4 KB: PSRAM; wiped before it is freed (it may hold a password)
    struct WipedFree { size_t len; void operator()(char* p) const { httpWipe(p, len); free(p); } };
    std::unique_ptr<char, WipedFree> body((char*)malloc(total + 1), WipedFree{ total + 1 });
    if (!body) { send(503, "text/plain", "low memory"); return false; }
    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        if (millis() - startMs > maxMs) { send(408, "text/plain", "request body too slow"); return false; }
        const int n = httpd_req_recv(req_, body.get() + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) { send(408, "text/plain", "request body incomplete"); return false; }
        got += (size_t)n;
        feedWatchdog();
    }
    body.get()[total] = '\0';
    if (ct.startsWith("application/x-www-form-urlencoded"))
        httpParseForm(body.get(), total, [](const char* n, const char* v, void* c) { static_cast<HttpArgs*>(c)->add(n, v); }, &args_);
    else
        args_.add("plain", body.get());   // JSON etc., as WebServer names it
    if (args_.overLimit()) { send(413, "text/plain", "too many arguments"); return false; }
    return true;
}

bool HttpServer::finishRequest()
{
    if (!started_) send(500, "text/plain", "no response");
    else if (chunked_ && !aborted_) httpd_resp_send_chunk(req_, nullptr, 0);
    req_ = nullptr;
    return !aborted_;
}


esp_err_t HttpServer::dispatch(httpd_req_t* r)
{
    HttpServer& s = *gServer;
    ReqLock lock(s.reqLock_);
    HandlerWatchdog watch;
    const Route* route = static_cast<const Route*>(r->user_ctx);
    s.beginRequest(r);
    // the pre-route check needs only the head: a redirect is answered before
    // a body (e.g. a firmware upload) is read - an unread body closes the connection
    if (s.preRoute_ && s.preRoute_()) {
        s.finishRequest();
        return r->content_len ? ESP_FAIL : ESP_OK;
    }
    const bool ok = s.readBody(route);   // false: refused, answered already
    if (ok && route->fn) route->fn();
    const bool complete = s.finishRequest();
    if (Listener* l = s.listenerOf(r->handle)) noteActivity(*l, httpd_req_to_sockfd(r));
    return ok && complete ? ESP_OK : ESP_FAIL;   // ESP_FAIL closes the connection: no draining of a refused body, no cut response kept
}

void HttpServer::noteActivity(Listener& l, int fd)
{
    Act* slot = nullptr;
    for (Act& a : l.act) if (a.fd == fd && a.ms) { slot = &a; break; }
    if (!slot) for (Act& a : l.act) if (!a.ms) { slot = &a; break; }
    if (!slot) {   // full: reuse the oldest entry
        slot = &l.act[0];
        for (Act& a : l.act) if ((int32_t)(a.ms - slot->ms) < 0) slot = &a;
    }
    slot->fd = fd;
    slot->ms = millis() | 1;   // 0 = free
}

void HttpServer::closeIdle(Listener& l)
{
    const uint32_t now = millis();
    for (Act& a : l.act) {
        if (!a.ms) continue;
        bool sse = false;
        for (int f : l.sseFd) sse |= f == a.fd;
        if (sse) { a.ms = 0; continue; }                 // a subscriber: never idle-closed
        if (now - a.ms < IDLE_CLOSE_MS) continue;
        httpd_sess_trigger_close(l.h, a.fd);             // a no-op for a socket that is gone already
        a.ms = 0;
    }
}

esp_err_t HttpServer::notFoundCb(httpd_req_t* r, httpd_err_code_t)
{
    HttpServer& s = *gServer;
    ReqLock lock(s.reqLock_);
    HandlerWatchdog watch;
    s.beginRequest(r);
    if (s.preRoute_ && s.preRoute_()) {
        s.finishRequest();
        return r->content_len ? ESP_FAIL : ESP_OK;
    }
    const bool ok = s.readBody(nullptr);
    if (ok) {
        if (s.notFound_) s.notFound_();
        else s.send(404, "text/plain", "not found");
    }
    const bool complete = s.finishRequest();
    return ok && complete ? ESP_OK : ESP_FAIL;
}

String HttpServer::header(const char* name) const
{
    if (!req_) return String();
    const size_t n = httpd_req_get_hdr_value_len(req_, name);
    if (!n) return String();
    std::unique_ptr<char[]> v(new (std::nothrow) char[n + 1]);
    if (!v || httpd_req_get_hdr_value_str(req_, name, v.get(), n + 1) != ESP_OK) return String();
    return String(v.get());
}

String HttpServer::uri() const
{
    if (!req_) return String();
    const char* q = strchr(req_->uri, '?');
    return q ? String(req_->uri).substring(0, q - req_->uri) : String(req_->uri);
}

HTTPMethod HttpServer::method() const { return req_ ? (HTTPMethod)req_->method : HTTP_GET; }

bool HttpServer::clientAddress(uint8_t out[16]) const
{
    if (!req_) return false;
    struct sockaddr_storage a;
    socklen_t len = sizeof(a);
    if (getpeername(httpd_req_to_sockfd(req_), (struct sockaddr*)&a, &len) != 0) return false;
    memset(out, 0, 16);
    if (a.ss_family == AF_INET) {
        out[10] = out[11] = 0xFF;   // as IPv4-mapped IPv6
        memcpy(out + 12, &((struct sockaddr_in*)&a)->sin_addr.s_addr, 4);
        return true;
    }
#if LWIP_IPV6
    if (a.ss_family == AF_INET6) {
        memcpy(out, &((struct sockaddr_in6*)&a)->sin6_addr, 16);
        return true;
    }
#endif
    return false;
}

bool HttpServer::localIPv4(uint8_t out[4]) const
{
    if (!req_) return false;
    struct sockaddr_storage a;
    socklen_t len = sizeof(a);
    if (getsockname(httpd_req_to_sockfd(req_), (struct sockaddr*)&a, &len) != 0) return false;
    if (a.ss_family == AF_INET) {
        memcpy(out, &((struct sockaddr_in*)&a)->sin_addr.s_addr, 4);
        return true;
    }
#if LWIP_IPV6
    if (a.ss_family == AF_INET6) {   // the server listens on IPv6 with IPv4 mapped: ::ffff:a.b.c.d
        const uint8_t* b = (const uint8_t*)&((struct sockaddr_in6*)&a)->sin6_addr;
        static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
        if (memcmp(b, mapped, 12)) return false;
        memcpy(out, b + 12, 4);
        return true;
    }
#endif
    return false;
}

// ---- response ------------------------------------------------------------------

void HttpServer::sendHeader(const String& name, const String& value, bool first)
{
    if (started_) return;   // too late: the head is out
    if (first) { hdrStore_.insert(hdrStore_.begin(), value); hdrStore_.insert(hdrStore_.begin(), name); }
    else { hdrStore_.push_back(name); hdrStore_.push_back(value); }
}

void HttpServer::startResponse(int code, const char* type, bool chunked)
{
    const char* r = reason(code);
    status_ = r ? String(r) : String(code) + " Status";
    httpd_resp_set_status(req_, status_.c_str());
    if (type && *type) { type_ = type; httpd_resp_set_type(req_, type_.c_str()); }
    for (size_t i = 0; i + 1 < hdrStore_.size(); i += 2) httpd_resp_set_hdr(req_, hdrStore_[i].c_str(), hdrStore_[i + 1].c_str());
    started_ = true;
    chunked_ = chunked;
}

void HttpServer::send(int code, const char* type, const char* content)
{
    if (!req_ || started_) return;
    const size_t len = content ? strlen(content) : 0;
    if (contentLength_ != CONTENT_LENGTH_NOT_SET) {   // setContentLength() before: the body follows with sendContent()
        startResponse(code, type, true);
        if (len) httpd_resp_send_chunk(req_, content, len);
        return;
    }
    startResponse(code, type, false);
    httpd_resp_send(req_, content ? content : "", len);
}

void HttpServer::send(int code, const char* type, const String& content) { send(code, type, content.c_str()); }

void HttpServer::send_P(int code, const char* type, const char* data, size_t len)
{
    if (!req_ || started_) return;
    startResponse(code, type, false);
    httpd_resp_send(req_, data, len);
}

bool HttpServer::sendContent(const char* data, size_t len)
{
    if (!req_ || aborted_) return false;
    if (!started_) startResponse(200, nullptr, true);
    if (!chunked_ || !len) return true;   // "" ends a chunked response in WebServer: done here in finishRequest()
    if (httpd_resp_send_chunk(req_, data, len) != ESP_OK) aborted_ = true;
    feedWatchdog();
    return !aborted_;
}

// ---- login ---------------------------------------------------------------------

bool HttpServer::authenticate(const char* user, const char* password)
{
    if (!req_ || !password || !*password) return false;
    if (authResult_ != AuthResult::UNCHECKED) return authResult_ == AuthResult::OK;
    const String h = header("Authorization");
    if (h.isEmpty()) return false;   // no login sent: not a wrong one, no brake
    authResult_ = AuthResult::FAILED;
    uint8_t client[16];
    if (!clientAddress(client)) memset(client, 0, sizeof(client));   // unknown: all such clients share one entry
    // a brake on guessing: answered with 429 (requestAuthentication), the
    // try is not checked - no sleep, so other requests are not held up
    brakeWaitMs_ = brake_.waitMs(client, millis());
    if (brakeWaitMs_) return false;
    HttpDigest d;
    uint32_t nc = 0;
    // qop=auth is what the challenge asks for: without it there is no nonce count to guard
    bool ok = httpParseDigest(h.c_str(), d) && !strcmp(d.username, user) && !strcmp(d.uri, req_->uri) &&
              !strcmp(d.qop, "auth") && httpParseNonceCount(d.nc, nc);
    Nonce* nonce = nullptr;
    if (ok) {
        for (Nonce& n : nonces_)
            if (n.v[0] && !strcmp(n.v, d.nonce) && millis() - n.ms < NONCE_MAX_AGE_MS) nonce = &n;
        if (!nonce) {   // issued before a restart, expired or pushed out: not a wrong password
            staleNonce_ = true;   // requestAuthentication(): the browser retries with its stored password
            return false;
        }
        char expect[33];
        httpDigestResponse(d, http_method_str((http_method)req_->method), password, expect);
        ok = !strcmp(expect, d.response);
    }
    if (!ok) { brake_.failed(client, millis()); return false; }
    // the right password - but a count used before is a recorded request
    // sent again (it must not clear the brake either)
    if (!httpNonceCountAccept(nonce->counts, nc)) { staleNonce_ = true; return false; }
    brake_.succeeded(client);
    authResult_ = AuthResult::OK;
    return true;
}

void HttpServer::requestAuthentication(HTTPAuthMethod, const char* realm)
{
    if (brakeWaitMs_) {
        sendHeader("Retry-After", String((brakeWaitMs_ + 999) / 1000));
        send(429, "text/plain", "429 Too Many Requests - too many wrong logins, try again later");
        return;
    }
    realm_ = realm ? realm : "Login Required";
    Nonce& n = nonces_[nonceNext_];
    nonceNext_ = (uint8_t)((nonceNext_ + 1) % (sizeof(nonces_) / sizeof(nonces_[0])));
    uint32_t rnd[4];
    for (uint32_t& x : rnd) x = esp_random();
    httpMd5Hex(rnd, sizeof(rnd), n.v);
    n.ms = millis();
    n.counts = HttpNonceCount();
    char opaque[33];
    httpMd5Hex(realm_.c_str(), realm_.length(), opaque);
    sendHeader("WWW-Authenticate", String("Digest realm=\"") + realm_ + "\", qop=\"auth\", nonce=\"" + n.v + "\", opaque=\"" + opaque + "\"" +
               (staleNonce_ ? ", stale=TRUE" : ""));
    send(401, "text/plain", "401 Unauthorized");
}
