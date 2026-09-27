#pragma once

// Minimal cross-platform child process with a stdin pipe and a line-oriented stdout reader
// thread (extension-4 §7.1 annotator transport). No shell is ever involved: the caller passes
// an argv vector. stderr is inherited so annotator diagnostics reach the user's terminal.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sp {

class Subprocess {
public:
    Subprocess() = default;
    ~Subprocess();
    Subprocess(const Subprocess &) = delete;
    Subprocess &operator=(const Subprocess &) = delete;

    // Spawns argv[0] with argv as arguments (argv[0] resolved through PATH). false + err on
    // failure to start.
    bool start(const std::vector<std::string> &argv, std::string &err);
    bool running() const;

    // Writes one line (appends '\n'). false if the child is gone.
    bool write_line(const std::string &line);

    // Pops the next complete stdout line, waiting up to timeout_s. Returns false on timeout
    // or when the child closed its stdout and the queue is drained (check `eof()`).
    bool read_line(std::string &line, double timeout_s);
    bool eof() const;

    // Forcibly terminates the child and joins the reader thread. Safe to call twice.
    void kill();
    // Closes stdin, waits for a natural exit (up to timeout_s), then kills. Returns exit code
    // or -1.
    int close_and_wait(double timeout_s);

private:
    void reader_loop();

#ifdef _WIN32
    void *process_ = nullptr; // HANDLE
    void *stdin_write_ = nullptr;
    void *stdout_read_ = nullptr;
#else
    int pid_ = -1;
    int stdin_write_ = -1;
    int stdout_read_ = -1;
#endif
    std::thread reader_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    std::string partial_;
    bool eof_ = false;
    bool started_ = false;
};

// Splits a command string into argv without a shell: whitespace separates, single or double
// quotes group, backslash escapes the next character (outside single quotes).
std::vector<std::string> split_command_line(const std::string &command);

} // namespace sp
