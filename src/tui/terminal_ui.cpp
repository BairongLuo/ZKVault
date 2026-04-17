#include "tui/terminal_ui.hpp"

#include <chrono>
#include <cerrno>
#include <iostream>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <termios.h>
#include <utility>
#include <unistd.h>

#include "app/frontend_contract.hpp"
#include "crypto/secure_memory.hpp"
#include "shell/shell_runtime.hpp"
#include "terminal/display.hpp"

namespace {

struct TuiPendingCommand {
    bool active = false;
    FrontendCommandKind kind = FrontendCommandKind::kHelp;
    std::string name;
};

inline void Cleanse(TuiPendingCommand& command) {
    ::Cleanse(command.name);
}

struct TuiRenderState {
    std::string status_message;
    TuiPendingCommand pending_command;
};

inline void Cleanse(TuiRenderState& state) {
    ::Cleanse(state.status_message);
    Cleanse(state.pending_command);
}

enum class TuiKey {
    kUnknown,
    kMoveUp,
    kMoveDown,
    kShowSelection,
    kHelp,
    kBrowse,
    kLock,
    kUnlock,
    kQuit
};

enum class TuiInputStatus {
    kKey,
    kEof,
    kTimedOut
};

struct TuiInputEvent {
    TuiInputStatus status = TuiInputStatus::kKey;
    TuiKey key = TuiKey::kUnknown;
};

class ScopedAlternateScreen {
public:
    ScopedAlternateScreen() {
        active_ = ShouldEmitTerminalControlSequences(STDOUT_FILENO);
        if (!active_) {
            return;
        }

        std::cout << BuildEnterAlternateScreenSequence()
                  << BuildClearScreenSequence();
        std::cout.flush();
    }

    ScopedAlternateScreen(const ScopedAlternateScreen&) = delete;
    ScopedAlternateScreen& operator=(const ScopedAlternateScreen&) = delete;

    ~ScopedAlternateScreen() {
        if (!active_) {
            return;
        }

        std::cout << BuildExitAlternateScreenSequence();
        std::cout.flush();
    }

private:
    bool active_ = false;
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

private:
    int fd_;
    termios settings_{};
    bool active_ = true;
};

void ReplaceStatusMessage(
    TuiRenderState& state,
    std::string next_message) {
    ::Cleanse(state.status_message);
    state.status_message = std::move(next_message);
}

void ClearStatusMessage(TuiRenderState& state) {
    ::Cleanse(state.status_message);
    state.status_message.clear();
}

void ClearPendingCommand(TuiRenderState& state) {
    Cleanse(state.pending_command);
    state.pending_command.name.clear();
    state.pending_command.active = false;
    state.pending_command.kind = FrontendCommandKind::kHelp;
}

void ReplacePendingCommand(
    TuiRenderState& state,
    const FrontendCommand& command) {
    ClearPendingCommand(state);
    state.pending_command.active = true;
    state.pending_command.kind = command.kind;
    state.pending_command.name = command.name;
}

bool ShouldPreviewPreparedCommand(
    const ShellRuntimeState& runtime,
    const FrontendCommand& command) {
    switch (command.kind) {
        case FrontendCommandKind::kUnlock:
            return !runtime.session.has_value();
        case FrontendCommandKind::kAdd:
        case FrontendCommandKind::kUpdate:
        case FrontendCommandKind::kDelete:
        case FrontendCommandKind::kChangeMasterPassword:
            return runtime.session.has_value();
        default:
            return false;
    }
}

std::string BuildPendingStatusMessage(const FrontendCommand& command) {
    switch (command.kind) {
        case FrontendCommandKind::kUnlock:
            return "awaiting master password";
        case FrontendCommandKind::kAdd:
            return "collecting fields for entry " + command.name;
        case FrontendCommandKind::kUpdate:
            return "awaiting overwrite confirmation for " + command.name;
        case FrontendCommandKind::kDelete:
            return "awaiting deletion confirmation for " + command.name;
        case FrontendCommandKind::kChangeMasterPassword:
            return "awaiting master password rotation confirmation";
        default:
            return "";
    }
}

std::string RenderTuiStatusMessage(const FrontendActionResult& result) {
    if (result.state == FrontendSessionState::kShowingHelp ||
        result.state == FrontendSessionState::kShowingList ||
        result.state == FrontendSessionState::kShowingEntry ||
        result.payload_kind == FrontendPayloadKind::kNone) {
        return "";
    }

    return RenderFrontendActionResult(result);
}

int WaitForTerminalInput(
    const std::optional<std::chrono::milliseconds>& timeout) {
    pollfd read_fd{
        STDIN_FILENO,
        POLLIN,
        0
    };

    while (true) {
        const int result = ::poll(
            &read_fd,
            1,
            timeout.has_value() ? static_cast<int>(timeout->count()) : -1);
        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0) {
            throw std::runtime_error("failed to wait for tui input");
        }

        return result;
    }
}

