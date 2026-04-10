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
    kNext,
    kPrev,
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

enum class FrontendStateEvent {
    kVaultMissingAtStartup,
    kVaultExistsAtStartup,
    kHelpRequested,
    kListRequested,
    kFindRequested,
    kNextRequested,
    kPrevRequested,
    kShowRequested,
    kAddRequested,
    kUpdateRequested,
    kDeleteRequested,
    kMasterPasswordRotationRequested,
    kLockRequested,
    kIdleTimeoutElapsed,
    kUnlockRequested,
    kQuitRequested,
    kConfirmationAccepted,
    kOperationFailed,
    kRecoveryCompletedWhileUnlocked,
    kRecoveryCompletedWhileLocked
};

struct FrontendStateTransition {
    FrontendSessionState from_state;
    FrontendStateEvent event;
    FrontendSessionState to_state;
};

struct FrontendActionResult;

class FrontendStateMachine {
public:
    explicit FrontendStateMachine(
        FrontendSessionState initial_state =
            FrontendSessionState::kInitializingVault) noexcept;

    FrontendSessionState state() const noexcept;

    void SetState(FrontendSessionState state) noexcept;

    FrontendSessionState ApplyEvent(FrontendStateEvent event);

    FrontendSessionState ApplyActionResult(
        const FrontendActionResult& result) noexcept;

    FrontendSessionState HandleStartup(bool vault_exists);

    FrontendSessionState HandleCommand(FrontendCommandKind kind);

    FrontendSessionState HandleIdleTimeout();

    FrontendSessionState HandleConfirmationAccepted();

    FrontendSessionState HandleFailure(bool session_unlocked);

private:
    FrontendSessionState state_;
};

enum class FrontendPayloadKind {
    kNone,
    kText,
    kEntry,
    kEntryNames,
    kFocusedList
};

enum class FrontendErrorKind {
    kUsage,
    kUnknownCommand,
    kConflict,
    kNotFound,
    kLocked,
    kSelection,
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

struct FrontendFocusedList {
    std::string filter_term;
    std::string selected_name;
    std::vector<std::string> entry_names;
};

struct FrontendActionResult {
    FrontendSessionState state;
    FrontendPayloadKind payload_kind;
    std::string message;
    std::string empty_message;
    PasswordEntry entry;
    std::vector<std::string> entry_names;
    FrontendFocusedList focused_list;
};

inline void Cleanse(std::vector<std::string>& values) {
    for (std::string& value : values) {
        Cleanse(value);
    }
}

inline void Cleanse(FrontendFocusedList& focused_list) {
    Cleanse(focused_list.filter_term);
    Cleanse(focused_list.selected_name);
    Cleanse(focused_list.entry_names);
}

inline void Cleanse(FrontendError& error) {
    Cleanse(error.message);
}

inline void Cleanse(FrontendActionResult& result) {
    Cleanse(result.message);
    Cleanse(result.empty_message);
    Cleanse(result.entry);
    Cleanse(result.entry_names);
    Cleanse(result.focused_list);
}

const std::vector<std::string>& CliUsageCommands();

const std::vector<std::string>& ShellHelpCommands();

FrontendCommand ParseShellCommand(const std::string& line);

bool IsBlankShellInput(std::string_view line);

const std::vector<FrontendStateTransition>& FrontendStateTransitions();

FrontendStateEvent ResolveStartupEvent(bool vault_exists);

FrontendStateEvent ResolveCommandEvent(FrontendCommandKind kind);

FrontendSessionState ResolveStateTransition(
    FrontendSessionState from_state,
    FrontendStateEvent event);

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

FrontendActionResult BuildIdleLockedResult();

FrontendActionResult BuildUnlockedResult();

FrontendActionResult BuildListResult(
    std::vector<std::string> entry_names,
    const std::string& empty_message);

FrontendActionResult BuildFocusedListResult(
    std::vector<std::string> entry_names,
    const std::string& selected_name,
    const std::string& filter_term,
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
