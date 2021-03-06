#include "net.h"

#include "uv.h"
#include "ikcp.h"

#include <list>

#ifndef _WIN32
#include <ctime>
uint32_t GetTickCount() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

struct ClientContext {
    Client *client = nullptr;
    uv_loop_t loop;
    uv_udp_t udp;
    struct sockaddr_in host_addr;
    ikcpcb *kcp = nullptr;
    std::list<std::string> pends;
    std::string recv_buf;
};

struct SendStruct {
    uv_udp_send_t req;
    char base[1];
};

Client::Client() {
    context_ = new ClientContext;
    context_->client = this;
    uv_loop_init(&context_->loop);
}

Client::~Client() {
    uv_loop_close(&context_->loop);
    if (context_->kcp) {
        ikcp_release(context_->kcp);
        context_->kcp = nullptr;
    }
    context_->client = nullptr;
    delete context_;
}

void Client::connect(const char *addr, uint16_t port) {
    uv_ip4_addr(addr, port, &context_->host_addr);
    uv_udp_init(&context_->loop, &context_->udp);
    context_->udp.data = context_;
    doSend("C", 1);
    uv_udp_recv_start(&context_->udp,
        [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
            buf->base = new char[suggested_size];
            buf->len = suggested_size;
        },
        [](uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
            const struct sockaddr* addr, unsigned flags) {
            auto *c = ((ClientContext*)handle->data);
            if (nread >= 0 && addr->sa_family == AF_INET) {
                const auto *addrin = reinterpret_cast<const struct sockaddr_in*>(addr);
                if (addrin->sin_addr.s_addr == c->host_addr.sin_addr.s_addr
                    && addrin->sin_port == c->host_addr.sin_port) {
                    c->client->onUdpRecv(buf->base, nread);
                    delete[](char*)buf->base;
                    return;
                }
            }
            c->client->onError();
            delete[] (char*)buf->base;
        }
    );
}

void Client::sendData(int op, const void *data, int len) {
    std::string n;
    n.resize(len + 8);
    *(int*)&n[0] = op;
    *(int*)&n[4] = len;
    if (len > 0)
        memcpy(&n[8], data, len);
    if (ikcp_send(context_->kcp, &n[0], len + 8) < 0)
        context_->pends.emplace_back(std::move(n));
}

void Client::run() {
    uv_timer_t timer;
    uv_timer_init(&context_->loop, &timer);
    timer.data = context_;
    uv_timer_start(&timer,
        [](uv_timer_t* handle) {
            auto *c = (ClientContext*)handle->data;
            if (!c->kcp) return;
            ikcp_update(c->kcp, GetTickCount());
            while (!c->pends.empty()) {
                auto &p = c->pends.front();
                if (ikcp_send(c->kcp, &p[0], p.size() < 0)) break;
                c->pends.pop_front();
            }
            int n;
            char buf[3072];
            while ((n = ikcp_recv(c->kcp, buf, 3072)) > 0) {
                c->recv_buf.insert(c->recv_buf.end(), buf, buf + n);
                while (c->recv_buf.length() >= 8) {
                    size_t len = *(uint32_t*)&c->recv_buf[4];
                    if (len + 8 > c->recv_buf.length()) break;
                    c->client->onKcpData(*(int*)&c->recv_buf[0], c->recv_buf.c_str() + 8, len);
                    c->recv_buf.erase(c->recv_buf.begin(), c->recv_buf.begin() + len + 8);
                }
            }
    },
        20, 20
    );
    uv_run(&context_->loop, UV_RUN_DEFAULT);
}

void Client::stop() {
    uv_stop(&context_->loop);
    uv_close((uv_handle_t*)&context_->udp, nullptr);
}

void Client::doSend(const void *data, int len, const void *data2, int len2) {
    auto *s = (SendStruct*)(new char[sizeof(uv_udp_send_t) + len + len2]);
    memcpy(&s->base[0], data, len);
    if (len2)
        memcpy(&s->base[len], data2, len2);
    uv_buf_t b;
    b.base = &s->base[0];
    b.len = len + len2;
    uv_udp_send((uv_udp_send_t*)s, &context_->udp, &b, 1, (const struct sockaddr*)&context_->host_addr,
        [](uv_udp_send_t *req, int status) {
            delete[](char*)req;
        }
    );
}

void Client::onUdpRecv(const void *buf, int len) {
    if (len <= 0) return;
    const char *cbuf = (const char*)buf;
    switch (cbuf[0]) {
        case 'C': {
            if (len < 5) return;
            uint32_t conv = *(const uint32_t*)(cbuf + 1);
            context_->kcp = ikcp_create(conv, context_);
            ikcp_nodelay(context_->kcp, 0, 100, 0, 0);
            ikcp_setmtu(context_->kcp, 1440);
            context_->kcp->output = [](const char *buf, int len, ikcpcb *kcp, void *user) {
                auto *c = (ClientContext*)user;
                int n = 'K';
                c->client->doSend(&n, 4, buf, len);
                return 0;
            };
            onConnected();
            break;
        }
        case 'K':{
            if (len <= 4) return;
            ikcp_input(context_->kcp, (const char*)buf + 4, len - 4);
            break;
        }
    }
}
