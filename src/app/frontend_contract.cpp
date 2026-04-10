#include "app/frontend_contract.hpp"

#include <utility>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string> SplitWords(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> parts;
    std::string part;
    while (input >> part) {
        parts.push_back(part);
    }
    return parts;
}

void RequireArgumentCount(
    const std::vector<std::string>& parts,
    std::size_t expected,
    const std::string& usage) {
    if (parts.size() != expected) {
        throw std::runtime_error("usage: " + usage);
    }
}

void RequireArgumentRange(
    const std::vector<std::string>& parts,
    std::size_t minimum,
    std::size_t maximum,
    const std::string& usage) {
    if (parts.size() < minimum || parts.size() > maximum) {
        throw std::runtime_error("usage: " + usage);
    }
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::ostringstream output;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            output << '\n';
        }
        output << lines[i];
    }

    return output.str();
}

std::string FormatCommandBlock(
    std::string_view title,
    const std::vector<std::string>& commands) {
    std::ostringstream output;
    output << title;
    for (const std::string& command : commands) {
        output << "\n  " << command;
    }

    return output.str();
}

void AppendTransitions(
    std::vector<FrontendStateTransition>& transitions,
    const std::vector<FrontendSessionState>& from_states,
    FrontendStateEvent event,
    FrontendSessionState to_state) {
    for (FrontendSessionState from_state : from_states) {
        transitions.push_back(FrontendStateTransition{
            from_state,
            event,
            to_state
        });
    }
}

std::string RenderFocusedList(
    const FrontendFocusedList& focused_list,
    std::string_view empty_message) {
    if (focused_list.entry_names.empty()) {
        return std::string(empty_message);
    }

    std::ostringstream output;
    for (std::size_t i = 0; i < focused_list.entry_names.size(); ++i) {
        if (i > 0) {
            output << '\n';
        }

        const std::string& entry_name = focused_list.entry_names[i];
        const bool is_selected = entry_name == focused_list.selected_name;
        output << (is_selected ? "> " : "  ") << entry_name;
    }

    return output.str();
}

}  // namespace

const std::vector<std::string>& CliUsageCommands() {
    static const std::vector<std::string> kCommands = {
        "zkvault init",
        "zkvault shell",
        "zkvault change-master-password",
        "zkvault add <name>",
        "zkvault get <name>",
        "zkvault update <name>",
        "zkvault delete <name>",
        "zkvault list"
    };

    return kCommands;
}

const std::vector<std::string>& ShellHelpCommands() {
    static const std::vector<std::string> kCommands = {
        "help",
        "list",
        "find <term>",
        "next",
        "prev",
        "show [name]",
        "add <name>",
        "update <name>",
        "delete <name>",
        "change-master-password",
        "lock",
        "unlock",
        "quit"
    };

    return kCommands;
}

FrontendCommand ParseShellCommand(const std::string& line) {
    const std::vector<std::string> parts = SplitWords(line);
    if (parts.empty()) {
        throw std::runtime_error("empty shell command");
    }

    const std::string& command = parts[0];

    if (command == "help") {
        RequireArgumentCount(parts, 1, "help");
        return FrontendCommand{FrontendCommandKind::kHelp, ""};
    }

    if (command == "list") {
        RequireArgumentCount(parts, 1, "list");
        return FrontendCommand{FrontendCommandKind::kList, ""};
    }

    if (command == "find") {
        RequireArgumentCount(parts, 2, "find <term>");
        return FrontendCommand{FrontendCommandKind::kFind, parts[1]};
    }

    if (command == "next") {
        RequireArgumentCount(parts, 1, "next");
        return FrontendCommand{FrontendCommandKind::kNext, ""};
    }

    if (command == "prev") {
        RequireArgumentCount(parts, 1, "prev");
        return FrontendCommand{FrontendCommandKind::kPrev, ""};
    }

    if (command == "show") {
        RequireArgumentRange(parts, 1, 2, "show [name]");
        const std::string name = parts.size() == 2 ? parts[1] : "";
        return FrontendCommand{FrontendCommandKind::kShow, name};
    }

    if (command == "add") {
        RequireArgumentCount(parts, 2, "add <name>");
        return FrontendCommand{FrontendCommandKind::kAdd, parts[1]};
    }

    if (command == "update") {
        RequireArgumentCount(parts, 2, "update <name>");
        return FrontendCommand{FrontendCommandKind::kUpdate, parts[1]};
    }

    if (command == "delete") {
        RequireArgumentCount(parts, 2, "delete <name>");
        return FrontendCommand{FrontendCommandKind::kDelete, parts[1]};
    }

    if (command == "change-master-password") {
        RequireArgumentCount(parts, 1, "change-master-password");
        return FrontendCommand{FrontendCommandKind::kChangeMasterPassword, ""};
    }

    if (command == "lock") {
        RequireArgumentCount(parts, 1, "lock");
        return FrontendCommand{FrontendCommandKind::kLock, ""};
    }

    if (command == "unlock") {
        RequireArgumentCount(parts, 1, "unlock");
        return FrontendCommand{FrontendCommandKind::kUnlock, ""};
    }

    if (command == "quit" || command == "exit") {
        RequireArgumentCount(parts, 1, "quit");
        return FrontendCommand{FrontendCommandKind::kQuit, ""};
    }

    throw std::runtime_error("unknown shell command");
}

