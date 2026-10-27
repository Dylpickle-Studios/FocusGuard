// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard - control channel server for Windows: an overlapped named pipe
// driven from the daemon's message loop (MsgWaitForMultipleObjectsEx).
//
// The pipe is per logon session, refuses remote clients, and its DACL admits
// only the current user. Its integrity label is medium, so a settings app
// running unelevated can still talk to an elevated daemon.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <memory>

#include "control.hpp"

namespace fg {

class ControlServer {
public:
    ~ControlServer() { stop(); }

    bool start(const std::wstring& name) {
        name_ = name;
        if (!build_security()) return false;
        return listen(true);
    }

    void stop() {
        for (auto& c : clients_) close_client(*c);
        clients_.clear();
        if (sd_) {
            LocalFree(sd_);
            sd_ = nullptr;
        }
    }

    // Event handles to wait on (at most 2 per client, well under the limit).
    void handles(std::vector<HANDLE>& out) const {
        for (auto& c : clients_) {
            out.push_back(c->rev);
            if (c->writing) out.push_back(c->wev);
        }
    }

    // Finishes whatever I/O completed. Cheap enough to call on every wake-up.
    // on_line(line, subscribe&) returns the reply.
    template <class F> void poll(F&& on_line) {
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = *clients_[i];
            DWORD n = 0;
            if (!c.connected) {
                if (!GetOverlappedResult(c.pipe, &c.rov, &n, FALSE)) {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) continue;
                    c.dead = true;  // listener broke; a new one is made below
                    continue;
                }
                c.connected = true;
                start_read(c);
            }
            // Drain completed reads; each completion starts the next read.
            while (c.reading && !c.dead) {
                if (!GetOverlappedResult(c.pipe, &c.rov, &n, FALSE)) {
                    if (GetLastError() != ERROR_IO_INCOMPLETE) c.dead = true;  // client went away
                    break;
                }
                c.reading = false;
                c.in.append(c.buf, n);
                size_t nl;
                while ((nl = c.in.find('\n')) != std::string::npos) {
                    std::string line = c.in.substr(0, nl);
                    c.in.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    queue(c, on_line(line, c.subscribed));
                }
                if (c.in.size() > kMaxLine) c.dead = true;
                if (!c.dead) start_read(c);
            }
            finish_write(c);
        }
        for (size_t i = 0; i < clients_.size();) {
            if (clients_[i]->dead) {
                close_client(*clients_[i]);
                clients_.erase(clients_.begin() + (long)i);
            } else {
                ++i;
            }
        }
        // Keep exactly one instance waiting for the next client.
        bool listening = false;
        for (auto& c : clients_) listening = listening || !c->connected;
        if (!listening) listen(clients_.empty());
    }

    // Sends a line to every client that subscribed.
    void broadcast(const std::string& json) {
        for (auto& c : clients_)
            if (c->connected && c->subscribed && !c->dead) queue(*c, json + "\n");
    }

    bool has_subscribers() const {
        for (auto& c : clients_)
            if (c->subscribed) return true;
        return false;
    }

private:
    struct Client {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        HANDLE rev = nullptr, wev = nullptr;  // manual-reset events for the overlapped ops
        OVERLAPPED rov{}, wov{};
        char buf[4096];
        std::string in, out, writing_buf;
        bool connected = false, reading = false, writing = false, subscribed = false, dead = false;
    };

    // DACL: the current user only. Label: medium, so unelevated clients may write.
    bool build_security() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len);
        const bool ok = len && GetTokenInformation(token, TokenUser, buf.data(), len, &len);
        CloseHandle(token);
        if (!ok) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) return false;
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")S:(ML;;NW;;;ME)";
        LocalFree(sid);
        return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr) !=
               FALSE;
    }

    // Creates a pipe instance and waits (overlapped) for a client on it.
    bool listen(bool first) {
        if (clients_.size() >= kMaxClients) return false;
        SECURITY_ATTRIBUTES sa{sizeof sa, sd_, FALSE};
        auto c = std::make_unique<Client>();
        c->pipe = CreateNamedPipeW(name_.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (c->pipe == INVALID_HANDLE_VALUE) return false;
        c->rev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        c->wev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        c->rov.hEvent = c->rev;
        c->wov.hEvent = c->wev;
        if (!ConnectNamedPipe(c->pipe, &c->rov)) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                SetEvent(c->rev);  // already connected: make poll() see it
            } else if (err != ERROR_IO_PENDING) {
                close_client(*c);
                return false;
            }
        }
        clients_.push_back(std::move(c));
        return true;
    }

    void start_read(Client& c) {
        if (!ReadFile(c.pipe, c.buf, sizeof c.buf, nullptr, &c.rov) && GetLastError() != ERROR_IO_PENDING) {
            c.dead = true;
            return;
        }
        c.reading = true;  // completion (even an immediate one) is collected in poll()
    }

    void queue(Client& c, const std::string& data) {
        if (data.empty() || c.dead) return;
        c.out += data;
        if (c.out.size() > kMaxBacklog) c.dead = true;  // stopped reading
        finish_write(c);
    }

    // Completes a pending write if done, then starts the next one.
    void finish_write(Client& c) {
        if (c.writing) {
            DWORD n = 0;
            if (!GetOverlappedResult(c.pipe, &c.wov, &n, FALSE)) {
                if (GetLastError() != ERROR_IO_INCOMPLETE) c.dead = true;
                return;
            }
            c.writing = false;
        }
        if (c.out.empty() || c.dead || !c.connected) return;
        c.writing_buf.swap(c.out);
        c.out.clear();
        if (!WriteFile(c.pipe, c.writing_buf.data(), (DWORD)c.writing_buf.size(), nullptr, &c.wov) &&
            GetLastError() != ERROR_IO_PENDING) {
            c.dead = true;
            return;
        }
        c.writing = true;
    }

    static void close_client(Client& c) {
        if (c.pipe != INVALID_HANDLE_VALUE) {
            CancelIo(c.pipe);
            if (c.connected) DisconnectNamedPipe(c.pipe);
            // Wait for cancelled I/O so the kernel is done with our buffers.
            DWORD n = 0;
            if (c.reading || !c.connected) GetOverlappedResult(c.pipe, &c.rov, &n, TRUE);
            if (c.writing) GetOverlappedResult(c.pipe, &c.wov, &n, TRUE);
            CloseHandle(c.pipe);
            c.pipe = INVALID_HANDLE_VALUE;
        }
        if (c.rev) CloseHandle(c.rev);
        if (c.wev) CloseHandle(c.wev);
        c.rev = c.wev = nullptr;
    }

    static constexpr size_t kMaxClients = 8;
    static constexpr size_t kMaxLine = 64 * 1024;
    static constexpr size_t kMaxBacklog = 1024 * 1024;

    std::wstring name_;
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    std::vector<std::unique_ptr<Client>> clients_;
};

}  // namespace fg
