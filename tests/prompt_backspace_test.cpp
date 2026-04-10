#include <chrono>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <pty.h>
#include <unistd.h>

#include "terminal/prompt.hpp"

namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }

        return *this;
    }

    ~ScopedFd() {
        Reset();
    }

    int Get() const noexcept {
        return fd_;
    }

    int Release() noexcept {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    void Reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_;
};

class ScopedFdRedirect {
public:
    ScopedFdRedirect(int target_fd, int replacement_fd)
        : target_fd_(target_fd), saved_fd_(::dup(target_fd)) {
        if (saved_fd_.Get() < 0) {
            throw std::runtime_error("failed to duplicate file descriptor");
        }

        if (::dup2(replacement_fd, target_fd_) != target_fd_) {
            throw std::runtime_error("failed to redirect file descriptor");
        }
    }

    ScopedFdRedirect(const ScopedFdRedirect&) = delete;
    ScopedFdRedirect& operator=(const ScopedFdRedirect&) = delete;

    ~ScopedFdRedirect() {
        if (saved_fd_.Get() >= 0) {
            ::dup2(saved_fd_.Get(), target_fd_);
        }
    }

private:
    int target_fd_;
    ScopedFd saved_fd_;
};

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void WriteAll(int fd, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t chunk =
            ::write(fd, data.data() + written, data.size() - written);
        if (chunk <= 0) {
            throw std::runtime_error("failed to write pseudo-terminal input");
        }
        written += static_cast<std::size_t>(chunk);
    }
}

std::string ReadAvailable(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        throw std::runtime_error("failed to read pseudo-terminal flags");
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("failed to enable nonblocking reads");
    }

    std::string output;
    char buffer[256];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }

        if (count == 0) {
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        throw std::runtime_error("failed to read pseudo-terminal output");
    }

    return output;
}

std::pair<std::string, std::string> RunPromptInput(
    std::string_view input,
    const std::function<std::string()>& reader) {
    int master_fd = -1;
    int slave_fd = -1;
    if (::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
        throw std::runtime_error("failed to open pseudo-terminal");
    }

    ScopedFd master(master_fd);
    ScopedFd slave(slave_fd);
    ScopedFdRedirect redirect_stdin(STDIN_FILENO, slave.Get());
    ScopedFdRedirect redirect_stdout(STDOUT_FILENO, slave.Get());

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        WriteAll(master.Get(), input);
    });

    std::string result = reader();
    writer.join();

    const std::string terminal_output = ReadAvailable(master.Get());
    return {result, terminal_output};
}

std::pair<std::string, std::string> RunSecretInput(std::string_view input) {
    return RunPromptInput(input, [] {
        return ReadSecret("Password: ");
    });
}

std::pair<std::string, std::string> RunLineInput(std::string_view input) {
    return RunPromptInput(input, [] {
        return ReadLine("Value: ");
    });
}

std::pair<PromptReadStatus, std::string> RunTimedLineInput(
    std::chrono::milliseconds timeout) {
    int master_fd = -1;
    int slave_fd = -1;
    if (::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
        throw std::runtime_error("failed to open pseudo-terminal");
    }

    ScopedFd master(master_fd);
    ScopedFd slave(slave_fd);
    ScopedFdRedirect redirect_stdin(STDIN_FILENO, slave.Get());
    ScopedFdRedirect redirect_stdout(STDOUT_FILENO, slave.Get());

    std::string value;
    const PromptReadStatus status =
        TryReadLineWithTimeout("Value: ", value, timeout);
    return {status, ReadAvailable(master.Get())};
}