std::optional<unsigned char> TryReadNextByte(
    std::chrono::milliseconds timeout) {
    if (WaitForTerminalInput(timeout) == 0) {
        return std::nullopt;
    }

    while (true) {
        unsigned char ch = 0;
        const ssize_t bytes_read = ::read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_read < 0) {
            throw std::runtime_error("failed to read tui input");
        }

        if (bytes_read == 0) {
            return std::nullopt;
        }

        return ch;
    }
}

TuiKey DecodeEscapeSequence() {
    const std::optional<unsigned char> second =
        TryReadNextByte(std::chrono::milliseconds(10));
    if (!second.has_value()) {
        return TuiKey::kBrowse;
    }

    if (*second != '[') {
        return TuiKey::kUnknown;
    }

    const std::optional<unsigned char> third =
        TryReadNextByte(std::chrono::milliseconds(10));
    if (!third.has_value()) {
        return TuiKey::kUnknown;
    }

    if (*third == 'A') {
        return TuiKey::kMoveUp;
    }

    if (*third == 'B') {
        return TuiKey::kMoveDown;
    }

    return TuiKey::kUnknown;
}

TuiInputEvent ReadTuiInput(
    const std::optional<std::chrono::milliseconds>& idle_timeout) {
    if (::isatty(STDIN_FILENO) == 0) {
        throw std::runtime_error("tui requires interactive terminal");
    }

    termios old_settings{};
    if (::tcgetattr(STDIN_FILENO, &old_settings) != 0) {
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

    if (::tcsetattr(STDIN_FILENO, TCSANOW, &new_settings) != 0) {
        throw std::runtime_error("failed to configure tui input mode");
    }

    ScopedTerminalSettings restore_settings(STDIN_FILENO, old_settings);
    if (WaitForTerminalInput(idle_timeout) == 0) {
        return {TuiInputStatus::kTimedOut, TuiKey::kUnknown};
    }

    while (true) {
        unsigned char ch = 0;
        const ssize_t bytes_read = ::read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_read < 0) {
            throw std::runtime_error("failed to read tui input");
        }

        if (bytes_read == 0 || ch == 4) {
            return {TuiInputStatus::kEof, TuiKey::kUnknown};
        }

        if (ch == '\n' || ch == '\r') {
            return {TuiInputStatus::kKey, TuiKey::kShowSelection};
        }

        if (ch == 27) {
            return {TuiInputStatus::kKey, DecodeEscapeSequence()};
        }

        if (ch == 'j' || ch == 'J') {
            return {TuiInputStatus::kKey, TuiKey::kMoveDown};
        }

        if (ch == 'k' || ch == 'K') {
            return {TuiInputStatus::kKey, TuiKey::kMoveUp};
        }

        if (ch == '?') {
            return {TuiInputStatus::kKey, TuiKey::kHelp};
        }

        if (ch == 'l' || ch == 'L') {
            return {TuiInputStatus::kKey, TuiKey::kLock};
        }

        if (ch == 'u' || ch == 'U') {
            return {TuiInputStatus::kKey, TuiKey::kUnlock};
        }

        if (ch == 'q' || ch == 'Q') {
            return {TuiInputStatus::kKey, TuiKey::kQuit};
        }

        return {TuiInputStatus::kKey, TuiKey::kUnknown};
    }
}

