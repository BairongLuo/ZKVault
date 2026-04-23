#include "shell/shell_runtime.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/vault_app.hpp"
#include "terminal/prompt.hpp"

namespace {

constexpr const char* kShellIdleTimeoutEnv =
    "ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS";

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
    Cleanse(state.visible_entry_names);
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
    Cleanse(state.filter_term);
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

void SyncBrowseStateAfterMutation(
    VaultSession& session,
    ShellBrowseState& state,
    std::string_view entry_name) {
    if (!state.active) {
        ActivateBrowseState(session, state, "");
    } else {
        RefreshBrowseState(session, state);
    }

    if (entry_name.empty()) {
        return;
    }

    static_cast<void>(SelectBrowseEntry(state, entry_name));
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

void RememberShellViewContext(
    const FrontendActionResult& result,
    ShellViewContext& context) {
    if (result.state == FrontendSessionState::kShowingEntry) {
        Cleanse(context.entry_name);
        context.entry_name = result.entry.name;
        return;
    }

    Cleanse(context.entry_name);
    context.entry_name.clear();
}

std::optional<FrontendActionResult> BuildRecoveredViewResult(
    const std::optional<VaultSession>& session,
    const ShellBrowseState& browse_state,
    const ShellViewContext& view_context,
    FrontendSessionState recovered_state) {
    if (recovered_state == FrontendSessionState::kShowingHelp) {
        return BuildShellHelpResult();
    }

    if (recovered_state == FrontendSessionState::kShowingList) {
        return BuildBrowseResult(browse_state);
    }

    if (recovered_state != FrontendSessionState::kShowingEntry ||
        !session.has_value() ||
        view_context.entry_name.empty()) {
        return std::nullopt;
    }

    PasswordEntry entry = session->LoadEntry(view_context.entry_name);
    auto entry_guard = MakeScopedCleanse(entry);
    return BuildShowEntryResult(std::move(entry));
}

FrontendActionResult FinalizeShellResult(
    FrontendStateMachine& state_machine,
    ShellViewContext& view_context,
    FrontendActionResult result) {
    static_cast<void>(state_machine.ApplyActionResult(result));
    RememberShellViewContext(result, view_context);
    return result;
}

VaultSession OpenOrInitializeSession(
    FrontendStateMachine& state_machine,
    std::optional<FrontendActionResult>& startup_result) {
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
        startup_result = BuildInitializedResult(result.master_key_path);
        static_cast<void>(state_machine.ApplyActionResult(*startup_result));
        return VaultSession::Open(init_request.master_password);
    }

    std::string master_password = ReadSecret("Master password: ");
    auto master_password_guard = MakeScopedCleanse(master_password);
    state_machine.SetState(FrontendSessionState::kReady);
    return VaultSession::Open(master_password);
}

}  // namespace

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

OpenShellRuntimeResult OpenOrInitializeShellRuntime() {
    OpenShellRuntimeResult result;
    result.runtime.session.emplace(
        OpenOrInitializeSession(
            result.runtime.state_machine,
            result.startup_result));
    return result;
}

FrontendActionResult ExecuteShellCommand(
    ShellRuntimeState& runtime,
    const FrontendCommand& command) {
    static_cast<void>(runtime.state_machine.HandleCommand(command.kind));
    return ExecutePreparedShellCommand(runtime, command);
}

