#include "shell/interactive_shell.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/frontend_contract.hpp"
#include "app/vault_app.hpp"
#include "app/vault_session.hpp"
#include "crypto/secure_memory.hpp"
#include "model/password_entry.hpp"
#include "terminal/display.hpp"
#include "terminal/prompt.hpp"

namespace {

constexpr const char* kShellIdleTimeoutEnv =
    "ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS";

void PrintFrontendResult(FrontendActionResult result) {
    auto result_guard = MakeScopedCleanse(result);
    std::string output = RenderFrontendActionResult(result);
    auto output_guard = MakeScopedCleanse(output);
    if (!output.empty()) {
        std::cout << output << '\n';
    }
}

char LowerAscii(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }

    return static_cast<char>(value);
}

bool ContainsCaseInsensitive(std::string_view value, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }

    if (needle.size() > value.size()) {
        return false;
    }

    for (std::size_t offset = 0; offset + needle.size() <= value.size(); ++offset) {
        bool matches = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (LowerAscii(value[offset + i]) != LowerAscii(needle[i])) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    return false;
}

std::vector<std::string> FilterEntryNames(
    const std::vector<std::string>& entry_names,
    std::string_view query) {
    std::vector<std::string> matches;
    for (const std::string& entry_name : entry_names) {
        if (ContainsCaseInsensitive(entry_name, query)) {
            matches.push_back(entry_name);
        }
    }

    return matches;
}

struct ShellBrowseState {
    bool active = false;
    std::string filter_term;
    std::vector<std::string> visible_entry_names;
    std::size_t selected_index = 0;
};

void Cleanse(ShellBrowseState& state) {
    ::Cleanse(state.filter_term);
    ::Cleanse(state.visible_entry_names);
}

void ResetBrowseState(ShellBrowseState& state) {
    Cleanse(state);
    state.active = false;
    state.filter_term.clear();
    state.visible_entry_names.clear();
    state.selected_index = 0;
}

bool HasBrowseSelection(const ShellBrowseState& state) {
    return state.active &&
           state.selected_index < state.visible_entry_names.size();
}

const std::string& SelectedBrowseEntryName(const ShellBrowseState& state) {
    return state.visible_entry_names[state.selected_index];
}

std::string BrowseEmptyMessage(const ShellBrowseState& state) {
    if (state.active && !state.filter_term.empty()) {
        return "(no matches)";
    }

    return "(empty)";
}

void ReplaceVisibleEntryNames(
    ShellBrowseState& state,
    std::vector<std::string> entry_names) {
    ::Cleanse(state.visible_entry_names);
    state.visible_entry_names = std::move(entry_names);
}

bool SelectBrowseEntry(
    ShellBrowseState& state,
    std::string_view entry_name) {
    if (!state.active) {
        return false;
    }

    for (std::size_t i = 0; i < state.visible_entry_names.size(); ++i) {
        if (state.visible_entry_names[i] == entry_name) {
            state.selected_index = i;
            return true;
        }
    }

    return false;
}

void ActivateBrowseState(
    VaultSession& session,
    ShellBrowseState& state,
    std::string_view filter_term) {
    state.active = true;
    ::Cleanse(state.filter_term);
    state.filter_term = std::string(filter_term);
    ReplaceVisibleEntryNames(
        state,
        FilterEntryNames(session.ListEntryNames(), filter_term));
    state.selected_index = 0;
}

void EnsureBrowseStateActive(
    VaultSession& session,
    ShellBrowseState& state,
    bool select_last) {
    if (!state.active) {
        ActivateBrowseState(session, state, "");
        if (select_last && !state.visible_entry_names.empty()) {
            state.selected_index = state.visible_entry_names.size() - 1;
        }
    }
}

