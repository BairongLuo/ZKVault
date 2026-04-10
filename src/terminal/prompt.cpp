#include "terminal/prompt.hpp"

#include <chrono>
#include <cerrno>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>

#include "crypto/secure_memory.hpp"

namespace {

constexpr const char* kInputCancelledMessage = "input cancelled";

enum class InputEchoMode {
    kVisible,
    kMasked,
};

struct InteractiveInputResult {
    std::string value;
    PromptReadStatus status = PromptReadStatus::kRead;
};

class ScopedTerminalSettings {
public:
    ScopedTerminalSettings(int fd, const termios& settings) noexcept
        : fd_(fd), settings_(settings) {}

    ScopedTerminalSettings(const ScopedTerminalSettings&) = delete;
    ScopedTerminalSettings& operator=(const ScopedTerminalSettings&) = delete;

    ~ScopedTerminalSettings() {
        if (active_) {
            ::tcsetattr(fd_, TCSANOW, &settings_);
        }
    }

    void Restore() {
        if (!active_) {
            return;
        }

        if (::tcsetattr(fd_, TCSANOW, &settings_) != 0) {
            throw std::runtime_error("failed to restore terminal settings");
        }

        active_ = false;
    }

private:
    int fd_;
    termios settings_{};
    bool active_ = true;
};

void EraseLastCharacter(std::string& value) {
    if (value.empty()) {
        return;
    }

    value.back() = '\0';
    value.pop_back();
}

void EchoMaskCharacter() {
    std::cout.put('*');
    std::cout.flush();
}

void EchoVisibleCharacter(char ch) {
    std::cout.put(ch);
    std::cout.flush();
}

void EraseEchoedCharacter() {
    std::cout << "\b \b";
    std::cout.flush();
}

void EchoInputCharacter(InputEchoMode echo_mode, char ch) {
    if (echo_mode == InputEchoMode::kMasked) {
        EchoMaskCharacter();
        return;
    }

    EchoVisibleCharacter(ch);
}

void EraseInputCharacter(InputEchoMode echo_mode, const std::string& value) {
    if (value.empty()) {
        return;
    }

    static_cast<void>(echo_mode);
    EraseEchoedCharacter();
}

InteractiveInputResult ReadInteractiveInputFromTerminal(
    InputEchoMode echo_mode,
    std::optional<std::chrono::milliseconds> timeout) {
    termios old_settings{};
    if (tcgetattr(STDIN_FILENO, &old_settings) != 0) {
        throw std::runtime_error("failed to read terminal settings");
    }

    termios new_settings = old_settings;
    new_settings.c_lflag &= ~(ECHO | ICANON);
#ifdef ECHOE
    new_settings.c_lflag &= ~ECHOE;
#endif
#ifdef ECHOK
    new_settings.c_lflag &= ~ECHOK;
#endif
#ifdef ECHONL
    new_settings.c_lflag &= ~ECHONL;
#endif
#ifdef ECHOCTL
    new_settings.c_lflag &= ~ECHOCTL;
#endif
    new_settings.c_cc[VMIN] = 1;
    new_settings.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_settings) != 0) {
        throw std::runtime_error("failed to disable terminal echo");
    }

    ScopedTerminalSettings restore_settings(STDIN_FILENO, old_settings);

    std::string value;
    while (true) {
        if (timeout.has_value()) {
            pollfd read_fd{
                STDIN_FILENO,
                POLLIN,
                0
            };
            int poll_result = 0;
            do {
                poll_result = ::poll(
                    &read_fd,
                    1,
                    static_cast<int>(timeout->count()));
            } while (poll_result < 0 && errno == EINTR);

            if (poll_result < 0) {
                throw std::runtime_error("failed to wait for terminal input");
            }

            if (poll_result == 0) {
                std::cout << '\n';
                return {std::move(value), PromptReadStatus::kTimedOut};
            }
        }

        unsigned char ch = 0;
        const ssize_t bytes_read = ::read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error("failed to read terminal input");
        }

        if (bytes_read == 0) {
            if (!value.empty()) {
                std::cout << '\n';
                return {std::move(value), PromptReadStatus::kRead};
            }

            return {std::move(value), PromptReadStatus::kEof};
        }

        if (ch == '\n' || ch == '\r') {
            std::cout << '\n';
            break;
        }

        if (ch == 4) {
            if (!value.empty()) {
                std::cout << '\n';
            }
            return {
                std::move(value),
                value.empty() ? PromptReadStatus::kEof
                              : PromptReadStatus::kRead
            };
        }

        if (ch == '\b' || ch == 127) {
            EraseInputCharacter(echo_mode, value);
            EraseLastCharacter(value);
            continue;
        }

        value.push_back(static_cast<char>(ch));
        EchoInputCharacter(echo_mode, static_cast<char>(ch));
    }

    restore_settings.Restore();
    return {std::move(value), PromptReadStatus::kRead};
}

}  // namespace

std::string ReadSecret(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

    if (!isatty(STDIN_FILENO)) {
        std::string secret;
        if (!std::getline(std::cin, secret)) {
            throw std::runtime_error(kInputCancelledMessage);
        }

        return secret;
    }

    const InteractiveInputResult result =
        ReadInteractiveInputFromTerminal(InputEchoMode::kMasked, std::nullopt);
    if (result.status == PromptReadStatus::kEof) {
        std::cout << '\n';
        throw std::runtime_error(kInputCancelledMessage);
    }

    return result.value;
}

bool TryReadLine(const std::string& prompt, std::string& value) {
    return TryReadLineWithTimeout(prompt, value, std::chrono::milliseconds(0)) ==
           PromptReadStatus::kRead;
}

PromptReadStatus TryReadLineWithTimeout(
    const std::string& prompt,
    std::string& value,
    std::chrono::milliseconds timeout) {
    std::cout << prompt;
    std::cout.flush();

    if (!isatty(STDIN_FILENO)) {
        return std::getline(std::cin, value)
                   ? PromptReadStatus::kRead
                   : PromptReadStatus::kEof;
    }

    const InteractiveInputResult result =
        ReadInteractiveInputFromTerminal(
            InputEchoMode::kVisible,
            timeout.count() > 0 ? std::optional(timeout) : std::nullopt);
    value = result.value;
    return result.status;
}

std::string ReadLine(const std::string& prompt) {
    std::string value;
    const PromptReadStatus read_status =
        TryReadLineWithTimeout(prompt, value, std::chrono::milliseconds(0));
    if (read_status != PromptReadStatus::kRead) {
        if (isatty(STDIN_FILENO)) {
            std::cout << '\n';
        }

        throw std::runtime_error(kInputCancelledMessage);
    }

    return value;
}

void RequireExactConfirmation(
    const std::string& prompt,
    const std::string& expected,
    const std::string& mismatch_error) {
    const std::string value = ReadLine(prompt);
    if (value != expected) {
        throw std::runtime_error(mismatch_error);
    }
}

std::string ReadConfirmedSecret(
    const std::string& prompt,
    const std::string& confirm_prompt,
    const std::string& mismatch_error) {
    std::string secret = ReadSecret(prompt);
    auto secret_guard = MakeScopedCleanse(secret);
    std::string confirm_secret = ReadSecret(confirm_prompt);
    auto confirm_secret_guard = MakeScopedCleanse(confirm_secret);

    if (secret != confirm_secret) {
        throw std::runtime_error(mismatch_error);
    }

    secret_guard.Release();
    return secret;
}