bool IsBlankShellInput(std::string_view line) {
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

const std::vector<FrontendStateTransition>& FrontendStateTransitions() {
    static const std::vector<FrontendStateTransition> kTransitions = [] {
        std::vector<FrontendStateTransition> transitions;

        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kInitializingVault,
            FrontendStateEvent::kVaultMissingAtStartup,
            FrontendSessionState::kInitializingVault
        });
        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kInitializingVault,
            FrontendStateEvent::kVaultExistsAtStartup,
            FrontendSessionState::kReady
        });

        const std::vector<FrontendSessionState> interactive_states = {
            FrontendSessionState::kReady,
            FrontendSessionState::kLocked,
            FrontendSessionState::kShowingHelp,
            FrontendSessionState::kShowingList,
            FrontendSessionState::kShowingEntry
        };

        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kHelpRequested,
            FrontendSessionState::kShowingHelp);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kListRequested,
            FrontendSessionState::kShowingList);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kFindRequested,
            FrontendSessionState::kShowingList);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kNextRequested,
            FrontendSessionState::kShowingList);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kPrevRequested,
            FrontendSessionState::kShowingList);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kShowRequested,
            FrontendSessionState::kShowingEntry);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kAddRequested,
            FrontendSessionState::kEditingEntryForm);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kUpdateRequested,
            FrontendSessionState::kConfirmingEntryOverwrite);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kDeleteRequested,
            FrontendSessionState::kConfirmingEntryDeletion);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kMasterPasswordRotationRequested,
            FrontendSessionState::kConfirmingMasterPasswordRotation);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kLockRequested,
            FrontendSessionState::kLocked);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kIdleTimeoutElapsed,
            FrontendSessionState::kLocked);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kUnlockRequested,
            FrontendSessionState::kUnlockingSession);
        AppendTransitions(
            transitions,
            interactive_states,
            FrontendStateEvent::kQuitRequested,
            FrontendSessionState::kQuitRequested);

        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kConfirmingEntryOverwrite,
            FrontendStateEvent::kConfirmationAccepted,
            FrontendSessionState::kEditingEntryForm
        });
        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kConfirmingEntryDeletion,
            FrontendStateEvent::kConfirmationAccepted,
            FrontendSessionState::kReady
        });
        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kConfirmingMasterPasswordRotation,
            FrontendStateEvent::kConfirmationAccepted,
            FrontendSessionState::kEditingMasterPasswordForm
        });

        const std::vector<FrontendSessionState> failure_states = {
            FrontendSessionState::kReady,
            FrontendSessionState::kLocked,
            FrontendSessionState::kUnlockingSession,
            FrontendSessionState::kEditingEntryForm,
            FrontendSessionState::kEditingMasterPasswordForm,
            FrontendSessionState::kConfirmingEntryOverwrite,
            FrontendSessionState::kConfirmingEntryDeletion,
            FrontendSessionState::kConfirmingMasterPasswordRotation,
            FrontendSessionState::kShowingHelp,
            FrontendSessionState::kShowingList,
            FrontendSessionState::kShowingEntry
        };

        AppendTransitions(
            transitions,
            failure_states,
            FrontendStateEvent::kOperationFailed,
            FrontendSessionState::kRecoveringFromFailure);
        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kRecoveringFromFailure,
            FrontendStateEvent::kRecoveryCompletedWhileUnlocked,
            FrontendSessionState::kReady
        });
        transitions.push_back(FrontendStateTransition{
            FrontendSessionState::kRecoveringFromFailure,
            FrontendStateEvent::kRecoveryCompletedWhileLocked,
            FrontendSessionState::kLocked
        });

        return transitions;
    }();

    return kTransitions;
}

