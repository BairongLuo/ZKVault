#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "app/frontend_contract.hpp"

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void RequireThrows(
    const std::function<void()>& fn,
    std::string_view expected_message) {
    try {
        fn();
    } catch (const std::exception& ex) {
        Require(ex.what() == expected_message,
                "exception message should match expected frontend contract");
        return;
    }

    throw std::runtime_error("expected function to throw");
}

void TestCliUsageCommands() {
    const auto& commands = CliUsageCommands();
    Require(commands.size() == 8, "cli usage should expose 8 commands");
    Require(commands[0] == "zkvault init", "cli usage should include init");
    Require(commands[1] == "zkvault shell", "cli usage should include shell");
    Require(commands[2] == "zkvault change-master-password",
            "cli usage should include master password rotation");
    Require(commands[7] == "zkvault list", "cli usage should include list");
}

void TestShellHelpCommands() {
    const auto& commands = ShellHelpCommands();
    Require(commands.size() == 8, "shell help should expose 8 commands");
    Require(commands[0] == "help", "shell help should include help");
    Require(commands[2] == "show <name>", "shell help should include show");
    Require(commands[6] == "change-master-password",
            "shell help should include rotation");
    Require(commands[7] == "quit", "shell help should include quit");
}

void TestShellCommandParsing() {
    const FrontendCommand show = ParseShellCommand("show email");
    Require(show.kind == FrontendCommandKind::kShow,
            "show command should parse as show");
    Require(show.name == "email", "show command should parse entry name");

    const FrontendCommand add = ParseShellCommand(" add   bank ");
    Require(add.kind == FrontendCommandKind::kAdd,
            "add command should parse as add");
    Require(add.name == "bank", "add command should ignore extra spacing");

    const FrontendCommand quit = ParseShellCommand("exit");
    Require(quit.kind == FrontendCommandKind::kQuit,
            "exit should map to quit");

    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("update")); },
        "usage: update <name>");
    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("list extra")); },
        "usage: list");
    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("unknown")); },
        "unknown shell command");
}

void TestBlankShellInput() {
    Require(IsBlankShellInput(""), "empty shell input should be blank");
    Require(IsBlankShellInput("  \t \r\n"),
            "whitespace-only shell input should be blank");
    Require(!IsBlankShellInput("list"),
            "non-empty command should not be blank");
}

void TestConfirmationRules() {
    const ExactConfirmationRule overwrite =
        BuildOverwriteConfirmationRule("email");
    Require(overwrite.prompt == "Type the entry name to confirm overwrite: ",
            "overwrite prompt should match");
    Require(overwrite.expected_value == "email",
            "overwrite expected value should match");
    Require(overwrite.mismatch_error == "entry overwrite cancelled",
            "overwrite mismatch error should match");

    const ExactConfirmationRule deletion =
        BuildDeletionConfirmationRule("bank");
    Require(deletion.prompt == "Type the entry name to confirm deletion: ",
            "deletion prompt should match");
    Require(deletion.expected_value == "bank",
            "deletion expected value should match");
    Require(deletion.mismatch_error == "entry deletion cancelled",
            "deletion mismatch error should match");

    const ExactConfirmationRule rotation =
        BuildMasterPasswordRotationConfirmationRule();
    Require(rotation.prompt ==
                "Type CHANGE to confirm master password rotation: ",
            "rotation prompt should match");
    Require(rotation.expected_value == "CHANGE",
            "rotation expected value should match");
    Require(rotation.mismatch_error == "master password rotation cancelled",
            "rotation mismatch error should match");
}