void StepBrowseSelection(
    VaultSession& session,
    ShellBrowseState& state,
    bool move_forward) {
    EnsureBrowseStateActive(session, state, !move_forward);
    if (state.visible_entry_names.empty()) {
        return;
    }

    if (move_forward) {
        state.selected_index =
            (state.selected_index + 1) % state.visible_entry_names.size();
        return;
    }

    state.selected_index =
        (state.selected_index + state.visible_entry_names.size() - 1) %
        state.visible_entry_names.size();
}

void RefreshBrowseState(VaultSession& session, ShellBrowseState& state) {
    if (!state.active) {
        return;
    }

    const std::string previously_selected =
        HasBrowseSelection(state) ? SelectedBrowseEntryName(state) : "";
    const std::size_t previous_index = state.selected_index;
    ReplaceVisibleEntryNames(
        state,
        FilterEntryNames(session.ListEntryNames(), state.filter_term));
    if (state.visible_entry_names.empty()) {
        state.selected_index = 0;
        return;
    }

    if (!previously_selected.empty() &&
        SelectBrowseEntry(state, previously_selected)) {
        return;
    }

    if (previous_index >= state.visible_entry_names.size()) {
        state.selected_index = state.visible_entry_names.size() - 1;
        return;
    }

    state.selected_index = previous_index;
}

void FocusBrowseEntry(
    VaultSession& session,
    ShellBrowseState& state,
    std::string_view entry_name) {
    if (SelectBrowseEntry(state, entry_name)) {
        return;
    }

    ActivateBrowseState(session, state, "");
    static_cast<void>(SelectBrowseEntry(state, entry_name));
}

FrontendActionResult BuildBrowseResult(const ShellBrowseState& state) {
    return BuildFocusedListResult(
        state.visible_entry_names,
        HasBrowseSelection(state) ? SelectedBrowseEntryName(state) : "",
        state.filter_term,
        BrowseEmptyMessage(state));
}

FrontendActionResult FinalizeShellResult(
    FrontendStateMachine& state_machine,
    FrontendActionResult result) {
    static_cast<void>(state_machine.ApplyActionResult(result));
    return result;
}

std::optional<std::chrono::milliseconds> ReadShellIdleTimeout() {
    const char* raw_value = std::getenv(kShellIdleTimeoutEnv);
    if (raw_value == nullptr || *raw_value == '\0') {
        return std::nullopt;
    }

    int seconds = 0;
    const char* parse_end = raw_value + std::char_traits<char>::length(raw_value);
    const auto [end, error] = std::from_chars(raw_value, parse_end, seconds);
    if (error != std::errc() || end != parse_end || seconds <= 0) {
        throw std::runtime_error(
            "invalid ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS");
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds(seconds));
}

PromptReadStatus ReadShellCommandLine(
    const std::optional<std::chrono::milliseconds>& idle_timeout,
    bool session_unlocked,
    std::string& line) {
    if (session_unlocked && idle_timeout.has_value()) {
        return TryReadLineWithTimeout("zkvault> ", line, *idle_timeout);
    }

    return TryReadLine("zkvault> ", line) ? PromptReadStatus::kRead
                                          : PromptReadStatus::kEof;
}

FrontendActionResult HandleIdleTimeout(
    std::optional<VaultSession>& session,
    FrontendStateMachine& state_machine,
    ShellBrowseState& browse_state) {
    if (!session.has_value()) {
        return FinalizeShellResult(state_machine, BuildLockedResult());
    }

    static_cast<void>(state_machine.HandleIdleTimeout());
    session.reset();
    ResetBrowseState(browse_state);
    ClearTerminalScreenIfInteractive();
    return FinalizeShellResult(state_machine, BuildIdleLockedResult());
}

