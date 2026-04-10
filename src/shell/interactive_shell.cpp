#include "shell/interactive_shell.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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
    VaultSession& session,
    const FrontendCommand& command,
    FrontendSessionState& state) {
    state = ResolveCommandInputState(command.kind);

    if (command.kind == FrontendCommandKind::kHelp) {
        return BuildShellHelpResult();
    }

    if (command.kind == FrontendCommandKind::kList) {
        return BuildListResult(session.ListEntryNames(), "(empty)");
    }

    if (command.kind == FrontendCommandKind::kShow) {
        PasswordEntry entry = session.LoadEntry(command.name);
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
        const StorePasswordEntryResult result = session.StoreEntry(request);
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
        const StorePasswordEntryResult result = session.StoreEntry(request);
        return BuildUpdatedResult(result.entry_path);
    }

    if (command.kind == FrontendCommandKind::kDelete) {
        const ExactConfirmationRule rule =
            BuildDeletionConfirmationRule(command.name);
        RequireExactConfirmation(
            rule.prompt,
            rule.expected_value,
            rule.mismatch_error);
        const RemovePasswordEntryResult result = session.RemoveEntry(command.name);
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
            session.RotateMasterPassword(new_master_password);
        return BuildUpdatedResult(result.master_key_path);
    }

    if (command.kind == FrontendCommandKind::kQuit) {
        return BuildQuitResult();
    }

    throw std::runtime_error("unknown shell command");
}

}  // namespace

int RunInteractiveShell() {
    FrontendSessionState state = FrontendSessionState::kInitializingVault;
    VaultSession session = OpenOrInitializeSession(state);
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
            state = FrontendSessionState::kReady;
        }
    }
}