FrontendStateEvent ResolveStartupEvent(bool vault_exists) {
    return vault_exists ? FrontendStateEvent::kVaultExistsAtStartup
                        : FrontendStateEvent::kVaultMissingAtStartup;
}

FrontendStateEvent ResolveCommandEvent(FrontendCommandKind kind) {
    if (kind == FrontendCommandKind::kHelp) {
        return FrontendStateEvent::kHelpRequested;
    }

    if (kind == FrontendCommandKind::kList) {
        return FrontendStateEvent::kListRequested;
    }

    if (kind == FrontendCommandKind::kFind) {
        return FrontendStateEvent::kFindRequested;
    }

    if (kind == FrontendCommandKind::kNext) {
        return FrontendStateEvent::kNextRequested;
    }

    if (kind == FrontendCommandKind::kPrev) {
        return FrontendStateEvent::kPrevRequested;
    }

    if (kind == FrontendCommandKind::kShow) {
        return FrontendStateEvent::kShowRequested;
    }

    if (kind == FrontendCommandKind::kAdd) {
        return FrontendStateEvent::kAddRequested;
    }

    if (kind == FrontendCommandKind::kUpdate) {
        return FrontendStateEvent::kUpdateRequested;
    }

    if (kind == FrontendCommandKind::kDelete) {
        return FrontendStateEvent::kDeleteRequested;
    }

    if (kind == FrontendCommandKind::kChangeMasterPassword) {
        return FrontendStateEvent::kMasterPasswordRotationRequested;
    }

    if (kind == FrontendCommandKind::kLock) {
        return FrontendStateEvent::kLockRequested;
    }

    if (kind == FrontendCommandKind::kUnlock) {
        return FrontendStateEvent::kUnlockRequested;
    }

    if (kind == FrontendCommandKind::kQuit) {
        return FrontendStateEvent::kQuitRequested;
    }

    throw std::runtime_error("unsupported frontend command kind");
}

FrontendSessionState ResolveStateTransition(
    FrontendSessionState from_state,
    FrontendStateEvent event) {
    for (const FrontendStateTransition& transition : FrontendStateTransitions()) {
        if (transition.from_state == from_state && transition.event == event) {
            return transition.to_state;
        }
    }

    throw std::runtime_error("unsupported frontend state transition");
}

FrontendStateMachine::FrontendStateMachine(
    FrontendSessionState initial_state) noexcept
    : state_(initial_state) {}

FrontendSessionState FrontendStateMachine::state() const noexcept {
    return state_;
}

void FrontendStateMachine::SetState(FrontendSessionState state) noexcept {
    state_ = state;
}

FrontendSessionState FrontendStateMachine::ApplyEvent(FrontendStateEvent event) {
    state_ = ResolveStateTransition(state_, event);
    return state_;
}

FrontendSessionState FrontendStateMachine::ApplyActionResult(
    const FrontendActionResult& result) noexcept {
    state_ = result.state;
    return state_;
}

FrontendSessionState FrontendStateMachine::HandleStartup(bool vault_exists) {
    return ApplyEvent(ResolveStartupEvent(vault_exists));
}

FrontendSessionState FrontendStateMachine::HandleCommand(FrontendCommandKind kind) {
    return ApplyEvent(ResolveCommandEvent(kind));
}

FrontendSessionState FrontendStateMachine::HandleIdleTimeout() {
    return ApplyEvent(FrontendStateEvent::kIdleTimeoutElapsed);
}

FrontendSessionState FrontendStateMachine::HandleConfirmationAccepted() {
    return ApplyEvent(FrontendStateEvent::kConfirmationAccepted);
}

