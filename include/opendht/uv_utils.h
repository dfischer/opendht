#pragma once

#include "utils.h"

#include <uv.h>

namespace dht {

struct EventLoop {
    using OnSignal = std::function<void(int)>;

    EventLoop() {
        uv_loop_init(&loop);
        uv_signal_init(&loop, &sig_handler);
        sig_handler.data = this;
        uv_signal_start(&sig_handler, &EventLoop::signal_handler, SIGINT);
    }
    ~EventLoop() {
        uv_loop_close(&loop);
    }
    auto run() {
        return uv_run(&loop, UV_RUN_DEFAULT);
    }
    auto runOnce() {
        return uv_run(&loop, UV_RUN_NOWAIT);
    }
    void stop() {
        uv_signal_stop(&sig_handler);
        uv_walk(&loop, &EventLoop::close_walk_cb, this);
    }
    uv_loop_t* get() { return &loop; }
    void setOnSignal(OnSignal cb) {
        onSignal_ = cb;
    }
private:
    uv_loop_t loop;
    uv_signal_t sig_handler;
    OnSignal onSignal_;
    void onSignal(int sig) {
        if (onSignal_)
            onSignal_(sig);
    }
    static void close_walk_cb(uv_handle_t* handle, void* /*arg*/) {
        if (not uv_is_closing(handle))
            uv_close(handle, [](uv_handle_t* handle) {
                std::cout << "handle " << handle << " of type " << handle->type << " closed" << std::endl;
            });
    }
    static void signal_handler(uv_signal_t* handle, int signum) {
        if (handle->data) {
            static_cast<EventLoop*>(handle->data)->onSignal(signum);
        }
    }
};

using GetAddrInfoCb = std::function<void(std::vector<SockAddr>&&)>;

void getAddrInfo(uv_loop_t* loop, const char* node, const char* service, GetAddrInfoCb&& cb);

struct Job : std::enable_shared_from_this<Job> {
    Job(uv_loop_t* loop, std::function<void()>&& f = {}) : do_(std::move(f)) {
        uv_timer_init(loop, &timer_);
        timer_.data = nullptr;
    }
    void run(uint64_t t = 0) {
        uv_timer_start(&timer_, &Job::timer_cb, t, 0);
    }
    inline void run(const duration& t) {
        run(getTimeout(t));
    }
    inline void run(const time_point& t) {
        run(getTimeout(t));
    }
    void cancel() {
        if (timer_.data and not uv_is_closing((uv_handle_t*)&timer_)) {
            uv_timer_stop(&timer_);
            uv_close((uv_handle_t*)&timer_, &Job::close_cb);
        }
    }
    static Sp<Job> make(uv_loop_t* loop, std::function<void()>&& f = {}) {
        auto job = std::make_shared<Job>(loop, std::move(f));
        job->timer_.data = new Sp<Job>(job);
        return job;
    }
    static uint64_t getTimeout(const duration& d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }
    uint64_t getTimeout(const time_point& tp) const {
        uint64_t t = getTimeout(tp.time_since_epoch());
        uint64_t now = uv_now(timer_.loop);
        return (t < now) ? 0 : t - now;
    }
    std::function<void()> do_;
private:
    uv_timer_t timer_;
    void onTimer() {
        if (do_)
            do_();
        cancel();
    }
    void onClosed() {
        if (timer_.data) {
            delete static_cast<Sp<Job>*>(timer_.data);
            timer_.data = nullptr;
        }
    }
    static void timer_cb(uv_timer_t* handle) {
        if (handle->data) {
            auto timer = *static_cast<Sp<Job>*>(handle->data);
            timer->onTimer();
        }
    }
    static void close_cb(uv_handle_t* handle) {
        if (handle->data) {
            auto timer = *static_cast<Sp<Job>*>(handle->data);
            timer->onClosed();
        } else {
            std::cerr << "Job::onClosed(): no data! " << handle << std::endl;
        }
    }
};


static inline void alloc_buffer(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}


using OnClose = std::function<void()>;

struct UdpSocket : public std::enable_shared_from_this<UdpSocket>
{
    using OnReceive = std::function<void(const uint8_t* data, size_t size, const SockAddr& addr)>;
    UdpSocket(uv_loop_t* loop) {
        uv_udp_init(loop, &sock);
        sock.data = nullptr;
    }
    void open(const char* bind_addr, in_port_t port, OnReceive cb) {
        if (not sock.data)
            sock.data = new std::shared_ptr<UdpSocket>(shared_from_this());
        receive_cb = cb;
        struct sockaddr_in6 addr;
        uv_ip6_addr(bind_addr, port, &addr);
        uv_udp_bind(&sock, (const struct sockaddr*)&addr, 0);
        uv_udp_recv_start(&sock, &UdpSocket::on_alloc_buffer, &UdpSocket::on_receive);
    }