std::pair<std::string, std::string> RunPromptFailure(
    std::string_view input,
    const std::function<void()>& reader) {
    int master_fd = -1;
    int slave_fd = -1;
    if (::openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
        throw std::runtime_error("failed to open pseudo-terminal");
    }

    ScopedFd master(master_fd);
    ScopedFd slave(slave_fd);
    ScopedFdRedirect redirect_stdin(STDIN_FILENO, slave.Get());
    ScopedFdRedirect redirect_stdout(STDOUT_FILENO, slave.Get());

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        WriteAll(master.Get(), input);
    });

    std::string failure_message;
    try {
        reader();
    } catch (const std::exception& ex) {
        failure_message = ex.what();
    }
    writer.join();

    if (failure_message.empty()) {
        throw std::runtime_error("expected prompt reader to fail");
    }

    const std::string terminal_output = ReadAvailable(master.Get());
    return {failure_message, terminal_output};
}

void TestBackspaceHandling() {
    const auto [secret, terminal_output] =
        RunSecretInput(std::string("ab\bcd\n", 6));
    Require(secret == "acd", "backspace should remove the previous character");
    Require(terminal_output.find("Password: **\b \b**") != std::string::npos,
            "backspace flow should mask input with '*'");
    Require(terminal_output.find("^H") == std::string::npos,
            "backspace should not echo ^H");
}

void TestDeleteHandling() {
    std::string input = "ab";
    input.push_back(static_cast<char>(127));
    input += "cd\n";

    const auto [secret, terminal_output] =
        RunSecretInput(input);
    Require(secret == "acd", "delete should remove the previous character");
    Require(terminal_output.find("Password: **\b \b**") != std::string::npos,
            "delete flow should mask input with '*'");
    Require(terminal_output.find("^?") == std::string::npos,
            "delete should not echo ^?");
}

void TestLineBackspaceHandling() {
    const auto [value, terminal_output] =
        RunLineInput(std::string("ab\bcd\n", 6));
    Require(value == "acd", "line input backspace should remove the previous character");
    Require(terminal_output.find("Value: ab\b \bcd") != std::string::npos,
            "line input should erase the visible character");
    Require(terminal_output.find("^H") == std::string::npos,
            "line input backspace should not echo ^H");
}

void TestLineDeleteHandling() {
    std::string input = "ab";
    input.push_back(static_cast<char>(127));
    input += "cd\n";

    const auto [value, terminal_output] =
        RunLineInput(input);
    Require(value == "acd", "line input delete should remove the previous character");
    Require(terminal_output.find("Value: ab\b \bcd") != std::string::npos,
            "line input should erase the visible character on delete");
    Require(terminal_output.find("^?") == std::string::npos,
            "line input delete should not echo ^?");
}

void TestSecretEofCancellation() {
    const auto [error_message, terminal_output] =
        RunPromptFailure(std::string(1, 4), [] {
            static_cast<void>(ReadSecret("Password: "));
        });
    Require(error_message == "input cancelled",
            "secret input should reject empty EOF");
    Require(terminal_output.find("Password: ") != std::string::npos,
            "secret EOF should still show the prompt");
}

void TestLineEofCancellation() {
    const auto [error_message, terminal_output] =
        RunPromptFailure(std::string(1, 4), [] {
            static_cast<void>(ReadLine("Value: "));
        });
    Require(error_message == "input cancelled",
            "line input should reject empty EOF");
    Require(terminal_output.find("Value: ") != std::string::npos,
            "line EOF should still show the prompt");
}

void TestLineTimeout() {
    const auto [status, terminal_output] =
        RunTimedLineInput(std::chrono::milliseconds(20));
    Require(status == PromptReadStatus::kTimedOut,
            "timed line input should report timeout");
    Require(terminal_output.find("Value: ") != std::string::npos,
            "timed line input should still show the prompt");
}

}  // namespace

int main() {
    try {
        TestBackspaceHandling();
        TestDeleteHandling();
        TestLineBackspaceHandling();
        TestLineDeleteHandling();
        TestSecretEofCancellation();
        TestLineEofCancellation();
        TestLineTimeout();
        return 0;
    } catch (const std::exception& ex) {
        return (std::fprintf(stderr, "prompt backspace test failed: %s\n", ex.what()), 1);
    }
}
