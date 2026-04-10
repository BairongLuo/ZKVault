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
    Require(commands.size() == 13, "shell help should expose 13 commands");
    Require(commands[0] == "help", "shell help should include help");
    Require(commands[2] == "find <term>", "shell help should include find");
    Require(commands[3] == "next", "shell help should include next");
    Require(commands[4] == "prev", "shell help should include prev");
    Require(commands[5] == "show [name]", "shell help should include show");
    Require(commands[9] == "change-master-password",
            "shell help should include rotation");
    Require(commands[10] == "lock", "shell help should include lock");
    Require(commands[11] == "unlock", "shell help should include unlock");
    Require(commands[12] == "quit", "shell help should include quit");
}

void TestShellCommandParsing() {
    const FrontendCommand find = ParseShellCommand("find MAIL");
    Require(find.kind == FrontendCommandKind::kFind,
            "find command should parse as find");
    Require(find.name == "MAIL", "find command should preserve search term");

    const FrontendCommand next = ParseShellCommand("next");
    Require(next.kind == FrontendCommandKind::kNext,
            "next command should parse as next");

    const FrontendCommand prev = ParseShellCommand(" prev ");
    Require(prev.kind == FrontendCommandKind::kPrev,
            "prev command should parse as prev");

    const FrontendCommand show_selected = ParseShellCommand("show");
    Require(show_selected.kind == FrontendCommandKind::kShow,
            "bare show should parse as show");
    Require(show_selected.name.empty(),
            "bare show should leave entry name empty");

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

    const FrontendCommand lock = ParseShellCommand("lock");
    Require(lock.kind == FrontendCommandKind::kLock,
            "lock should map to lock");

    const FrontendCommand unlock = ParseShellCommand(" unlock ");
    Require(unlock.kind == FrontendCommandKind::kUnlock,
            "unlock should map to unlock");

    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("update")); },
        "usage: update <name>");
    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("list extra")); },
        "usage: list");
    RequireThrows(
        [] { static_cast<void>(ParseShellCommand("show email extra")); },
        "usage: show [name]");
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

    Require(ResolveCommandInputState(FrontendCommandKind::kLock) ==
                FrontendSessionState::kLocked,
            "lock should enter locked state");
    Require(ResolveCommandInputState(FrontendCommandKind::kUnlock) ==
                FrontendSessionState::kUnlockingSession,
            "unlock should enter unlocking state");
    Require(ResolveCommandInputState(FrontendCommandKind::kFind) ==
                FrontendSessionState::kShowingList,
            "find should reuse list state");
    Require(ResolveCommandInputState(FrontendCommandKind::kNext) ==
                FrontendSessionState::kShowingList,
            "next should stay in list state");
    Require(ResolveCommandInputState(FrontendCommandKind::kPrev) ==
                FrontendSessionState::kShowingList,
            "prev should stay in list state");
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

void TestExplicitStateMachineTransitions() {
    const auto& transitions = FrontendStateTransitions();
    Require(!transitions.empty(),
            "frontend state machine should expose explicit transitions");

    Require(ResolveStartupEvent(false) ==
                FrontendStateEvent::kVaultMissingAtStartup,
            "missing vault should map to startup-missing event");
    Require(ResolveStartupEvent(true) ==
                FrontendStateEvent::kVaultExistsAtStartup,
            "existing vault should map to startup-ready event");
    Require(ResolveCommandEvent(FrontendCommandKind::kShow) ==
                FrontendStateEvent::kShowRequested,
            "show command should map to show-requested event");

    Require(ResolveStateTransition(
                FrontendSessionState::kInitializingVault,
                FrontendStateEvent::kVaultExistsAtStartup) ==
                FrontendSessionState::kReady,
            "existing vault should transition startup state to ready");
    Require(ResolveStateTransition(
                FrontendSessionState::kLocked,
                FrontendStateEvent::kHelpRequested) ==
                FrontendSessionState::kShowingHelp,
            "locked state should still allow help view");
    Require(ResolveStateTransition(
                FrontendSessionState::kShowingHelp,
                FrontendStateEvent::kUnlockRequested) ==
                FrontendSessionState::kUnlockingSession,
            "display states should allow unlock transition");
    Require(ResolveStateTransition(
                FrontendSessionState::kShowingList,
                FrontendStateEvent::kShowRequested) ==
                FrontendSessionState::kShowingEntry,
            "list view should transition into entry view");
    Require(ResolveStateTransition(
                FrontendSessionState::kShowingEntry,
                FrontendStateEvent::kIdleTimeoutElapsed) ==
                FrontendSessionState::kLocked,
            "visible states should allow idle lock transition");
    Require(ResolveStateTransition(
                FrontendSessionState::kConfirmingEntryOverwrite,
                FrontendStateEvent::kConfirmationAccepted) ==
                FrontendSessionState::kEditingEntryForm,
            "overwrite confirmation should lead into entry form");
    Require(ResolveStateTransition(
                FrontendSessionState::kConfirmingEntryDeletion,
                FrontendStateEvent::kConfirmationAccepted) ==
                FrontendSessionState::kReady,
            "delete confirmation should resolve to ready");
    Require(ResolveStateTransition(
                FrontendSessionState::kShowingEntry,
                FrontendStateEvent::kOperationFailed) ==
                FrontendSessionState::kRecoveringFromFailure,
            "view states should recover through failure state");
    Require(ResolveStateTransition(
                FrontendSessionState::kRecoveringFromFailure,
                FrontendStateEvent::kRecoveryCompletedWhileUnlocked) ==
                FrontendSessionState::kReady,
            "recovery should return to ready when session remains unlocked");
    Require(ResolveStateTransition(
                FrontendSessionState::kRecoveringFromFailure,
                FrontendStateEvent::kRecoveryCompletedWhileLocked) ==
                FrontendSessionState::kLocked,
            "recovery should return to locked when session is absent");

    RequireThrows(
        [] {
            static_cast<void>(ResolveStateTransition(
                FrontendSessionState::kReady,
                FrontendStateEvent::kConfirmationAccepted));
        },
        "unsupported frontend state transition");
}