    int send(uint8_t* data, size_t size, const SockAddr& addr) {
        auto req = new SendReq;
        req->msg = data;
        req->req.data = req;
        const uv_buf_t buf {
            /*.base =*/ (char*)data,
            /*.len =*/ size
        };
        return uv_udp_send(&req->req, &sock, &buf, 1, addr.get(), &UdpSocket::on_sent);
    }

    void close(OnClose cb) {
        close_cb = cb;
        uv_close((uv_handle_t*) &sock, &UdpSocket::on_close);
    }
    bool isRunning() const {
        return uv_is_active((uv_handle_t*) &sock);
    }
    SockAddr getBoundAddr() {
        SockAddr addr;
        int size = sizeof(addr.first);
        uv_udp_getsockname(&sock, addr.get(), &size);
        addr.second = size;
        return addr;
    }
private:
    struct SendReq {
        uv_udp_send_t req;
        uint8_t* msg;
    };

    uv_udp_t sock;
    OnReceive receive_cb;
    OnClose close_cb;

    void onAllocBuffer(size_t suggested_size, uv_buf_t* buf) {
        buf->base = (char*)malloc(suggested_size);
        buf->len = suggested_size;
    }
    void onReceive(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned /*flags*/) {
        if (nread > 0 and addr and receive_cb) {
            receive_cb((const uint8_t*)buf->base, nread, SockAddr(addr));
        }
        if (buf->base)
            free(buf->base);
    }
    void onClosed() {
        if (close_cb)
            close_cb();
        if (auto s = static_cast<std::shared_ptr<UdpSocket>*>(sock.data)) {
            sock.data = nullptr;
            delete s;
        }
    }
    void onSent(SendReq& req, int /*status*/) {
        // if (status) fprintf(stderr, "onSent %s\n", uv_strerror(status));
        free(req.msg);
    }
    static std::shared_ptr<UdpSocket> get(uv_handle_t* handle) {
        return handle->data ? *static_cast<std::shared_ptr<UdpSocket>*>(handle->data) : std::shared_ptr<UdpSocket>();
    }
    static void on_close(uv_handle_t* handle) {
        if (auto node = get(handle))
            node->onClosed();
    }
    static void on_sent(uv_udp_send_t* req, int status) {
        if (auto data = static_cast<SendReq*>(req->data)) {
            if (auto node = get((uv_handle_t*)req->handle))
                node->onSent(*data, status);
            delete data;
        }
    }
    static void on_receive(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
        if (auto node = get((uv_handle_t*)handle))
            node->onReceive(nread, buf, addr, flags);
    }
    static inline void on_alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t *buf) {
        if (auto node = get((uv_handle_t*)handle))
            node->onAllocBuffer(suggested_size, buf);
    }
};

struct TcpSocket : public std::enable_shared_from_this<TcpSocket>
{
    using OnConnect = std::function<void(std::shared_ptr<TcpSocket>&& new_socket)>;
    using OnConnected = std::function<void(int status)>;
    using OnRead = std::function<void(const uint8_t* data, size_t size)>;
    using OnWrite = std::function<void(int status)>;