void ActivateBrowseView(ShellRuntimeState& runtime) {
    if (!runtime.session.has_value()) {
        return;
    }

    FrontendActionResult result = ShowCurrentShellBrowseView(runtime);
    auto result_guard = MakeScopedCleanse(result);
}

void RenderBrowseSnapshot(const ShellBrowseSnapshot& snapshot) {
    std::cout << "Entries";
    if (!snapshot.filter_term.empty()) {
        std::cout << " [filter: " << snapshot.filter_term << "]";
    }
    std::cout << ":\n";

    if (snapshot.entry_names.empty()) {
        std::cout << snapshot.empty_message << '\n';
        return;
    }

    for (const std::string& entry_name : snapshot.entry_names) {
        const bool is_selected =
            !snapshot.selected_name.empty() &&
            entry_name == snapshot.selected_name;
        std::cout << (is_selected ? "> " : "  ") << entry_name << '\n';
    }
}

std::string_view DescribeCurrentView(const ShellRuntimeState& runtime) {
    switch (runtime.state_machine.state()) {
        case FrontendSessionState::kShowingHelp:
            return "help";
        case FrontendSessionState::kShowingList:
            return "list";
        case FrontendSessionState::kShowingEntry:
            return "entry";
        case FrontendSessionState::kLocked:
            return "locked";
        case FrontendSessionState::kReady:
            return "ready";
        case FrontendSessionState::kUnlockingSession:
            return "unlock";
        case FrontendSessionState::kEditingEntryForm:
            return "edit-entry";
        case FrontendSessionState::kEditingMasterPasswordForm:
            return "edit-master-password";
        case FrontendSessionState::kConfirmingEntryOverwrite:
            return "confirm-overwrite";
        case FrontendSessionState::kConfirmingEntryDeletion:
            return "confirm-delete";
        case FrontendSessionState::kConfirmingMasterPasswordRotation:
            return "confirm-master-password-rotation";
        case FrontendSessionState::kRecoveringFromFailure:
            return "recovering";
        default:
            return "transient";
    }
}

void RenderStatusSection(std::string_view status_message) {
    std::cout << "Status:\n";
    if (status_message.empty()) {
        std::cout << "(none)\n";
        return;
    }

    std::cout << status_message << '\n';
}

void RenderHelpView() {
    std::cout << "Keys:\n";
    std::cout << "  Down / j  move selection forward\n";
    std::cout << "  Up / k    move selection backward\n";
    std::cout << "  Enter     view the selected entry\n";
    std::cout << "  Esc       return to the browse list\n";
    std::cout << "  ?         show this help screen\n";
    std::cout << "  l         lock the vault\n";
    std::cout << "  u         unlock the vault\n";
    std::cout << "  q         quit the TUI\n";
    std::cout << "\nMutating actions still use the CLI or `zkvault shell` for now.\n";
}

void RenderListView(const ShellBrowseSnapshot& snapshot) {
    if (snapshot.entry_names.empty()) {
        std::cout << "No entries selected. Use the CLI or `zkvault shell` to add one.\n";
        return;
    }

    if (snapshot.selected_name.empty()) {
        std::cout << "Use Up/Down or j/k to choose an entry.\n";
        return;
    }

    std::cout << "Current selection: " << snapshot.selected_name << '\n';
    std::cout << "Press Enter to open the selected entry.\n";
}

void RenderEntryView(const ShellRuntimeState& runtime) {
    if (!runtime.session.has_value() || runtime.view_context.entry_name.empty()) {
        std::cout << "No entry details available.\n";
        return;
    }

    PasswordEntry entry =
        runtime.session->LoadEntry(runtime.view_context.entry_name);
    auto entry_guard = MakeScopedCleanse(entry);
    FrontendActionResult result = BuildShowEntryResult(std::move(entry));
    auto result_guard = MakeScopedCleanse(result);
    std::string rendered = RenderFrontendActionResult(result);
    auto rendered_guard = MakeScopedCleanse(rendered);
    std::cout << rendered << '\n';
}

void RenderReadyView(const ShellBrowseSnapshot& snapshot) {
    if (snapshot.entry_names.empty()) {
        std::cout << "Vault ready. No entries available yet.\n";
        return;
    }

    std::cout << "Vault ready. Press Esc to return to the browse list.\n";
}