FrontendSessionState FrontendStateMachine::HandleFailure(bool session_unlocked) {
    static_cast<void>(ApplyEvent(FrontendStateEvent::kOperationFailed));
    return ApplyEvent(
        session_unlocked ? FrontendStateEvent::kRecoveryCompletedWhileUnlocked
                         : FrontendStateEvent::kRecoveryCompletedWhileLocked);
}

FrontendSessionState ResolveStartupState(bool vault_exists) {
    return ResolveStateTransition(
        FrontendSessionState::kInitializingVault,
        ResolveStartupEvent(vault_exists));
}

FrontendSessionState ResolveCommandInputState(FrontendCommandKind kind) {
    return ResolveStateTransition(
        FrontendSessionState::kReady,
        ResolveCommandEvent(kind));
}

FrontendSessionState ResolvePostConfirmationState(FrontendCommandKind kind) {
    if (kind == FrontendCommandKind::kUpdate ||
        kind == FrontendCommandKind::kDelete ||
        kind == FrontendCommandKind::kChangeMasterPassword) {
        return ResolveStateTransition(
            ResolveCommandInputState(kind),
            FrontendStateEvent::kConfirmationAccepted);
    }

    return ResolveCommandInputState(kind);
}

ExactConfirmationRule BuildOverwriteConfirmationRule(const std::string& name) {
    return ExactConfirmationRule{
        "Type the entry name to confirm overwrite: ",
        name,
        "entry overwrite cancelled"
    };
}

ExactConfirmationRule BuildDeletionConfirmationRule(const std::string& name) {
    return ExactConfirmationRule{
        "Type the entry name to confirm deletion: ",
        name,
        "entry deletion cancelled"
    };
}

ExactConfirmationRule BuildMasterPasswordRotationConfirmationRule() {
    return ExactConfirmationRule{
        "Type CHANGE to confirm master password rotation: ",
        "CHANGE",
        "master password rotation cancelled"
    };
}

