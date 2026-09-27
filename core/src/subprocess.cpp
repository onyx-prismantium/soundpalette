#include "subprocess.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace sp {

std::vector<std::string> split_command_line(const std::string &command) {
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    char quote = 0;
    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < command.size()) {
                cur.push_back(command[++i]);
            } else {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            in_token = true;
        } else if (c == '\\' && i + 1 < command.size()) {
            cur.push_back(command[++i]);
            in_token = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (in_token) {
                out.push_back(cur);
                cur.clear();
                in_token = false;
            }
        } else {
            cur.push_back(c);
            in_token = true;
        }
    }
    if (in_token) {
        out.push_back(cur);
    }
    return out;
}

Subprocess::~Subprocess() {
    kill();
}

bool Subprocess::running() const {
    std::lock_guard<std::mutex> lock(mu_);
    return started_ && !eof_;
}

bool Subprocess::eof() const {
    std::lock_guard<std::mutex> lock(mu_);
    return eof_ && lines_.empty();
}

bool Subprocess::read_line(std::string &line, double timeout_s) {
    std::unique_lock<std::mutex> lock(mu_);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<long long>(timeout_s * 1000.0));
    while (lines_.empty()) {
        if (eof_) {
            return false;
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout && lines_.empty()) {
            return false;
        }
    }
    line = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string &s) {
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// CommandLineToArgvW-compatible quoting.
std::wstring quote_arg(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace

bool Subprocess::start(const std::vector<std::string> &argv, std::string &err) {
    if (argv.empty()) {
        err = "empty command";
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
        err = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            cmdline += L' ';
        }
        cmdline += quote_arg(widen(argv[i]));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        err = "cannot start '" + argv[0] + "' (CreateProcess error " +
              std::to_string(GetLastError()) + ")";
        CloseHandle(in_w);
        CloseHandle(out_r);
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    stdin_write_ = in_w;
    stdout_read_ = out_r;
    {
        std::lock_guard<std::mutex> lock(mu_);
        started_ = true;
        eof_ = false;
        lines_.clear();
        partial_.clear();
    }
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

bool Subprocess::write_line(const std::string &line) {
    if (!stdin_write_) {
        return false;
    }
    std::string data = line + "\n";
    DWORD written = 0;
    const char *p = data.data();
    DWORD left = static_cast<DWORD>(data.size());
    while (left > 0) {
        if (!WriteFile(stdin_write_, p, left, &written, nullptr)) {
            return false;
        }
        p += written;
        left -= written;
    }
    return true;
}

void Subprocess::reader_loop() {
    char buf[4096];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(stdout_read_, buf, sizeof buf, &got, nullptr) || got == 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(mu_);
        partial_.append(buf, got);
        std::size_t nl;
        while ((nl = partial_.find('\n')) != std::string::npos) {
            std::string l = partial_.substr(0, nl);
            if (!l.empty() && l.back() == '\r') {
                l.pop_back();
            }
            lines_.push_back(std::move(l));
            partial_.erase(0, nl + 1);
        }
        cv_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mu_);
    eof_ = true;
    cv_.notify_all();
}

void Subprocess::kill() {
    if (process_) {
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 2000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (stdout_read_) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    eof_ = true;
    cv_.notify_all();
}

int Subprocess::close_and_wait(double timeout_s) {
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    int code = -1;
    if (process_) {
        DWORD ms = static_cast<DWORD>(timeout_s * 1000.0);
        if (WaitForSingleObject(process_, ms) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(process_, &ec);
            code = static_cast<int>(ec);
        }
    }
    kill();
    return code;
}

#else // POSIX

bool Subprocess::start(const std::vector<std::string> &argv, std::string &err) {
    if (argv.empty()) {
        err = "empty command";
        return false;
    }
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        err = "pipe() failed";
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    std::vector<char *> cargv;
    for (const std::string &a : argv) {
        cargv.push_back(const_cast<char *>(a.c_str()));
    }
    cargv.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (rc != 0) {
        err = "cannot start '" + argv[0] + "': " + std::strerror(rc);
        close(in_pipe[1]);
        close(out_pipe[0]);
        return false;
    }
    pid_ = pid;
    stdin_write_ = in_pipe[1];
    stdout_read_ = out_pipe[0];
    {
        std::lock_guard<std::mutex> lock(mu_);
        started_ = true;
        eof_ = false;
        lines_.clear();
        partial_.clear();
    }
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

bool Subprocess::write_line(const std::string &line) {
    if (stdin_write_ < 0) {
        return false;
    }
    std::string data = line + "\n";
    const char *p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t n = ::write(stdin_write_, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

void Subprocess::reader_loop() {
    char buf[4096];
    for (;;) {
        ssize_t got = ::read(stdout_read_, buf, sizeof buf);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(mu_);
        partial_.append(buf, static_cast<std::size_t>(got));
        std::size_t nl;
        while ((nl = partial_.find('\n')) != std::string::npos) {
            lines_.push_back(partial_.substr(0, nl));
            partial_.erase(0, nl + 1);
        }
        cv_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mu_);
    eof_ = true;
    cv_.notify_all();
}

void Subprocess::kill() {
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    if (stdin_write_ >= 0) {
        close(stdin_write_);
        stdin_write_ = -1;
    }
    if (stdout_read_ >= 0) {
        close(stdout_read_);
        stdout_read_ = -1;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    eof_ = true;
    cv_.notify_all();
}

int Subprocess::close_and_wait(double timeout_s) {
    if (stdin_write_ >= 0) {
        close(stdin_write_);
        stdin_write_ = -1;
    }
    int code = -1;
    if (pid_ > 0) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(static_cast<long long>(timeout_s * 1000.0));
        for (;;) {
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                pid_ = -1;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            usleep(20000);
        }
    }
    kill();
    return code;
}

#endif

} // namespace sp
