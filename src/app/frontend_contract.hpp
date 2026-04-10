#pragma once

#include <string_view>
#include <string>
#include <vector>

#include "crypto/secure_memory.hpp"
#include "model/password_entry.hpp"

enum class FrontendCommandKind {
    kHelp,
    kList,
    kFind,
    kShow,
    kAdd,
    kUpdate,
    kDelete,
    kChangeMasterPassword,
    kLock,
    kUnlock,
    kQuit
};

struct FrontendCommand {
    FrontendCommandKind kind;
    std::string name;
};

enum class FrontendSessionState {
    kInitializingVault,
    kLocked,
    kReady,
    kUnlockingSession,
    kEditingEntryForm,
    kEditingMasterPasswordForm,
    kConfirmingEntryOverwrite,
    kConfirmingEntryDeletion,
    kConfirmingMasterPasswordRotation,
    kShowingHelp,
    kShowingList,
    kShowingEntry,
    kRecoveringFromFailure,
    kQuitRequested
};

enum class FrontendPayloadKind {
    kNone,
    kText,
    kEntry,
    kEntryNames
};

enum class FrontendErrorKind {
    kUsage,
    kUnknownCommand,
    kConflict,
    kNotFound,
    kLocked,
    kValidation,
    kConfirmationRejected,
    kInputCancelled,
    kAuthentication,
    kStorage,
    kUnknown
};

struct ExactConfirmationRule {
    std::string prompt;
    std::string expected_value;
    std::string mismatch_error;
};

struct FrontendError {
    FrontendErrorKind kind;
    std::string message;
};

struct FrontendActionResult {
    FrontendSessionState state;
    FrontendPayloadKind payload_kind;
    std::string message;
    std::string empty_message;
    PasswordEntry entry;
    std::vector<std::string> entry_names;
};

inline void Cleanse(std::vector<std::string>& values) {
    for (std::string& value : values) {
        Cleanse(value);
    }
}

inline void Cleanse(FrontendError& error) {
    Cleanse(error.message);
}

inline void Cleanse(FrontendActionResult& result) {
    Cleanse(result.message);
    Cleanse(result.empty_message);
    Cleanse(result.entry);
    Cleanse(result.entry_names);
}

const std::vector<std::string>& CliUsageCommands();

const std::vector<std::string>& ShellHelpCommands();

FrontendCommand ParseShellCommand(const std::string& line);

bool IsBlankShellInput(std::string_view line);

FrontendSessionState ResolveStartupState(bool vault_exists);

FrontendSessionState ResolveCommandInputState(FrontendCommandKind kind);

FrontendSessionState ResolvePostConfirmationState(FrontendCommandKind kind);

ExactConfirmationRule BuildOverwriteConfirmationRule(const std::string& name);

ExactConfirmationRule BuildDeletionConfirmationRule(const std::string& name);

ExactConfirmationRule BuildMasterPasswordRotationConfirmationRule();

FrontendActionResult BuildCliUsageResult();

FrontendActionResult BuildShellReadyResult();

FrontendActionResult BuildShellHelpResult();

FrontendActionResult BuildLockedResult();

FrontendActionResult BuildUnlockedResult();

FrontendActionResult BuildListResult(
    std::vector<std::string> entry_names,
    const std::string& empty_message);

FrontendActionResult BuildShowEntryResult(PasswordEntry entry);

FrontendActionResult BuildInitializedResult(const std::string& master_key_path);

FrontendActionResult BuildStoredEntryResult(const std::string& entry_path);

FrontendActionResult BuildUpdatedResult(const std::string& path);

FrontendActionResult BuildDeletedEntryResult(const std::string& entry_path);

FrontendActionResult BuildQuitResult();

FrontendError ClassifyFrontendError(std::string_view message);

std::string RenderFrontendActionResult(const FrontendActionResult& result);

std::string RenderFrontendError(const FrontendError& error);

std::string FormatStoredEntryMessage(const std::string& entry_path);

std::string FormatUpdatedPathMessage(const std::string& path);

std::string FormatDeletedEntryMessage(const std::string& entry_path);