FrontendActionResult ExecutePreparedShellCommand(
    ShellRuntimeState& runtime,
    const FrontendCommand& command) {
    if (command.kind == FrontendCommandKind::kHelp) {
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildShellHelpResult());
    }

    if (command.kind == FrontendCommandKind::kLock) {
        if (!runtime.session.has_value()) {
            throw std::runtime_error("vault is already locked");
        }

        runtime.session.reset();
        ResetBrowseState(runtime.browse_state);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildLockedResult());
    }

    if (command.kind == FrontendCommandKind::kUnlock) {
        if (runtime.session.has_value()) {
            throw std::runtime_error("vault is already unlocked");
        }

        std::string master_password = ReadSecret("Master password: ");
        auto master_password_guard = MakeScopedCleanse(master_password);
        runtime.session.emplace(VaultSession::Open(master_password));
        ResetBrowseState(runtime.browse_state);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildUnlockedResult());
    }

    if (command.kind == FrontendCommandKind::kQuit) {
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildQuitResult());
    }

    if (!runtime.session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    if (command.kind == FrontendCommandKind::kList) {
        VaultSession& active_session = *runtime.session;
        ActivateBrowseState(active_session, runtime.browse_state, "");
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildBrowseResult(runtime.browse_state));
    }

    if (command.kind == FrontendCommandKind::kFind) {
        VaultSession& active_session = *runtime.session;
        ActivateBrowseState(active_session, runtime.browse_state, command.name);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildBrowseResult(runtime.browse_state));
    }

    if (command.kind == FrontendCommandKind::kNext) {
        VaultSession& active_session = *runtime.session;
        StepBrowseSelection(active_session, runtime.browse_state, true);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildBrowseResult(runtime.browse_state));
    }

    if (command.kind == FrontendCommandKind::kPrev) {
        VaultSession& active_session = *runtime.session;
        StepBrowseSelection(active_session, runtime.browse_state, false);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildBrowseResult(runtime.browse_state));
    }

    if (command.kind == FrontendCommandKind::kShow) {
        VaultSession& active_session = *runtime.session;
        const std::string entry_name = command.name.empty()
                                           ? (HasBrowseSelection(runtime.browse_state)
                                                  ? SelectedBrowseEntryName(
                                                        runtime.browse_state)
                                                  : "")
                                           : command.name;
        if (entry_name.empty()) {
            throw std::runtime_error("no entry selected");
        }

        PasswordEntry entry = active_session.LoadEntry(entry_name);
        auto entry_guard = MakeScopedCleanse(entry);
        FocusBrowseEntry(active_session, runtime.browse_state, entry_name);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildShowEntryResult(std::move(entry)));
    }

    if (command.kind == FrontendCommandKind::kAdd) {
        std::string password = ReadSecret("Entry password: ");
        auto password_guard = MakeScopedCleanse(password);
        std::string note = ReadLine("Note: ");
        auto note_guard = MakeScopedCleanse(note);
        return StoreShellEntryWithContent(
            runtime,
            EntryMutationMode::kCreate,
            command.name,
            password,
            note);
    }

    if (command.kind == FrontendCommandKind::kUpdate) {
        const ExactConfirmationRule rule =
            BuildOverwriteConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
        std::string password = ReadSecret("Entry password: ");
        auto password_guard = MakeScopedCleanse(password);
        std::string note = ReadLine("Note: ");
        auto note_guard = MakeScopedCleanse(note);
        return StoreShellEntryWithContent(
            runtime,
            EntryMutationMode::kUpdate,
            command.name,
            password,
            note);
    }

    if (command.kind == FrontendCommandKind::kDelete) {
        const ExactConfirmationRule rule =
            BuildDeletionConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
        return RemoveShellEntryByName(runtime, command.name);
    }

    if (command.kind == FrontendCommandKind::kChangeMasterPassword) {
        VaultSession& active_session = *runtime.session;
        const ExactConfirmationRule rule =
            BuildMasterPasswordRotationConfirmationRule();
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        static_cast<void>(runtime.state_machine.HandleConfirmationAccepted());
        std::string new_master_password = ReadConfirmedSecret(
            "New master password: ",
            "Confirm new master password: ",
            "new master passwords do not match");
        auto new_master_password_guard = MakeScopedCleanse(new_master_password);
        const RotateMasterPasswordResult result =
            active_session.RotateMasterPassword(new_master_password);
        RefreshBrowseState(active_session, runtime.browse_state);
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildUpdatedResult(result.master_key_path));
    }

    throw std::runtime_error("unknown shell command");
}

