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

enum class TuiEntryFormField {
    kName,
    kPassword,
    kNote
};

struct TuiEntryFormState {
    bool active = false;
    EntryMutationMode mode = EntryMutationMode::kCreate;
    TuiEntryFormField field = TuiEntryFormField::kName;
    std::string name;
    std::string password;
    std::string note;
};

inline void Cleanse(TuiEntryFormState& state) {
    ::Cleanse(state.name);
    ::Cleanse(state.password);
    ::Cleanse(state.note);
}

enum class TuiMasterPasswordFormField {
    kNewPassword,
    kConfirmPassword
};

struct TuiMasterPasswordFormState {
    bool active = false;
    TuiMasterPasswordFormField field =
        TuiMasterPasswordFormField::kNewPassword;
    std::string new_master_password;
    std::string confirm_master_password;
};

inline void Cleanse(TuiMasterPasswordFormState& state) {
    ::Cleanse(state.new_master_password);
    ::Cleanse(state.confirm_master_password);
}

inline void Cleanse(ExactConfirmationRule& rule) {
    ::Cleanse(rule.prompt);
    ::Cleanse(rule.expected_value);
    ::Cleanse(rule.mismatch_error);
}

struct TuiExactConfirmationState {
    bool active = false;
    FrontendCommandKind kind = FrontendCommandKind::kHelp;
    ExactConfirmationRule rule;
    std::string entry_name;
    std::string typed_value;
};

inline void Cleanse(TuiExactConfirmationState& state) {
    Cleanse(state.rule);
    ::Cleanse(state.entry_name);
    ::Cleanse(state.typed_value);
}

struct TuiRenderState {
    std::string status_message;
    TuiPendingCommand pending_command;
    TuiEntryFormState entry_form;
    TuiMasterPasswordFormState master_password_form;
    TuiExactConfirmationState exact_confirmation;
};

inline void Cleanse(TuiRenderState& state) {
    ::Cleanse(state.status_message);
    Cleanse(state.pending_command);
    Cleanse(state.entry_form);
    Cleanse(state.master_password_form);
    Cleanse(state.exact_confirmation);
}

enum class TuiKey {
    kUnknown,
    kMoveUp,
    kMoveDown,
    kShowSelection,
    kHelp,
    kAdd,
    kDelete,
    kBrowse,
    kNextField,
    kBackspace,
    kLock,
    kUnlock,
    kQuit,
    kCharacter
};

enum class TuiInputStatus {
    kKey,
    kEof,
    kTimedOut
};