void RenderLockedView() {
    std::cout << "Vault locked. Press `u` to reopen the session.\n";
}

void RenderUnlockView() {
    std::cout << "Unlock the vault with the current master password.\n";
    std::cout << "Prompt: Master password (masked)\n";
}

void RenderFormPlaceholder(std::string_view label) {
    std::cout << label << '\n';
    std::cout << "Interactive editing forms are reserved for the next TUI increment.\n";
}

void RenderViewSection(
    const ShellRuntimeState& runtime,
    const ShellBrowseSnapshot& snapshot) {
    std::cout << "View: " << DescribeCurrentView(runtime) << '\n';

    switch (runtime.state_machine.state()) {
        case FrontendSessionState::kShowingHelp:
            RenderHelpView();
            return;
        case FrontendSessionState::kShowingEntry:
            RenderEntryView(runtime);
            return;
        case FrontendSessionState::kLocked:
            RenderLockedView();
            return;
        case FrontendSessionState::kUnlockingSession:
            RenderUnlockView();
            return;
        case FrontendSessionState::kShowingList:
            RenderListView(snapshot);
            return;
        case FrontendSessionState::kEditingEntryForm:
            RenderFormPlaceholder("Entry form preview");
            return;
        case FrontendSessionState::kEditingMasterPasswordForm:
            RenderFormPlaceholder("Master password form preview");
            return;
        case FrontendSessionState::kConfirmingEntryOverwrite:
            RenderFormPlaceholder("Overwrite confirmation preview");
            return;
        case FrontendSessionState::kConfirmingEntryDeletion:
            RenderFormPlaceholder("Deletion confirmation preview");
            return;
        case FrontendSessionState::kConfirmingMasterPasswordRotation:
            RenderFormPlaceholder("Master password rotation confirmation preview");
            return;
        case FrontendSessionState::kReady:
            RenderReadyView(snapshot);
            return;
        case FrontendSessionState::kRecoveringFromFailure:
            std::cout << "Recovering the last stable view...\n";
            return;
        default:
            std::cout << "Completing interactive command...\n";
            return;
    }
}

void RenderScreen(
    const ShellRuntimeState& runtime,
    const TuiRenderState& render_state) {
    if (ShouldEmitTerminalControlSequences(STDOUT_FILENO)) {
        std::cout << BuildClearScreenSequence();
    }

    std::cout << "ZKVault TUI Prototype\n";
    std::cout << "Session: "
              << (ShellSessionUnlocked(runtime) ? "unlocked" : "locked")
              << " | State: " << DescribeCurrentView(runtime)
              << "\n\n";

    RenderStatusSection(render_state.status_message);
    std::cout << '\n';

    ShellBrowseSnapshot snapshot = SnapshotShellBrowseState(runtime);
    auto snapshot_guard = MakeScopedCleanse(snapshot);
    std::cout << "Browse:\n";
    RenderBrowseSnapshot(snapshot);
    std::cout << '\n';

    RenderViewSection(runtime, snapshot);

    std::cout << "\nKeys: Up/Down or j/k move, Enter shows, Esc browses, ? help, l lock, u unlock, q quit.\n";
    std::cout.flush();
}

std::optional<FrontendCommand> ResolveTuiCommand(
    const ShellRuntimeState& runtime,
    TuiKey key) {
    switch (key) {
        case TuiKey::kMoveUp:
            if (!runtime.session.has_value()) {
                return std::nullopt;
            }
            return FrontendCommand{FrontendCommandKind::kPrev, ""};
        case TuiKey::kMoveDown:
            if (!runtime.session.has_value()) {
                return std::nullopt;
            }
            return FrontendCommand{FrontendCommandKind::kNext, ""};
        case TuiKey::kShowSelection:
            if (!runtime.session.has_value()) {
                return std::nullopt;
            }
            return FrontendCommand{FrontendCommandKind::kShow, ""};
        case TuiKey::kHelp:
            return FrontendCommand{FrontendCommandKind::kHelp, ""};
        case TuiKey::kBrowse:
            if (!runtime.session.has_value()) {
                return std::nullopt;
            }
            return FrontendCommand{FrontendCommandKind::kList, ""};
        case TuiKey::kLock:
            return FrontendCommand{FrontendCommandKind::kLock, ""};
        case TuiKey::kUnlock:
            return FrontendCommand{FrontendCommandKind::kUnlock, ""};
        case TuiKey::kQuit:
            return FrontendCommand{FrontendCommandKind::kQuit, ""};
        default:
            return std::nullopt;
    }
}

}  // namespace