FrontendActionResult BuildCliUsageResult() {
    return FrontendActionResult{
        FrontendSessionState::kShowingHelp,
        FrontendPayloadKind::kText,
        FormatCommandBlock("Usage:", CliUsageCommands()),
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildShellReadyResult() {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        "shell ready; type help for commands",
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildShellHelpResult() {
    return FrontendActionResult{
        FrontendSessionState::kShowingHelp,
        FrontendPayloadKind::kText,
        FormatCommandBlock("Commands:", ShellHelpCommands()),
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildLockedResult() {
    return FrontendActionResult{
        FrontendSessionState::kLocked,
        FrontendPayloadKind::kText,
        "vault locked",
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildIdleLockedResult() {
    return FrontendActionResult{
        FrontendSessionState::kLocked,
        FrontendPayloadKind::kText,
        "vault locked due to inactivity",
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildUnlockedResult() {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        "vault unlocked",
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildListResult(
    std::vector<std::string> entry_names,
    const std::string& empty_message) {
    return FrontendActionResult{
        FrontendSessionState::kShowingList,
        FrontendPayloadKind::kEntryNames,
        "",
        empty_message,
        {},
        std::move(entry_names),
        {}
    };
}

FrontendActionResult BuildFocusedListResult(
    std::vector<std::string> entry_names,
    const std::string& selected_name,
    const std::string& filter_term,
    const std::string& empty_message) {
    return FrontendActionResult{
        FrontendSessionState::kShowingList,
        FrontendPayloadKind::kFocusedList,
        "",
        empty_message,
        {},
        {},
        FrontendFocusedList{
            filter_term,
            selected_name,
            std::move(entry_names)
        }
    };
}

FrontendActionResult BuildShowEntryResult(PasswordEntry entry) {
    return FrontendActionResult{
        FrontendSessionState::kShowingEntry,
        FrontendPayloadKind::kEntry,
        "",
        "",
        std::move(entry),
        {},
        {}
    };
}

FrontendActionResult BuildInitializedResult(const std::string& master_key_path) {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        "initialized " + master_key_path,
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildStoredEntryResult(const std::string& entry_path) {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        FormatStoredEntryMessage(entry_path),
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildUpdatedResult(const std::string& path) {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        FormatUpdatedPathMessage(path),
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildDeletedEntryResult(const std::string& entry_path) {
    return FrontendActionResult{
        FrontendSessionState::kReady,
        FrontendPayloadKind::kText,
        FormatDeletedEntryMessage(entry_path),
        "",
        {},
        {},
        {}
    };
}

FrontendActionResult BuildQuitResult() {
    return FrontendActionResult{
        FrontendSessionState::kQuitRequested,
        FrontendPayloadKind::kNone,
        "",
        "",
        {},
        {},
        {}
    };
}

FrontendError ClassifyFrontendError(std::string_view message) {
    if (StartsWith(message, "usage: ")) {
        return FrontendError{FrontendErrorKind::kUsage, std::string(message)};
    }

    if (message == "unknown shell command") {
        return FrontendError{
            FrontendErrorKind::kUnknownCommand,
            std::string(message)
        };
    }

    if (message == "entry already exists" ||
        message == ".zkv_master already exists") {
        return FrontendError{FrontendErrorKind::kConflict, std::string(message)};
    }

    if (message == "entry does not exist") {
        return FrontendError{FrontendErrorKind::kNotFound, std::string(message)};
    }

    if (message == "vault is locked") {
        return FrontendError{FrontendErrorKind::kLocked, std::string(message)};
    }

    if (message == "no entry selected") {
        return FrontendError{FrontendErrorKind::kSelection, std::string(message)};
    }

    if (message == "entry overwrite cancelled" ||
        message == "entry deletion cancelled" ||
        message == "master password rotation cancelled") {
        return FrontendError{
            FrontendErrorKind::kConfirmationRejected,
            std::string(message)
        };
    }

    if (message == "input cancelled") {
        return FrontendError{
            FrontendErrorKind::kInputCancelled,
            std::string(message)
        };
    }

    if (message == "master passwords do not match" ||
        message == "new master passwords do not match" ||
        message == "invalid ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS" ||
        message == "entry name must not be empty" ||
        message == "entry name must not be '.' or '..'" ||
        message == "entry name may only contain letters, digits, '.', '-' and '_'") {
        return FrontendError{
            FrontendErrorKind::kValidation,
            std::string(message)
        };
    }

    if (message == "vault is already locked" ||
        message == "vault is already unlocked") {
        return FrontendError{FrontendErrorKind::kConflict, std::string(message)};
    }

    if (message == "AES-256-GCM decryption failed") {
        return FrontendError{
            FrontendErrorKind::kAuthentication,
            std::string(message)
        };
    }

    if (StartsWith(message, "invalid .zkv_master") ||
        StartsWith(message, "unsupported .zkv_master") ||
        StartsWith(message, "invalid encrypted entry") ||
        StartsWith(message, "unsupported encrypted entry")) {
        return FrontendError{FrontendErrorKind::kStorage, std::string(message)};
    }

    return FrontendError{FrontendErrorKind::kUnknown, std::string(message)};
}

std::string RenderFrontendActionResult(const FrontendActionResult& result) {
    if (result.payload_kind == FrontendPayloadKind::kNone) {
        return "";
    }

    if (result.payload_kind == FrontendPayloadKind::kText) {
        return result.message;
    }

    if (result.payload_kind == FrontendPayloadKind::kEntry) {
        return json(result.entry).dump(2);
    }

    if (result.payload_kind == FrontendPayloadKind::kEntryNames) {
        if (result.entry_names.empty()) {
            return result.empty_message;
        }

        return JoinLines(result.entry_names);
    }

    if (result.payload_kind == FrontendPayloadKind::kFocusedList) {
        return RenderFocusedList(result.focused_list, result.empty_message);
    }

    throw std::runtime_error("unsupported frontend payload kind");
}

std::string RenderFrontendError(const FrontendError& error) {
    return "error: " + error.message;
}

std::string FormatStoredEntryMessage(const std::string& entry_path) {
    return "saved to " + entry_path;
}

std::string FormatUpdatedPathMessage(const std::string& path) {
    return "updated " + path;
}

std::string FormatDeletedEntryMessage(const std::string& entry_path) {
    return "deleted " + entry_path;
}