VaultSession OpenOrInitializeSession(FrontendStateMachine& state_machine) {
    const FrontendSessionState state =
        state_machine.HandleStartup(std::filesystem::exists(".zkv_master"));
    if (state == FrontendSessionState::kInitializingVault) {
        const std::string choice = ReadLine(
            "Vault not initialized. Create one now? [y/N]: ");
        if (choice != "y" && choice != "Y" && choice != "yes" && choice != "YES") {
            throw std::runtime_error("vault not initialized");
        }

        InitializeVaultRequest init_request{
            ReadConfirmedSecret(
                "Master password: ",
                "Confirm master password: ",
                "master passwords do not match")
        };
        auto init_request_guard = MakeScopedCleanse(init_request);
        const InitializeVaultResult result = InitializeVault(init_request);
        FrontendActionResult initialized =
            BuildInitializedResult(result.master_key_path);
        static_cast<void>(state_machine.ApplyActionResult(initialized));
        PrintFrontendResult(std::move(initialized));
        return VaultSession::Open(init_request.master_password);
    }

    std::string master_password = ReadSecret("Master password: ");
    auto master_password_guard = MakeScopedCleanse(master_password);
    state_machine.SetState(FrontendSessionState::kReady);
    return VaultSession::Open(master_password);
}

FrontendActionResult ExecuteShellCommand(
    std::optional<VaultSession>& session,
    const FrontendCommand& command,
    FrontendStateMachine& state_machine,
    ShellBrowseState& browse_state) {
    static_cast<void>(state_machine.HandleCommand(command.kind));

    if (command.kind == FrontendCommandKind::kHelp) {
        return FinalizeShellResult(state_machine, BuildShellHelpResult());
    }

    if (command.kind == FrontendCommandKind::kLock) {
        if (!session.has_value()) {
            throw std::runtime_error("vault is already locked");
        }

        session.reset();
        ResetBrowseState(browse_state);
        ClearTerminalScreenIfInteractive();
        return FinalizeShellResult(state_machine, BuildLockedResult());
    }

    if (command.kind == FrontendCommandKind::kUnlock) {
        if (session.has_value()) {
            throw std::runtime_error("vault is already unlocked");
        }

        std::string master_password = ReadSecret("Master password: ");
        auto master_password_guard = MakeScopedCleanse(master_password);
        session.emplace(VaultSession::Open(master_password));
        ResetBrowseState(browse_state);
        return FinalizeShellResult(state_machine, BuildUnlockedResult());
    }

    if (command.kind == FrontendCommandKind::kQuit) {
        return FinalizeShellResult(state_machine, BuildQuitResult());
    }

    if (!session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *session;

    if (command.kind == FrontendCommandKind::kList) {
        ActivateBrowseState(active_session, browse_state, "");
        return FinalizeShellResult(state_machine, BuildBrowseResult(browse_state));
    }

    if (command.kind == FrontendCommandKind::kFind) {
        ActivateBrowseState(active_session, browse_state, command.name);
        return FinalizeShellResult(state_machine, BuildBrowseResult(browse_state));
    }

    if (command.kind == FrontendCommandKind::kNext) {
        StepBrowseSelection(active_session, browse_state, true);
        return FinalizeShellResult(state_machine, BuildBrowseResult(browse_state));
    }

    if (command.kind == FrontendCommandKind::kPrev) {
        StepBrowseSelection(active_session, browse_state, false);
        return FinalizeShellResult(state_machine, BuildBrowseResult(browse_state));
    }

    if (command.kind == FrontendCommandKind::kShow) {
        const std::string entry_name = command.name.empty()
                                           ? (HasBrowseSelection(browse_state)
                                                  ? SelectedBrowseEntryName(
                                                        browse_state)
                                                  : "")
                                           : command.name;
        if (entry_name.empty()) {
            throw std::runtime_error("no entry selected");
        }

        PasswordEntry entry = active_session.LoadEntry(entry_name);
        auto entry_guard = MakeScopedCleanse(entry);
        FocusBrowseEntry(active_session, browse_state, entry_name);
        return FinalizeShellResult(
            state_machine,
            BuildShowEntryResult(std::move(entry)));
    }

    if (command.kind == FrontendCommandKind::kAdd) {
        StorePasswordEntryRequest request{
            EntryMutationMode::kCreate,
            command.name,
            "",
            ReadSecret("Entry password: "),
            ReadLine("Note: ")
        };
        auto request_guard = MakeScopedCleanse(request);
        const StorePasswordEntryResult result = active_session.StoreEntry(request);
        RefreshBrowseState(active_session, browse_state);
        static_cast<void>(SelectBrowseEntry(browse_state, command.name));
        return FinalizeShellResult(
            state_machine,
            BuildStoredEntryResult(result.entry_path));
    }

    if (command.kind == FrontendCommandKind::kUpdate) {
        const ExactConfirmationRule rule =
            BuildOverwriteConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(state_machine.HandleConfirmationAccepted());
        StorePasswordEntryRequest request{
            EntryMutationMode::kUpdate,
            command.name,
            "",
            ReadSecret("Entry password: "),
            ReadLine("Note: ")
        };
        auto request_guard = MakeScopedCleanse(request);
        const StorePasswordEntryResult result = active_session.StoreEntry(request);
        RefreshBrowseState(active_session, browse_state);
        static_cast<void>(SelectBrowseEntry(browse_state, command.name));
        return FinalizeShellResult(
            state_machine,
            BuildUpdatedResult(result.entry_path));
    }

    if (command.kind == FrontendCommandKind::kDelete) {
        const ExactConfirmationRule rule =
            BuildDeletionConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(state_machine.HandleConfirmationAccepted());
        const RemovePasswordEntryResult result =
            active_session.RemoveEntry(command.name);
        RefreshBrowseState(active_session, browse_state);
        return FinalizeShellResult(
            state_machine,
            BuildDeletedEntryResult(result.entry_path));
    }

    if (command.kind == FrontendCommandKind::kChangeMasterPassword) {
        const ExactConfirmationRule rule =
            BuildMasterPasswordRotationConfirmationRule();
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(state_machine.HandleConfirmationAccepted());
        std::string new_master_password = ReadConfirmedSecret(
            "New master password: ",
            "Confirm new master password: ",
            "new master passwords do not match");
        auto new_master_password_guard = MakeScopedCleanse(new_master_password);
        const RotateMasterPasswordResult result =
            active_session.RotateMasterPassword(new_master_password);
        RefreshBrowseState(active_session, browse_state);
        return FinalizeShellResult(
            state_machine,
            BuildUpdatedResult(result.master_key_path));
    }

    throw std::runtime_error("unknown shell command");
}

}  // namespace

