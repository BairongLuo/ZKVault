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
        "show <name>",
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

    if (command == "show") {
        RequireArgumentCount(parts, 2, "show <name>");
        return FrontendCommand{FrontendCommandKind::kShow, parts[1]};
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

FrontendSessionState ResolveStartupState(bool vault_exists) {
    if (!vault_exists) {
        return FrontendSessionState::kInitializingVault;
    }

    return FrontendSessionState::kReady;
}

FrontendSessionState ResolveCommandInputState(FrontendCommandKind kind) {
    if (kind == FrontendCommandKind::kLock) {
        return FrontendSessionState::kLocked;
    }

    if (kind == FrontendCommandKind::kUnlock) {
        return FrontendSessionState::kUnlockingSession;
    }

    if (kind == FrontendCommandKind::kAdd) {
        return FrontendSessionState::kEditingEntryForm;
    }

    if (kind == FrontendCommandKind::kUpdate) {
        return FrontendSessionState::kConfirmingEntryOverwrite;
    }

    if (kind == FrontendCommandKind::kDelete) {
        return FrontendSessionState::kConfirmingEntryDeletion;
    }

    if (kind == FrontendCommandKind::kChangeMasterPassword) {
        return FrontendSessionState::kConfirmingMasterPasswordRotation;
    }

    if (kind == FrontendCommandKind::kHelp) {
        return FrontendSessionState::kShowingHelp;
    }

    if (kind == FrontendCommandKind::kList) {
        return FrontendSessionState::kShowingList;
    }

    if (kind == FrontendCommandKind::kFind) {
        return FrontendSessionState::kShowingList;
    }

    if (kind == FrontendCommandKind::kShow) {
        return FrontendSessionState::kShowingEntry;
    }

    if (kind == FrontendCommandKind::kQuit) {
        return FrontendSessionState::kQuitRequested;
    }

    return FrontendSessionState::kReady;
}

FrontendSessionState ResolvePostConfirmationState(FrontendCommandKind kind) {
    if (kind == FrontendCommandKind::kUpdate) {
        return FrontendSessionState::kEditingEntryForm;
    }

    if (kind == FrontendCommandKind::kChangeMasterPassword) {
        return FrontendSessionState::kEditingMasterPasswordForm;
    }

    if (kind == FrontendCommandKind::kDelete) {
        return FrontendSessionState::kReady;
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
        std::move(entry_names)
    };
}

FrontendActionResult BuildShowEntryResult(PasswordEntry entry) {
    return FrontendActionResult{
        FrontendSessionState::kShowingEntry,
        FrontendPayloadKind::kEntry,
        "",
        "",
        std::move(entry),
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