int RunTerminalUi() {
    ScopedAlternateScreen alternate_screen;

    OpenShellRuntimeResult open_result = OpenOrInitializeShellRuntime();
    auto open_result_guard = MakeScopedCleanse(open_result);
    ShellRuntimeState& runtime = open_result.runtime;
    const std::optional<std::chrono::milliseconds> idle_timeout =
        ReadShellIdleTimeout();

    FrontendActionResult ready_result = BuildTuiReadyResult();
    auto ready_result_guard = MakeScopedCleanse(ready_result);
    static_cast<void>(runtime.state_machine.ApplyActionResult(ready_result));
    ActivateBrowseView(runtime);

    TuiRenderState render_state;
    auto render_state_guard = MakeScopedCleanse(render_state);

    std::string initial_status;
    if (open_result.startup_result.has_value()) {
        initial_status = RenderTuiStatusMessage(*open_result.startup_result);
    }

    const std::string ready_status = RenderTuiStatusMessage(ready_result);
    if (!initial_status.empty() && !ready_status.empty()) {
        initial_status += '\n';
    }
    initial_status += ready_status;
    ReplaceStatusMessage(render_state, std::move(initial_status));

    while (true) {
        RenderScreen(runtime, render_state);

        const TuiInputEvent input_event = ReadTuiInput(
            ShellSessionUnlocked(runtime) ? idle_timeout : std::nullopt);
        if (input_event.status == TuiInputStatus::kTimedOut) {
            FrontendActionResult result = HandleShellIdleTimeout(runtime);
            auto result_guard = MakeScopedCleanse(result);
            ReplaceStatusMessage(
                render_state,
                RenderTuiStatusMessage(result));
            continue;
        }

        if (input_event.status == TuiInputStatus::kEof) {
            std::cout << '\n';
            return 0;
        }

        const std::optional<FrontendCommand> command =
            ResolveTuiCommand(runtime, input_event.key);
        if (!command.has_value()) {
            continue;
        }

        try {
            FrontendActionResult result{};
            if (input_event.key == TuiKey::kBrowse) {
                result = ShowCurrentShellBrowseView(runtime);
            } else if (ShouldPreviewPreparedCommand(runtime, *command)) {
                ReplacePendingCommand(render_state, *command);
                ReplaceStatusMessage(
                    render_state,
                    BuildPendingStatusMessage(*command));
                static_cast<void>(runtime.state_machine.HandleCommand(command->kind));
                RenderScreen(runtime, render_state);
                result = ExecutePreparedShellCommand(runtime, *command);
                if (command->kind == FrontendCommandKind::kUnlock &&
                    runtime.session.has_value()) {
                    ActivateBrowseView(runtime);
                }
                ClearPendingCommand(render_state);
            } else {
                result = ExecuteShellCommand(runtime, *command);
            }
            auto result_guard = MakeScopedCleanse(result);
            const std::string status_message = RenderTuiStatusMessage(result);
            if (status_message.empty()) {
                ClearStatusMessage(render_state);
            } else {
                ReplaceStatusMessage(render_state, status_message);
            }
            if (runtime.state_machine.state() ==
                FrontendSessionState::kQuitRequested) {
                return 0;
            }
        } catch (const std::exception& ex) {
            FrontendError error = ClassifyFrontendError(ex.what());
            std::string output = RenderFrontendError(error);
            auto error_guard = MakeScopedCleanse(error);
            auto output_guard = MakeScopedCleanse(output);
            ClearPendingCommand(render_state);
            static_cast<void>(RecoverShellViewAfterFailure(runtime));
            ReplaceStatusMessage(render_state, std::move(output));
        }
    }
}