int RunInteractiveShell() {
    FrontendStateMachine state_machine;
    std::optional<VaultSession> session = OpenOrInitializeSession(state_machine);
    ShellBrowseState browse_state;
    const std::optional<std::chrono::milliseconds> idle_timeout =
        ReadShellIdleTimeout();
    PrintFrontendResult(BuildShellReadyResult());

    std::string line;
    while (true) {
        const PromptReadStatus read_status =
            ReadShellCommandLine(idle_timeout, session.has_value(), line);
        if (read_status == PromptReadStatus::kTimedOut) {
            Cleanse(line);
            line.clear();
            PrintFrontendResult(
                HandleIdleTimeout(session, state_machine, browse_state));
            continue;
        }

        if (read_status == PromptReadStatus::kEof) {
            std::cout << '\n';
            return 0;
        }

        if (IsBlankShellInput(line)) {
            continue;
        }

        try {
            const FrontendCommand command = ParseShellCommand(line);
            FrontendActionResult result =
                ExecuteShellCommand(session, command, state_machine, browse_state);
            PrintFrontendResult(std::move(result));
            if (state_machine.state() == FrontendSessionState::kQuitRequested) {
                return 0;
            }
        } catch (const std::exception& ex) {
            FrontendError error = ClassifyFrontendError(ex.what());
            std::string output = RenderFrontendError(error);
            auto error_guard = MakeScopedCleanse(error);
            auto output_guard = MakeScopedCleanse(output);
            std::cout << output << '\n';
            static_cast<void>(state_machine.HandleFailure(session.has_value()));
        }
    }
}