void TestSessionStateMapping() {
    Require(ResolveStartupState(false) ==
                FrontendSessionState::kInitializingVault,
            "missing vault should map to initializing state");
    Require(ResolveStartupState(true) == FrontendSessionState::kReady,
            "existing vault should map to ready state");

    Require(ResolveCommandInputState(FrontendCommandKind::kAdd) ==
                FrontendSessionState::kEditingEntryForm,
            "add should enter editing state");
    Require(ResolveCommandInputState(FrontendCommandKind::kUpdate) ==
                FrontendSessionState::kConfirmingEntryOverwrite,
            "update should enter overwrite confirmation state first");
    Require(ResolveCommandInputState(FrontendCommandKind::kDelete) ==
                FrontendSessionState::kConfirmingEntryDeletion,
            "delete should enter deletion confirmation state");
    Require(ResolveCommandInputState(
                FrontendCommandKind::kChangeMasterPassword) ==
                FrontendSessionState::kConfirmingMasterPasswordRotation,
            "rotation should enter master password confirmation state");
    Require(ResolveCommandInputState(FrontendCommandKind::kList) ==
                FrontendSessionState::kShowingList,
            "list should map to list state");
    Require(ResolveCommandInputState(FrontendCommandKind::kShow) ==
                FrontendSessionState::kShowingEntry,
            "show should map to entry state");
    Require(ResolveCommandInputState(FrontendCommandKind::kHelp) ==
                FrontendSessionState::kShowingHelp,
            "help should map to help state");
    Require(ResolveCommandInputState(FrontendCommandKind::kQuit) ==
                FrontendSessionState::kQuitRequested,
            "quit should map to quit state");

    Require(ResolvePostConfirmationState(FrontendCommandKind::kUpdate) ==
                FrontendSessionState::kEditingEntryForm,
            "confirmed update should continue into entry form state");
    Require(ResolvePostConfirmationState(
                FrontendCommandKind::kChangeMasterPassword) ==
                FrontendSessionState::kEditingMasterPasswordForm,
            "confirmed rotation should continue into master password form state");
    Require(ResolvePostConfirmationState(FrontendCommandKind::kDelete) ==
                FrontendSessionState::kReady,
            "confirmed delete should return to ready state");
}

void TestOutputFormatting() {
    Require(FormatStoredEntryMessage("data/email.zkv") ==
                "saved to data/email.zkv",
            "store output should match");
    Require(FormatUpdatedPathMessage(".zkv_master") == "updated .zkv_master",
            "updated output should match");
    Require(FormatDeletedEntryMessage("data/email.zkv") ==
                "deleted data/email.zkv",
            "delete output should match");
}

void TestActionResultsAndRendering() {
    const FrontendActionResult cli_usage = BuildCliUsageResult();
    Require(cli_usage.state == FrontendSessionState::kShowingHelp,
            "cli usage should map to help state");
    Require(cli_usage.payload_kind == FrontendPayloadKind::kText,
            "cli usage should be rendered as text");
    Require(RenderFrontendActionResult(cli_usage).find("Usage:") == 0,
            "cli usage should render with Usage header");
    Require(RenderFrontendActionResult(cli_usage).find("zkvault shell") !=
                std::string::npos,
            "cli usage should render shell command");

    const FrontendActionResult shell_ready = BuildShellReadyResult();
    Require(shell_ready.state == FrontendSessionState::kReady,
            "shell ready should map to ready state");
    Require(RenderFrontendActionResult(shell_ready) ==
                "shell ready; type help for commands",
            "shell ready message should match");

    const FrontendActionResult shell_help = BuildShellHelpResult();
    Require(shell_help.state == FrontendSessionState::kShowingHelp,
            "shell help should map to help state");
    Require(RenderFrontendActionResult(shell_help).find("Commands:") == 0,
            "shell help should render commands header");
    Require(RenderFrontendActionResult(shell_help).find("show <name>") !=
                std::string::npos,
            "shell help should render show command");

    const FrontendActionResult listed_entries =
        BuildListResult(std::vector<std::string>{"bank", "email"}, "(empty)");
    Require(listed_entries.state == FrontendSessionState::kShowingList,
            "list result should map to list state");
    Require(listed_entries.payload_kind == FrontendPayloadKind::kEntryNames,
            "list result should expose entry names");
    Require(RenderFrontendActionResult(listed_entries) == "bank\nemail",
            "list result should render newline-separated names");

    const FrontendActionResult empty_list =
        BuildListResult(std::vector<std::string>{}, "(empty)");
    Require(RenderFrontendActionResult(empty_list) == "(empty)",
            "empty list should render explicit placeholder");

    const FrontendActionResult shown_entry = BuildShowEntryResult(PasswordEntry{
        "email",
        "entry-password",
        "note",
        "created-at",
        "updated-at"
    });
    Require(shown_entry.state == FrontendSessionState::kShowingEntry,
            "show result should map to entry state");
    Require(shown_entry.payload_kind == FrontendPayloadKind::kEntry,
            "show result should expose entry payload");
    Require(RenderFrontendActionResult(shown_entry).find(
                "\"password\": \"entry-password\"") != std::string::npos,
            "show result should render entry JSON");

    const FrontendActionResult initialized =
        BuildInitializedResult(".zkv_master");
    Require(RenderFrontendActionResult(initialized) ==
                "initialized .zkv_master",
            "initialized result should render shared message");

    const FrontendActionResult stored =
        BuildStoredEntryResult("data/email.zkv");
    Require(RenderFrontendActionResult(stored) == "saved to data/email.zkv",
            "stored result should render shared message");

    const FrontendActionResult updated = BuildUpdatedResult(".zkv_master");
    Require(RenderFrontendActionResult(updated) == "updated .zkv_master",
            "updated result should render shared message");

    const FrontendActionResult deleted =
        BuildDeletedEntryResult("data/email.zkv");
    Require(RenderFrontendActionResult(deleted) == "deleted data/email.zkv",
            "deleted result should render shared message");

    const FrontendActionResult quit = BuildQuitResult();
    Require(quit.state == FrontendSessionState::kQuitRequested,
            "quit result should map to quit state");
    Require(quit.payload_kind == FrontendPayloadKind::kNone,
            "quit result should not produce output");
    Require(RenderFrontendActionResult(quit).empty(),
            "quit result should render empty output");
}