FrontendActionResult StoreShellEntryWithContent(
    ShellRuntimeState& runtime,
    EntryMutationMode mode,
    const std::string& entry_name,
    const std::string& password,
    const std::string& note) {
    if (!runtime.session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *runtime.session;
    StorePasswordEntryRequest request{
        mode,
        entry_name,
        "",
        password,
        note
    };
    auto request_guard = MakeScopedCleanse(request);
    const StorePasswordEntryResult result = active_session.StoreEntry(request);
    SyncBrowseStateAfterMutation(
        active_session,
        runtime.browse_state,
        entry_name);
    return FinalizeShellResult(
        runtime.state_machine,
        runtime.view_context,
        mode == EntryMutationMode::kCreate
            ? BuildStoredEntryResult(result.entry_path)
            : BuildUpdatedResult(result.entry_path));
}

FrontendActionResult RotateShellMasterPassword(
    ShellRuntimeState& runtime,
    const std::string& new_master_password) {
    if (!runtime.session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *runtime.session;
    const RotateMasterPasswordResult result =
        active_session.RotateMasterPassword(new_master_password);
    return FinalizeShellResult(
        runtime.state_machine,
        runtime.view_context,
        BuildUpdatedResult(result.master_key_path));
}

FrontendActionResult RemoveShellEntryByName(
    ShellRuntimeState& runtime,
    const std::string& entry_name) {
    if (!runtime.session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *runtime.session;
    const RemovePasswordEntryResult result =
        active_session.RemoveEntry(entry_name);
    RefreshBrowseState(active_session, runtime.browse_state);
    return FinalizeShellResult(
        runtime.state_machine,
        runtime.view_context,
        BuildDeletedEntryResult(result.entry_path));
}

FrontendActionResult ShowCurrentShellBrowseView(
    ShellRuntimeState& runtime) {
    if (!runtime.session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *runtime.session;
    if (!runtime.browse_state.active) {
        ActivateBrowseState(active_session, runtime.browse_state, "");
    }

    return FinalizeShellResult(
        runtime.state_machine,
        runtime.view_context,
        BuildBrowseResult(runtime.browse_state));
}

FrontendActionResult HandleShellIdleTimeout(ShellRuntimeState& runtime) {
    if (!runtime.session.has_value()) {
        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            BuildLockedResult());
    }

    static_cast<void>(runtime.state_machine.HandleIdleTimeout());
    runtime.session.reset();
    ResetBrowseState(runtime.browse_state);
    return FinalizeShellResult(
        runtime.state_machine,
        runtime.view_context,
        BuildIdleLockedResult());
}

std::optional<FrontendActionResult> RecoverShellViewAfterFailure(
    ShellRuntimeState& runtime) {
    const FrontendSessionState recovered_state =
        runtime.state_machine.HandleFailure(runtime.session.has_value());
    try {
        std::optional<FrontendActionResult> recovered_result =
            BuildRecoveredViewResult(
                runtime.session,
                runtime.browse_state,
                runtime.view_context,
                recovered_state);
        if (!recovered_result.has_value()) {
            return std::nullopt;
        }

        return FinalizeShellResult(
            runtime.state_machine,
            runtime.view_context,
            std::move(*recovered_result));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

ShellBrowseSnapshot SnapshotShellBrowseState(const ShellRuntimeState& runtime) {
    if (!runtime.session.has_value()) {
        return ShellBrowseSnapshot{
            false,
            "",
            "",
            {},
            "unlock to browse entries"
        };
    }

    if (!runtime.browse_state.active) {
        return ShellBrowseSnapshot{
            false,
            "",
            "",
            runtime.session->ListEntryNames(),
            "(empty)"
        };
    }

    return ShellBrowseSnapshot{
        true,
        runtime.browse_state.filter_term,
        HasBrowseSelection(runtime.browse_state)
            ? SelectedBrowseEntryName(runtime.browse_state)
            : "",
        runtime.browse_state.visible_entry_names,
        BrowseEmptyMessage(runtime.browse_state)
    };
}

bool ShellSessionUnlocked(const ShellRuntimeState& runtime) noexcept {
    return runtime.session.has_value();
}