struct TuiInputEvent {
    TuiInputStatus status = TuiInputStatus::kKey;
    TuiKey key = TuiKey::kUnknown;
    char text = '\0';
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

void ClearEntryForm(TuiRenderState& state) {
    Cleanse(state.entry_form);
    state.entry_form.name.clear();
    state.entry_form.password.clear();
    state.entry_form.note.clear();
    state.entry_form.active = false;
    state.entry_form.mode = EntryMutationMode::kCreate;
    state.entry_form.field = TuiEntryFormField::kName;
}

void ClearMasterPasswordForm(TuiRenderState& state) {
    Cleanse(state.master_password_form);
    state.master_password_form.active = false;
    state.master_password_form.field =
        TuiMasterPasswordFormField::kNewPassword;
    state.master_password_form.new_master_password.clear();
    state.master_password_form.confirm_master_password.clear();
}

void ClearExactConfirmation(TuiRenderState& state) {
    Cleanse(state.exact_confirmation);
    state.exact_confirmation.active = false;
    state.exact_confirmation.kind = FrontendCommandKind::kHelp;
    state.exact_confirmation.rule = ExactConfirmationRule{};
    state.exact_confirmation.entry_name.clear();
    state.exact_confirmation.typed_value.clear();
}

void ReplacePendingCommand(
    TuiRenderState& state,
    const FrontendCommand& command) {
    ClearPendingCommand(state);
    state.pending_command.active = true;
    state.pending_command.kind = command.kind;
    state.pending_command.name = command.name;
}

void BeginEntryForm(
    TuiRenderState& state,
    EntryMutationMode mode,
    const std::string& entry_name) {
    ClearEntryForm(state);
    state.entry_form.active = true;
    state.entry_form.mode = mode;
    state.entry_form.field = mode == EntryMutationMode::kCreate
                                 ? TuiEntryFormField::kName
                                 : TuiEntryFormField::kPassword;
    state.entry_form.name = entry_name;
}

void PopulateEntryFormForUpdate(
    TuiRenderState& state,
    const PasswordEntry& entry) {
    BeginEntryForm(state, EntryMutationMode::kUpdate, entry.name);
    state.entry_form.password = entry.password;
    state.entry_form.note = entry.note;
}

void BeginMasterPasswordForm(TuiRenderState& state) {
    ClearMasterPasswordForm(state);
    state.master_password_form.active = true;
}

void BeginExactConfirmation(
    TuiRenderState& state,
    FrontendCommandKind kind,
    const std::string& entry_name,
    ExactConfirmationRule rule) {
    ClearExactConfirmation(state);
    state.exact_confirmation.active = true;
    state.exact_confirmation.kind = kind;
    state.exact_confirmation.entry_name = entry_name;
    state.exact_confirmation.rule = std::move(rule);
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

std::optional<std::string> SelectedBrowseEntryName(
    const ShellRuntimeState& runtime) {
    ShellBrowseSnapshot snapshot = SnapshotShellBrowseState(runtime);
    auto snapshot_guard = MakeScopedCleanse(snapshot);
    if (snapshot.selected_name.empty()) {
        return std::nullopt;
    }

    return snapshot.selected_name;
}

std::string& ActiveEntryFormFieldValue(TuiEntryFormState& state) {
    switch (state.field) {
        case TuiEntryFormField::kName:
            return state.name;
        case TuiEntryFormField::kPassword:
            return state.password;
        case TuiEntryFormField::kNote:
            return state.note;
    }

    throw std::runtime_error("unsupported tui entry form field");
}

std::string& ActiveMasterPasswordFormFieldValue(
    TuiMasterPasswordFormState& state) {
    switch (state.field) {
        case TuiMasterPasswordFormField::kNewPassword:
            return state.new_master_password;
        case TuiMasterPasswordFormField::kConfirmPassword:
            return state.confirm_master_password;
    }

    throw std::runtime_error("unsupported tui master password form field");
}

void AdvanceEntryFormField(TuiEntryFormState& state) {
    if (state.field == TuiEntryFormField::kName) {
        state.field = TuiEntryFormField::kPassword;
        return;
    }

    if (state.field == TuiEntryFormField::kPassword) {
        state.field = TuiEntryFormField::kNote;
    }
}

void AdvanceMasterPasswordFormField(TuiMasterPasswordFormState& state) {
    if (state.field == TuiMasterPasswordFormField::kNewPassword) {
        state.field = TuiMasterPasswordFormField::kConfirmPassword;
    }
}

std::string MaskSecret(std::string_view value) {
    return std::string(value.size(), '*');
}

void EraseLastCharacter(std::string& value) {
    if (value.empty()) {
        return;
    }

    value.back() = '\0';
    value.pop_back();
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

        if (ch == '\t') {
            return {TuiInputStatus::kKey, TuiKey::kNextField};
        }

        if (ch == '\b' || ch == 127) {
            return {TuiInputStatus::kKey, TuiKey::kBackspace};
        }

        if (ch == 27) {
            return {TuiInputStatus::kKey, DecodeEscapeSequence()};
        }

        if (ch >= 32 && ch <= 126) {
            return {
                TuiInputStatus::kKey,
                TuiKey::kCharacter,
                static_cast<char>(ch)
            };
        }

        return {TuiInputStatus::kKey, TuiKey::kUnknown, '\0'};
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
    std::cout << "  a         create a new entry\n";
    std::cout << "  e         update the selected entry\n";
    std::cout << "  d         delete the selected entry\n";
    std::cout << "  m         change the master password\n";
    std::cout << "  Esc       return to the browse list\n";
    std::cout << "  Tab       move to the next form field\n";
    std::cout << "  Backspace delete the last typed character\n";
    std::cout << "  ?         show this help screen\n";
    std::cout << "  l         lock the vault\n";
    std::cout << "  u         unlock the vault\n";
    std::cout << "  q         quit the TUI\n";
}

void RenderListView(const ShellBrowseSnapshot& snapshot) {
    if (snapshot.entry_names.empty()) {
        std::cout << "No entries selected. Press `a` to add one.\n";
        return;
    }

    if (snapshot.selected_name.empty()) {
        std::cout << "Use Up/Down or j/k to choose an entry.\n";
        return;
    }

    std::cout << "Current selection: " << snapshot.selected_name << '\n';
    std::cout << "Press Enter to open it, `a` to add, `e` to update, or `d` to delete.\n";
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

void RenderEntryFormField(
    std::string_view label,
    std::string_view value,
    bool focused,
    bool masked) {
    std::cout << (focused ? "> " : "  ") << label << ": ";
    if (masked) {
        std::cout << MaskSecret(value);
    } else {
        std::cout << value;
    }
    std::cout << '\n';
}

void RenderEntryFormView(const TuiEntryFormState& state) {
    if (state.mode == EntryMutationMode::kCreate) {
        std::cout << "Create a new entry.\n";
        std::cout << "Type values directly, use Tab to move fields, Enter to save, Esc to cancel.\n\n";
    } else {
        std::cout << "Update entry: " << state.name << '\n';
        std::cout << "Edit the current values, use Tab to move fields, Enter to save, Esc to cancel.\n";
        std::cout << "The entry name is fixed during updates.\n\n";
    }

    RenderEntryFormField(
        "Name",
        state.name,
        state.field == TuiEntryFormField::kName,
        false);
    RenderEntryFormField(
        "Password",
        state.password,
        state.field == TuiEntryFormField::kPassword,
        true);
    RenderEntryFormField(
        "Note",
        state.note,
        state.field == TuiEntryFormField::kNote,
        false);
}

void RenderMasterPasswordFormView(
    const TuiMasterPasswordFormState& state) {
    std::cout << "Change the master password.\n";
    std::cout << "Enter the new password twice, use Tab to move fields, Enter to apply, Esc to cancel.\n\n";
    RenderEntryFormField(
        "New master password",
        state.new_master_password,
        state.field == TuiMasterPasswordFormField::kNewPassword,
        true);
    RenderEntryFormField(
        "Confirm new master password",
        state.confirm_master_password,
        state.field == TuiMasterPasswordFormField::kConfirmPassword,
        true);
}

void RenderExactConfirmationView(
    const TuiExactConfirmationState& state) {
    if (state.kind == FrontendCommandKind::kUpdate) {
        std::cout << "Update entry: " << state.entry_name << '\n';
        std::cout << "Type the entry name exactly, then press Enter to continue editing.\n";
    } else if (state.kind == FrontendCommandKind::kChangeMasterPassword) {
        std::cout << "Change the master password\n";
        std::cout << "Type CHANGE exactly, then press Enter to continue.\n";
    } else {
        std::cout << "Delete entry: " << state.entry_name << '\n';
        std::cout << "Type the entry name exactly, then press Enter to confirm.\n";
    }

    std::cout << "Press Esc to cancel.\n\n";
    std::cout << "> Confirmation: " << state.typed_value << '\n';
}

void RenderViewSection(
    const ShellRuntimeState& runtime,
    const ShellBrowseSnapshot& snapshot,
    const TuiRenderState& render_state) {
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
            RenderEntryFormView(render_state.entry_form);
            return;
        case FrontendSessionState::kEditingMasterPasswordForm:
            RenderMasterPasswordFormView(render_state.master_password_form);
            return;
        case FrontendSessionState::kConfirmingEntryOverwrite:
            RenderExactConfirmationView(render_state.exact_confirmation);
            return;
        case FrontendSessionState::kConfirmingEntryDeletion:
            RenderExactConfirmationView(render_state.exact_confirmation);
            return;
        case FrontendSessionState::kConfirmingMasterPasswordRotation:
            RenderExactConfirmationView(render_state.exact_confirmation);
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

    RenderViewSection(runtime, snapshot, render_state);

    std::cout << "\nKeys: Up/Down or j/k move, Enter shows, a adds, e updates, d deletes, m changes master password, Esc cancels/browses, ? help, l lock, u unlock, q quit.\n";
    std::cout.flush();
}

std::optional<FrontendCommand> ResolveTuiCommand(
    const ShellRuntimeState& runtime,
    const TuiInputEvent& input_event) {
    switch (input_event.key) {
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
        case TuiKey::kBrowse:
            if (!runtime.session.has_value()) {
                return std::nullopt;
            }
            return FrontendCommand{FrontendCommandKind::kList, ""};
        case TuiKey::kCharacter: {
            const char ch = input_event.text;
            if (ch == '?') {
                return FrontendCommand{FrontendCommandKind::kHelp, ""};
            }

            const unsigned char value = static_cast<unsigned char>(ch);
            const char lower =
                value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : static_cast<char>(value);
            switch (lower) {
                case 'j':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{FrontendCommandKind::kNext, ""};
                case 'k':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{FrontendCommandKind::kPrev, ""};
                case 'a':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{FrontendCommandKind::kAdd, ""};
                case 'e':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{FrontendCommandKind::kUpdate, ""};
                case 'd':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{FrontendCommandKind::kDelete, ""};
                case 'm':
                    if (!runtime.session.has_value()) {
                        return std::nullopt;
                    }
                    return FrontendCommand{
                        FrontendCommandKind::kChangeMasterPassword,
                        ""
                    };
                case 'l':
                    return FrontendCommand{FrontendCommandKind::kLock, ""};
                case 'u':
                    return FrontendCommand{FrontendCommandKind::kUnlock, ""};
                case 'q':
                    return FrontendCommand{FrontendCommandKind::kQuit, ""};
                default:
                    return std::nullopt;
            }
        }
        default:
            return std::nullopt;
    }
}

void ReplaceStatusWithError(
    TuiRenderState& render_state,
    std::string_view message) {
    FrontendError error = ClassifyFrontendError(message);
    std::string output = RenderFrontendError(error);
    auto error_guard = MakeScopedCleanse(error);
    auto output_guard = MakeScopedCleanse(output);
    ReplaceStatusMessage(render_state, std::move(output));
}

void RestoreBrowseView(
    ShellRuntimeState& runtime) {
    FrontendActionResult result = ShowCurrentShellBrowseView(runtime);
    auto result_guard = MakeScopedCleanse(result);
}

void BeginAddEntryFlow(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    ClearPendingCommand(render_state);
    ClearMasterPasswordForm(render_state);
    ClearExactConfirmation(render_state);
    BeginEntryForm(render_state, EntryMutationMode::kCreate, "");
    ReplaceStatusMessage(render_state, "creating entry");
    static_cast<void>(runtime.state_machine.HandleCommand(FrontendCommandKind::kAdd));
}

void BeginUpdateEntryFlow(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    const std::optional<std::string> selected_name =
        SelectedBrowseEntryName(runtime);
    if (!selected_name.has_value()) {
        ReplaceStatusWithError(render_state, "no entry selected");
        return;
    }

    ClearPendingCommand(render_state);
    ClearEntryForm(render_state);
    ClearMasterPasswordForm(render_state);
    BeginExactConfirmation(
        render_state,
        FrontendCommandKind::kUpdate,
        *selected_name,
        BuildOverwriteConfirmationRule(*selected_name));
    ReplaceStatusMessage(
        render_state,
        "type the selected entry name to confirm update");
    static_cast<void>(runtime.state_machine.HandleCommand(FrontendCommandKind::kUpdate));
}

void BeginDeleteEntryFlow(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    const std::optional<std::string> selected_name =
        SelectedBrowseEntryName(runtime);
    if (!selected_name.has_value()) {
        ReplaceStatusWithError(render_state, "no entry selected");
        return;
    }

    ClearPendingCommand(render_state);
    ClearEntryForm(render_state);
    ClearMasterPasswordForm(render_state);
    BeginExactConfirmation(
        render_state,
        FrontendCommandKind::kDelete,
        *selected_name,
        BuildDeletionConfirmationRule(*selected_name));
    ReplaceStatusMessage(
        render_state,
        "type the selected entry name to confirm deletion");
    static_cast<void>(runtime.state_machine.HandleCommand(FrontendCommandKind::kDelete));
}

void BeginMasterPasswordRotationFlow(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    ClearPendingCommand(render_state);
    ClearEntryForm(render_state);
    ClearMasterPasswordForm(render_state);
    BeginExactConfirmation(
        render_state,
        FrontendCommandKind::kChangeMasterPassword,
        "",
        BuildMasterPasswordRotationConfirmationRule());
    ReplaceStatusMessage(
        render_state,
        "type CHANGE to confirm master password rotation");
    static_cast<void>(
        runtime.state_machine.HandleCommand(
            FrontendCommandKind::kChangeMasterPassword));
}

void CancelEntryForm(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    const std::string status_message =
        render_state.entry_form.mode == EntryMutationMode::kCreate
            ? "entry creation cancelled"
            : "entry update cancelled";
    ClearEntryForm(render_state);
    RestoreBrowseView(runtime);
    ReplaceStatusMessage(render_state, status_message);
}

void CancelMasterPasswordForm(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    ClearMasterPasswordForm(render_state);
    RestoreBrowseView(runtime);
    ReplaceStatusMessage(render_state, "master password rotation cancelled");
}

void CancelExactConfirmation(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state,
    std::string status_message) {
    ClearExactConfirmation(render_state);
    RestoreBrowseView(runtime);
    ReplaceStatusMessage(render_state, std::move(status_message));
}

void SubmitEntryForm(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    try {
        FrontendActionResult result = StoreShellEntryWithContent(
            runtime,
            render_state.entry_form.mode,
            render_state.entry_form.name,
            render_state.entry_form.password,
            render_state.entry_form.note);
        auto result_guard = MakeScopedCleanse(result);
        const std::string status_message = RenderTuiStatusMessage(result);
        ClearEntryForm(render_state);
        RestoreBrowseView(runtime);
        if (status_message.empty()) {
            ClearStatusMessage(render_state);
        } else {
            ReplaceStatusMessage(render_state, status_message);
        }
    } catch (const std::exception& ex) {
        ReplaceStatusWithError(render_state, ex.what());
    }
}

void SubmitMasterPasswordForm(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    if (render_state.master_password_form.new_master_password !=
        render_state.master_password_form.confirm_master_password) {
        ::Cleanse(render_state.master_password_form.confirm_master_password);
        render_state.master_password_form.confirm_master_password.clear();
        render_state.master_password_form.field =
            TuiMasterPasswordFormField::kConfirmPassword;
        ReplaceStatusWithError(
            render_state,
            "new master passwords do not match");
        return;
    }

    try {
        FrontendActionResult result = RotateShellMasterPassword(
            runtime,
            render_state.master_password_form.new_master_password);
        auto result_guard = MakeScopedCleanse(result);
        const std::string status_message = RenderTuiStatusMessage(result);
        ClearMasterPasswordForm(render_state);
        RestoreBrowseView(runtime);
        if (status_message.empty()) {
            ClearStatusMessage(render_state);
        } else {
            ReplaceStatusMessage(render_state, status_message);
        }
    } catch (const std::exception& ex) {
        ReplaceStatusWithError(render_state, ex.what());
    }
}

bool HandleEntryFormInput(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state,
    const TuiInputEvent& input_event) {
    if (!render_state.entry_form.active) {
        return false;
    }

    if (input_event.key == TuiKey::kBrowse) {
        CancelEntryForm(runtime, render_state);
        return true;
    }

    if (input_event.key == TuiKey::kBackspace) {
        EraseLastCharacter(ActiveEntryFormFieldValue(render_state.entry_form));
        return true;
    }

    if (input_event.text != '\0') {
        ActiveEntryFormFieldValue(render_state.entry_form).push_back(input_event.text);
        return true;
    }

    if (input_event.key == TuiKey::kNextField) {
        AdvanceEntryFormField(render_state.entry_form);
        return true;
    }

    if (input_event.key != TuiKey::kShowSelection) {
        return true;
    }

    if (render_state.entry_form.field != TuiEntryFormField::kNote) {
        AdvanceEntryFormField(render_state.entry_form);
        return true;
    }

    SubmitEntryForm(runtime, render_state);
    return true;
}

bool HandleMasterPasswordFormInput(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state,
    const TuiInputEvent& input_event) {
    if (!render_state.master_password_form.active) {
        return false;
    }

    if (input_event.key == TuiKey::kBrowse) {
        CancelMasterPasswordForm(runtime, render_state);
        return true;
    }

    if (input_event.key == TuiKey::kBackspace) {
        EraseLastCharacter(
            ActiveMasterPasswordFormFieldValue(
                render_state.master_password_form));
        return true;
    }

    if (input_event.text != '\0') {
        ActiveMasterPasswordFormFieldValue(
            render_state.master_password_form).push_back(input_event.text);
        return true;
    }

    if (input_event.key == TuiKey::kNextField) {
        AdvanceMasterPasswordFormField(render_state.master_password_form);
        return true;
    }

    if (input_event.key != TuiKey::kShowSelection) {
        return true;
    }

    if (render_state.master_password_form.field !=
        TuiMasterPasswordFormField::kConfirmPassword) {
        AdvanceMasterPasswordFormField(render_state.master_password_form);
        return true;
    }

    SubmitMasterPasswordForm(runtime, render_state);
    return true;
}

void SubmitExactConfirmation(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state) {
    if (render_state.exact_confirmation.typed_value !=
        render_state.exact_confirmation.rule.expected_value) {
        CancelExactConfirmation(
            runtime,
            render_state,
            RenderFrontendError(FrontendError{
                FrontendErrorKind::kConfirmationRejected,
                render_state.exact_confirmation.rule.mismatch_error
            }));
        return;
    }

    try {
        if (render_state.exact_confirmation.kind ==
            FrontendCommandKind::kChangeMasterPassword) {
            static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
            ClearExactConfirmation(render_state);
            BeginMasterPasswordForm(render_state);
            ReplaceStatusMessage(
                render_state,
                "enter the new master password");
            return;
        }

        if (render_state.exact_confirmation.kind == FrontendCommandKind::kUpdate) {
            if (!runtime.session.has_value()) {
                throw std::runtime_error("vault is locked");
            }

            PasswordEntry entry =
                runtime.session->LoadEntry(render_state.exact_confirmation.entry_name);
            auto entry_guard = MakeScopedCleanse(entry);
            static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
            ClearExactConfirmation(render_state);
            PopulateEntryFormForUpdate(render_state, entry);
            ReplaceStatusMessage(
                render_state,
                "editing entry " + render_state.entry_form.name);
            return;
        }

        static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
        FrontendActionResult result = RemoveShellEntryByName(
            runtime,
            render_state.exact_confirmation.entry_name);
        auto result_guard = MakeScopedCleanse(result);
        const std::string status_message = RenderTuiStatusMessage(result);
        ClearExactConfirmation(render_state);
        RestoreBrowseView(runtime);
        if (status_message.empty()) {
            ClearStatusMessage(render_state);
        } else {
            ReplaceStatusMessage(render_state, status_message);
        }
    } catch (const std::exception& ex) {
        ClearExactConfirmation(render_state);
        RestoreBrowseView(runtime);
        ReplaceStatusWithError(render_state, ex.what());
    }
}

bool HandleExactConfirmationInput(
    ShellRuntimeState& runtime,
    TuiRenderState& render_state,
    const TuiInputEvent& input_event) {
    if (!render_state.exact_confirmation.active) {
        return false;
    }

    if (input_event.key == TuiKey::kBrowse) {
        CancelExactConfirmation(
            runtime,
            render_state,
            render_state.exact_confirmation.kind ==
                    FrontendCommandKind::kUpdate
                ? "entry update cancelled"
                : (render_state.exact_confirmation.kind ==
                           FrontendCommandKind::kChangeMasterPassword
                       ? "master password rotation cancelled"
                       : "entry deletion cancelled"));
        return true;
    }

    if (input_event.key == TuiKey::kBackspace) {
        EraseLastCharacter(render_state.exact_confirmation.typed_value);
        return true;
    }

    if (input_event.text != '\0') {
        render_state.exact_confirmation.typed_value.push_back(input_event.text);
        return true;
    }

    if (input_event.key == TuiKey::kShowSelection) {
        SubmitExactConfirmation(runtime, render_state);
        return true;
    }

    return true;
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
            ClearPendingCommand(render_state);
            ClearEntryForm(render_state);
            ClearMasterPasswordForm(render_state);
            ClearExactConfirmation(render_state);
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

        if (HandleEntryFormInput(runtime, render_state, input_event)) {
            continue;
        }

        if (HandleMasterPasswordFormInput(runtime, render_state, input_event)) {
            continue;
        }

        if (HandleExactConfirmationInput(runtime, render_state, input_event)) {
            continue;
        }

        const std::optional<FrontendCommand> command =
            ResolveTuiCommand(runtime, input_event);
        if (!command.has_value()) {
            continue;
        }

        try {
            if (command->kind == FrontendCommandKind::kAdd) {
                BeginAddEntryFlow(runtime, render_state);
                continue;
            }

            if (command->kind == FrontendCommandKind::kUpdate) {
                BeginUpdateEntryFlow(runtime, render_state);
                continue;
            }

            if (command->kind == FrontendCommandKind::kDelete) {
                BeginDeleteEntryFlow(runtime, render_state);
                continue;
            }

            if (command->kind == FrontendCommandKind::kChangeMasterPassword) {
                BeginMasterPasswordRotationFlow(runtime, render_state);
                continue;
            }

            FrontendActionResult result{};
            if (command->kind == FrontendCommandKind::kList) {
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
            ClearEntryForm(render_state);
            ClearMasterPasswordForm(render_state);
            ClearExactConfirmation(render_state);
            static_cast<void>(RecoverShellViewAfterFailure(runtime));
            ReplaceStatusMessage(render_state, std::move(output));
        }
    }
}