void TestErrorClassification() {
    Require(ClassifyFrontendError("usage: update <name>").kind ==
                FrontendErrorKind::kUsage,
            "usage errors should be classified");
    Require(ClassifyFrontendError("unknown shell command").kind ==
                FrontendErrorKind::kUnknownCommand,
            "unknown command errors should be classified");
    Require(ClassifyFrontendError("entry already exists").kind ==
                FrontendErrorKind::kConflict,
            "conflict errors should be classified");
    Require(ClassifyFrontendError("entry does not exist").kind ==
                FrontendErrorKind::kNotFound,
            "not-found errors should be classified");
    Require(ClassifyFrontendError("entry overwrite cancelled").kind ==
                FrontendErrorKind::kConfirmationRejected,
            "confirmation errors should be classified");
    Require(ClassifyFrontendError("input cancelled").kind ==
                FrontendErrorKind::kInputCancelled,
            "input cancellation should be classified");
    Require(ClassifyFrontendError(
                "entry name may only contain letters, digits, '.', '-' and '_'")
                .kind == FrontendErrorKind::kValidation,
            "validation errors should be classified");
    Require(ClassifyFrontendError("AES-256-GCM decryption failed").kind ==
                FrontendErrorKind::kAuthentication,
            "authentication errors should be classified");
    const FrontendError storage_error =
        ClassifyFrontendError("invalid .zkv_master JSON");
    Require(storage_error.kind == FrontendErrorKind::kStorage,
            "storage errors should be classified");
    Require(RenderFrontendError(storage_error) ==
                "error: invalid .zkv_master JSON",
            "frontend errors should render consistently");
    Require(ClassifyFrontendError("something unexpected").kind ==
                FrontendErrorKind::kUnknown,
            "unknown errors should stay unknown");
}

}  // namespace

int main() {
    try {
        TestCliUsageCommands();
        TestShellHelpCommands();
        TestShellCommandParsing();
        TestBlankShellInput();
        TestConfirmationRules();
        TestSessionStateMapping();
        TestOutputFormatting();
        TestActionResultsAndRendering();
        TestErrorClassification();
        return 0;
    } catch (const std::exception& ex) {
        return (std::fprintf(stderr, "frontend contract test failed: %s\n", ex.what()), 1);
    }
}