    TcpSocket(uv_loop_t* loop) {
        uv_tcp_init(loop, &sock);
        sock.data = nullptr;
    }
    void listen(const char* bind_addr, in_port_t port, OnConnect cb) {
        if (not sock.data)
            sock.data = new std::shared_ptr<TcpSocket>(shared_from_this());
        connect_cb = cb;
        struct sockaddr_in6 addr;
        uv_ip6_addr(bind_addr, port, &addr);
        uv_tcp_bind(&sock, (const struct sockaddr*)&addr, 0);
        if (int r = uv_listen((uv_stream_t*)&sock, 128, &TcpSocket::on_new_connection)) {
            fprintf(stderr, "Listen error %s\n", uv_strerror(r));
        }
    }
    void connect(const sockaddr* addr, OnConnected&& cb) {
        if (not sock.data)
            sock.data = new std::shared_ptr<TcpSocket>(shared_from_this());
        auto req = new ConnectReq;
        req->connect.data = req;
        req->sock = shared_from_this();
        req->callback = std::move(cb);
        uv_tcp_connect(&req->connect, &sock, addr, &TcpSocket::on_connect);
    }
    void start(OnRead&& cb) {
        std::cerr << "TCP start" << std::endl;
        read_cb = std::move(cb);
        uv_read_start((uv_stream_t*)&sock, &TcpSocket::on_alloc_buffer, &TcpSocket::on_read);
    }
    int write(uint8_t* msg, size_t len, OnWrite&& cb = {}) {
        std::cerr << "TCP write " << len << std::endl;
        auto req = new WriteReq;
        req->write.data = req;
        req->msg = msg;
        req->sock = shared_from_this();
        req->callback = std::move(cb);
        uv_buf_t buf;
        buf.base = (char*)msg;
        buf.len = len;
        return uv_write(&req->write, (uv_stream_t *)&sock, &buf, 1, &TcpSocket::on_write_end);
    }
    void close(OnClose onclose = {}) {
        std::cerr << "TCP close" << std::endl;
        if (not isClosed()) {
            close_cb = onclose;
            uv_close((uv_handle_t*) &sock, &TcpSocket::on_close);
        }
    }
    bool isClosed() {
        return not sock.data or uv_is_closing((const uv_handle_t*) &sock);
    }
    bool canWrite() const {
        return uv_is_writable((const uv_stream_t*)&sock);
    }
    SockAddr getBoundAddr() {
        SockAddr addr;
        int size = sizeof(addr.first);
        uv_tcp_getsockname(&sock, addr.get(), &size);
        addr.second = size;
        return addr;
    }
    SockAddr getPeerAddr() {
        SockAddr addr;
        int size = sizeof(addr.first);
        uv_tcp_getpeername(&sock, addr.get(), &size);
        addr.second = size;
        return addr;
    }
private:
    uv_tcp_t sock;
    OnConnect connect_cb;
    OnRead read_cb;
    OnClose close_cb;
    struct ConnectReq {
        uv_connect_t connect;
        std::shared_ptr<TcpSocket> sock;
        OnConnected callback;
    };
    struct WriteReq {
        uv_write_t write;
        uint8_t* msg;
        std::shared_ptr<TcpSocket> sock;
        OnWrite callback;
    };  
    std::shared_ptr<TcpSocket> accept() {
        auto s = std::make_shared<TcpSocket>(sock.loop);
        s->sock.data = new std::shared_ptr<TcpSocket>(s);
        if (uv_accept((uv_stream_t*)&sock, (uv_stream_t*) &s->sock) == 0) {
            return s;
        } else {
            s->close();
            return {};
        }
    }
    void onNewConnection(int status) {
        if (status < 0) {
            fprintf(stderr, "New connection error %s\n", uv_strerror(status));
            return;
        }
        if (auto s = accept())
            connect_cb(std::move(s));
        fprintf(stderr, "on_new_connection end\n");
    }
    void onConnected(int status, OnConnected&& cb) {
        if (cb)
            cb(status);
    }
    void onAllocBuffer(size_t suggested_size, uv_buf_t* buf) {
        buf->base = (char*)malloc(suggested_size);
        buf->len = suggested_size;
    }
    void onRead(ssize_t nread, const uv_buf_t *buf) {
        if (nread < 0) {
            if (nread != UV_EOF) {
                fprintf(stderr, "Read error %s\n", uv_err_name(nread));
            } else {
                fprintf(stderr, "EOF\n");
            }
            close();
        } else if (nread > 0) {
            if (read_cb)
                read_cb((const uint8_t*)buf->base, nread);
        }
        if (buf->base)
            free(buf->base);
    }
    void onWrite(int status, OnWrite&& cb) {
        if (cb)
            cb(status);
    }
    void onClosed() {
        read_cb = {};
        connect_cb = {};
        if (close_cb) {
            close_cb();
            close_cb = {};
        }
        if (auto s = static_cast<std::shared_ptr<TcpSocket>*>(sock.data)) {
            sock.data = nullptr;
            delete s;
        }
    }
    static std::shared_ptr<TcpSocket> get(uv_handle_t* handle) {
        return handle->data ? *static_cast<std::shared_ptr<TcpSocket>*>(handle->data) : std::shared_ptr<TcpSocket>();
    }
    static void on_new_connection(uv_stream_t* handle, int status) {
        if (auto node = get((uv_handle_t*)handle))
            node->onNewConnection(status);
    }
    static void on_close(uv_handle_t* handle) {
        if (auto node = get(handle))
            node->onClosed();
    }
    static inline void on_alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t *buf) {
        if (auto node = get((uv_handle_t*)handle))
            node->onAllocBuffer(suggested_size, buf);
    }
    static void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t *buf) {
        if (auto node = get((uv_handle_t*)client))
            node->onRead(nread, buf);
    }
    static void on_write_end(uv_write_t* req, int status) {
        if (auto write = static_cast<WriteReq*>(req->data)) {
            write->sock->onWrite(status, std::move(write->callback));
            if (write->msg)
                free(write->msg);
            delete write;
        }
    }
    static void on_connect(uv_connect_t* req, int status) {
        if (auto conn = static_cast<ConnectReq*>(req->data)) {
            conn->sock->onConnected(status, std::move(conn->callback));
            delete conn;
        }
    }
};


}
