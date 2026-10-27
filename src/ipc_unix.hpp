// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - control channel server for Linux: a non-blocking Unix socket
// driven from the daemon's poll() loop. Only the same user may connect.
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>

#include "control.hpp"

namespace fg {

class ControlServer {
public:
    ~ControlServer() { stop(); }

    bool start(const std::string& path) {
        sockaddr_un addr{};
        if (path.size() >= sizeof addr.sun_path) return false;
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        unlink(path.c_str());  // stale socket from a crash; we hold the single-instance lock
        const mode_t old_mask = umask(077);  // owner-only from the start, no chmod race
        const bool ok = bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(fd_, 8) == 0;
        umask(old_mask);
        if (!ok) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        path_ = path;
        return true;
    }

    void stop() {
        for (auto& c : clients_) close(c->fd);
        clients_.clear();
        if (fd_ >= 0) {
            close(fd_);
            unlink(path_.c_str());
            fd_ = -1;
        }
    }

    // Appends the descriptors to watch; call right before poll().
    void fill(std::vector<pollfd>& fds) {
        first_ = fds.size();
        polled_ = clients_.size();
        fds.push_back({fd_, POLLIN, 0});  // a negative fd is ignored by poll()
        for (auto& c : clients_) fds.push_back({c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
    }

    // Handles what poll() reported. on_line(line, subscribe&) returns the reply.
    template <class F> void handle(const std::vector<pollfd>& fds, F&& on_line) {
        if (first_ + 1 + polled_ > fds.size()) return;
        for (size_t k = 0; k < polled_; ++k) {
            Client& c = *clients_[k];
            const short re = fds[first_ + 1 + k].revents;
            if (re & POLLIN) read_from(c, on_line);
            if (re & (POLLHUP | POLLERR | POLLNVAL)) c.dead = true;
            flush(c);
        }
        if (fds[first_].revents & POLLIN) accept_all();
        reap();
    }

    // Sends a line to every client that subscribed. Dead clients are only
    // marked here and removed by handle(), as this may run from inside it.
    void broadcast(const std::string& json) {
        for (auto& c : clients_) {
            if (!c->subscribed || c->dead) continue;
            c->out += json + "\n";
            flush(*c);
        }
    }

    bool has_subscribers() const {
        for (auto& c : clients_)
            if (c->subscribed) return true;
        return false;
    }

private:
    struct Client {
        int fd = -1;
        std::string in, out;
        bool subscribed = false, dead = false;
    };

    void accept_all() {
        for (;;) {
            const int fd = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) return;
            ucred cred{};
            socklen_t len = sizeof cred;
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid() ||
                clients_.size() >= kMaxClients) {
                close(fd);
                continue;
            }
            auto c = std::make_unique<Client>();
            c->fd = fd;
            clients_.push_back(std::move(c));
        }
    }

    template <class F> void read_from(Client& c, F&& on_line) {
        char buf[4096];
        for (;;) {
            const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.in.append(buf, (size_t)n);
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) c.dead = true;
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        size_t nl;
        while ((nl = c.in.find('\n')) != std::string::npos) {
            const std::string line = c.in.substr(0, nl);
            c.in.erase(0, nl + 1);
            c.out += on_line(line, c.subscribed);
        }
        if (c.in.size() > kMaxLine) c.dead = true;  // not a well-behaved client
    }

    void flush(Client& c) {
        while (!c.out.empty() && !c.dead) {
            const ssize_t n = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                c.out.erase(0, (size_t)n);
            } else {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true;
                break;
            }
        }
        if (c.out.size() > kMaxBacklog) c.dead = true;  // a client that stopped reading
    }

    void reap() {
        for (size_t i = 0; i < clients_.size();) {
            if (clients_[i]->dead) {
                close(clients_[i]->fd);
                clients_.erase(clients_.begin() + (long)i);
                if (i < polled_) --polled_;
            } else {
                ++i;
            }
        }
    }

    static constexpr size_t kMaxClients = 8;
    static constexpr size_t kMaxLine = 64 * 1024;
    static constexpr size_t kMaxBacklog = 1024 * 1024;

    int fd_ = -1;
    std::string path_;
    std::vector<std::unique_ptr<Client>> clients_;
    size_t first_ = 0, polled_ = 0;
};

}  // namespace fg