void TestFrontendStateMachine() {
    FrontendStateMachine state_machine;
    Require(state_machine.state() ==
                FrontendSessionState::kInitializingVault,
            "state machine should start in initializing state");

    Require(state_machine.HandleStartup(true) ==
                FrontendSessionState::kReady,
            "startup handler should move existing vaults to ready");
    Require(state_machine.HandleCommand(FrontendCommandKind::kUpdate) ==
                FrontendSessionState::kConfirmingEntryOverwrite,
            "command handler should enter overwrite confirmation");
    Require(state_machine.HandleConfirmationAccepted() ==
                FrontendSessionState::kEditingEntryForm,
            "confirmation handler should advance to entry form");
    Require(state_machine.HandleFailure(true) ==
                FrontendSessionState::kReady,
            "failure handler should recover to ready when session remains open");

    state_machine.SetState(FrontendSessionState::kShowingHelp);
    Require(state_machine.HandleCommand(FrontendCommandKind::kUnlock) ==
                FrontendSessionState::kUnlockingSession,
            "state machine should reuse shared transition table for unlock");
    state_machine.SetState(FrontendSessionState::kShowingEntry);
    Require(state_machine.HandleIdleTimeout() ==
                FrontendSessionState::kLocked,
            "state machine should support idle-lock transitions");
    Require(state_machine.HandleFailure(false) ==
                FrontendSessionState::kLocked,
            "failure handler should recover to locked when session is absent");

    const FrontendActionResult quit = BuildQuitResult();
    Require(state_machine.ApplyActionResult(quit) ==
                FrontendSessionState::kQuitRequested,
            "state machine should also accept result-driven state sync");
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
    Require(RenderFrontendActionResult(shell_help).find("show [name]") !=
                std::string::npos,
            "shell help should render show command");
    Require(RenderFrontendActionResult(shell_help).find("find <term>") !=
                std::string::npos,
            "shell help should render find command");
    Require(RenderFrontendActionResult(shell_help).find("next") !=
                std::string::npos,
            "shell help should render next command");
    Require(RenderFrontendActionResult(shell_help).find("unlock") !=
                std::string::npos,
            "shell help should render unlock command");

    const FrontendActionResult locked = BuildLockedResult();
    Require(locked.state == FrontendSessionState::kLocked,
            "locked result should map to locked state");
    Require(RenderFrontendActionResult(locked) == "vault locked",
            "locked result should render lock message");

    const FrontendActionResult idle_locked = BuildIdleLockedResult();
    Require(idle_locked.state == FrontendSessionState::kLocked,
            "idle-lock result should map to locked state");
    Require(RenderFrontendActionResult(idle_locked) ==
                "vault locked due to inactivity",
            "idle-lock result should render timeout message");

    const FrontendActionResult unlocked = BuildUnlockedResult();
    Require(unlocked.state == FrontendSessionState::kReady,
            "unlocked result should return to ready state");
    Require(RenderFrontendActionResult(unlocked) == "vault unlocked",
            "unlocked result should render unlock message");

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

    const FrontendActionResult focused_list = BuildFocusedListResult(
        std::vector<std::string>{"bank", "email"},
        "email",
        "mail",
        "(no matches)");
    Require(focused_list.state == FrontendSessionState::kShowingList,
            "focused list should map to list state");
    Require(focused_list.payload_kind == FrontendPayloadKind::kFocusedList,
            "focused list should expose focused list payload");
    Require(focused_list.focused_list.filter_term == "mail",
            "focused list should preserve filter term");
    Require(focused_list.focused_list.selected_name == "email",
            "focused list should preserve selected name");
    Require(RenderFrontendActionResult(focused_list) ==
                "  bank\n> email",
            "focused list should render selection marker");

    const FrontendActionResult empty_focused_list = BuildFocusedListResult(
        std::vector<std::string>{},
        "",
        "mail",
        "(no matches)");
    Require(RenderFrontendActionResult(empty_focused_list) == "(no matches)",
            "empty focused list should render explicit empty state");

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
    Require(ClassifyFrontendError("vault is locked").kind ==
                FrontendErrorKind::kLocked,
            "locked errors should be classified");
    Require(ClassifyFrontendError("no entry selected").kind ==
                FrontendErrorKind::kSelection,
            "selection errors should be classified");
    Require(ClassifyFrontendError("entry overwrite cancelled").kind ==
                FrontendErrorKind::kConfirmationRejected,
            "confirmation errors should be classified");
    Require(ClassifyFrontendError("vault is already unlocked").kind ==
                FrontendErrorKind::kConflict,
            "state conflicts should be classified");
    Require(ClassifyFrontendError("input cancelled").kind ==
                FrontendErrorKind::kInputCancelled,
            "input cancellation should be classified");
    Require(ClassifyFrontendError(
                "invalid ZKVAULT_SHELL_IDLE_TIMEOUT_SECONDS").kind ==
                FrontendErrorKind::kValidation,
            "idle-timeout configuration errors should be classified");
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
        TestExplicitStateMachineTransitions();
        TestFrontendStateMachine();
        TestOutputFormatting();
        TestActionResultsAndRendering();
        TestErrorClassification();
        return 0;
    } catch (const std::exception& ex) {
        return (std::fprintf(stderr, "frontend contract test failed: %s\n", ex.what()), 1);
    }
}
