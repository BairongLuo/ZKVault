#include <array>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>

#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ~ScopedFd() {
        Reset();
    }

    int Get() const noexcept {
        return fd_;
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

class ScopedChildProcess {
public:
    explicit ScopedChildProcess(pid_t pid = -1) noexcept : pid_(pid) {}

    ScopedChildProcess(const ScopedChildProcess&) = delete;
    ScopedChildProcess& operator=(const ScopedChildProcess&) = delete;

    ~ScopedChildProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
    }

    pid_t Get() const noexcept {
        return pid_;
    }

    void Release() noexcept {
        pid_ = -1;
    }

private:
    pid_t pid_;
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

void ReadUntilContains(
    int fd,
    std::string_view needle,
    std::chrono::milliseconds timeout,
    std::string& output,
    std::size_t& cursor,
    std::string_view step_name) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 256> buffer{};

    while (output.find(needle, cursor) == std::string::npos) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error(
                "timed out waiting for shell output during " +
                std::string(step_name) + "; captured output: " + output);
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        pollfd read_fd{
            fd,
            POLLIN,
            0
        };
        int poll_result = 0;
        do {
            poll_result = ::poll(
                &read_fd,
                1,
                static_cast<int>(remaining.count()));
        } while (poll_result < 0 && errno == EINTR);

        if (poll_result < 0) {
            throw std::runtime_error("failed to wait for shell output");
        }

        if (poll_result == 0) {
            continue;
        }

        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error("failed to read shell output");
        }

        if (count == 0) {
            break;
        }

        output.append(buffer.data(), static_cast<std::size_t>(count));
    }

    cursor = output.find(needle, cursor);
    cursor += needle.size();
}

std::filesystem::path MakeTempDirectory() {
    char path_template[] = "/tmp/zkvault-shell-idle-XXXXXX";
    char* created = ::mkdtemp(path_template);
    if (created == nullptr) {
        throw std::runtime_error("failed to create temporary directory");
    }

    return std::filesystem::path(created);
}

void TestShellIdleLock(const char* binary_path) {
    const std::filesystem::path temp_dir = MakeTempDirectory();
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    };

    int master_fd = -1;
    const pid_t child_pid = ::forkpty(&master_fd, nullptr, nullptr, nullptr);
    if (child_pid < 0) {
        cleanup();
        throw std::runtime_error("failed to fork pseudo-terminal shell");
    }

    if (child_pid == 0) {
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS", "1", 1);
        if (::chdir(temp_dir.c_str()) != 0) {
            std::perror("chdir");
            std::_Exit(1);
        }

        ::execl(binary_path, binary_path, "shell", nullptr);
        std::perror("execl");
        std::_Exit(1);
    }

    ScopedFd master(master_fd);
    ScopedChildProcess child(child_pid);

    std::string output;
    std::size_t cursor = 0;
    ReadUntilContains(
        master.Get(),
        "Vault not initialized. Create one now? [y/N]: ",
        std::chrono::seconds(2),
        output,
        cursor,
        "startup confirmation prompt");
    WriteAll(master.Get(), "y\n");

    ReadUntilContains(
        master.Get(),
        "Master password: ",
        std::chrono::seconds(2),
        output,
        cursor,
        "initial master password prompt");
    WriteAll(master.Get(), "test-master-password\n");

    ReadUntilContains(
        master.Get(),
        "Confirm master password: ",
        std::chrono::seconds(2),
        output,
        cursor,
        "master password confirmation prompt");
    WriteAll(master.Get(), "test-master-password\n");

    ReadUntilContains(
        master.Get(),
        "shell ready; type help for commands",
        std::chrono::seconds(3),
        output,
        cursor,
        "shell ready banner");
    ReadUntilContains(
        master.Get(),
        "zkvault> ",
        std::chrono::seconds(2),
        output,
        cursor,
        "initial shell prompt");

    ReadUntilContains(
        master.Get(),
        "vault locked due to inactivity",
        std::chrono::seconds(3),
        output,
        cursor,
        "idle-lock message");
    Require(output.find("\x1b[2J\x1b[H") != std::string::npos,
            "idle timeout should clear the terminal before reporting lock");

    ReadUntilContains(
        master.Get(),
        "zkvault> ",
        std::chrono::seconds(2),
        output,
        cursor,
        "post-lock shell prompt");
    WriteAll(master.Get(), "unlock\n");

    ReadUntilContains(
        master.Get(),
        "Master password: ",
        std::chrono::seconds(2),
        output,
        cursor,
        "unlock master password prompt");
    WriteAll(master.Get(), "test-master-password\n");

    ReadUntilContains(
        master.Get(),
        "vault unlocked",
        std::chrono::seconds(2),
        output,
        cursor,
        "unlock result");
    ReadUntilContains(
        master.Get(),
        "zkvault> ",
        std::chrono::seconds(2),
        output,
        cursor,
        "post-unlock shell prompt");
    WriteAll(master.Get(), "quit\n");

    int status = 0;
    if (::waitpid(child.Get(), &status, 0) < 0) {
        cleanup();
        throw std::runtime_error("failed to wait for shell child");
    }

    child.Release();
    cleanup();

    Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "shell should exit cleanly after idle lock and unlock");
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: shell_idle_lock_test <zkvault-binary>");
        }

        TestShellIdleLock(argv[1]);
        return 0;
    } catch (const std::exception& ex) {
        return (std::fprintf(stderr, "shell idle lock test failed: %s\n", ex.what()), 1);
    }
}
