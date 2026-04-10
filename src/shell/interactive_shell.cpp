#include "shell/interactive_shell.hpp"

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
#include "terminal/prompt.hpp"

namespace {

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

VaultSession OpenOrInitializeSession(FrontendSessionState& state) {
    state = ResolveStartupState(std::filesystem::exists(".zkv_master"));
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
        PrintFrontendResult(BuildInitializedResult(result.master_key_path));
        state = FrontendSessionState::kReady;
        return VaultSession::Open(init_request.master_password);
    }

    std::string master_password = ReadSecret("Master password: ");
    auto master_password_guard = MakeScopedCleanse(master_password);
    state = FrontendSessionState::kReady;
    return VaultSession::Open(master_password);
}

FrontendActionResult ExecuteShellCommand(
    std::optional<VaultSession>& session,
    const FrontendCommand& command,
    FrontendSessionState& state) {
    state = ResolveCommandInputState(command.kind);

    if (command.kind == FrontendCommandKind::kHelp) {
        return BuildShellHelpResult();
    }

    if (command.kind == FrontendCommandKind::kLock) {
        if (!session.has_value()) {
            throw std::runtime_error("vault is already locked");
        }

        session.reset();
        return BuildLockedResult();
    }

    if (command.kind == FrontendCommandKind::kUnlock) {
        if (session.has_value()) {
            throw std::runtime_error("vault is already unlocked");
        }

        std::string master_password = ReadSecret("Master password: ");
        auto master_password_guard = MakeScopedCleanse(master_password);
        session.emplace(VaultSession::Open(master_password));
        return BuildUnlockedResult();
    }

    if (command.kind == FrontendCommandKind::kQuit) {
        return BuildQuitResult();
    }

    if (!session.has_value()) {
        throw std::runtime_error("vault is locked");
    }

    VaultSession& active_session = *session;

    if (command.kind == FrontendCommandKind::kList) {
        return BuildListResult(active_session.ListEntryNames(), "(empty)");
    }

    if (command.kind == FrontendCommandKind::kFind) {
        return BuildListResult(
            FilterEntryNames(active_session.ListEntryNames(), command.name),
            "(no matches)");
    }

    if (command.kind == FrontendCommandKind::kShow) {
        PasswordEntry entry = active_session.LoadEntry(command.name);
        auto entry_guard = MakeScopedCleanse(entry);
        return BuildShowEntryResult(std::move(entry));
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
        return BuildStoredEntryResult(result.entry_path);
    }

    if (command.kind == FrontendCommandKind::kUpdate) {
        const ExactConfirmationRule rule =
            BuildOverwriteConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        state = ResolvePostConfirmationState(command.kind);
        StorePasswordEntryRequest request{
            EntryMutationMode::kUpdate,
            command.name,
            "",
            ReadSecret("Entry password: "),
            ReadLine("Note: ")
        };
        auto request_guard = MakeScopedCleanse(request);
        const StorePasswordEntryResult result = active_session.StoreEntry(request);
        return BuildUpdatedResult(result.entry_path);
    }

    if (command.kind == FrontendCommandKind::kDelete) {
        const ExactConfirmationRule rule =
            BuildDeletionConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        const RemovePasswordEntryResult result =
            active_session.RemoveEntry(command.name);
        return BuildDeletedEntryResult(result.entry_path);
    }

    if (command.kind == FrontendCommandKind::kChangeMasterPassword) {
        const ExactConfirmationRule rule =
            BuildMasterPasswordRotationConfirmationRule();
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        state = ResolvePostConfirmationState(command.kind);
        std::string new_master_password = ReadConfirmedSecret(
            "New master password: ",
            "Confirm new master password: ",
            "new master passwords do not match");
        auto new_master_password_guard = MakeScopedCleanse(new_master_password);
        const RotateMasterPasswordResult result =
            active_session.RotateMasterPassword(new_master_password);
        return BuildUpdatedResult(result.master_key_path);
    }

    throw std::runtime_error("unknown shell command");
}

}  // namespace

int RunInteractiveShell() {
    FrontendSessionState state = FrontendSessionState::kInitializingVault;
    std::optional<VaultSession> session = OpenOrInitializeSession(state);
    PrintFrontendResult(BuildShellReadyResult());

    std::string line;
    while (true) {
        if (!TryReadLine("zkvault> ", line)) {
            std::cout << '\n';
            return 0;
        }

        if (IsBlankShellInput(line)) {
            continue;
        }

        try {
            const FrontendCommand command = ParseShellCommand(line);
            FrontendActionResult result = ExecuteShellCommand(session, command, state);
            state = result.state;
            PrintFrontendResult(std::move(result));
            if (state == FrontendSessionState::kQuitRequested) {
                return 0;
            }
        } catch (const std::exception& ex) {
            state = FrontendSessionState::kRecoveringFromFailure;
            FrontendError error = ClassifyFrontendError(ex.what());
            std::string output = RenderFrontendError(error);
            auto error_guard = MakeScopedCleanse(error);
            auto output_guard = MakeScopedCleanse(output);
            std::cout << output << '\n';
            state = session.has_value()
                        ? FrontendSessionState::kReady
                        : FrontendSessionState::kLocked;
        }
    }
}
