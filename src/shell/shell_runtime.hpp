#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "app/frontend_contract.hpp"
#include "app/vault_session.hpp"
#include "crypto/secure_memory.hpp"

struct ShellBrowseState {
    bool active = false;
    std::string filter_term;
    std::vector<std::string> visible_entry_names;
    std::size_t selected_index = 0;
};

inline void Cleanse(ShellBrowseState& state) {
    Cleanse(state.filter_term);
    Cleanse(state.visible_entry_names);
}

struct ShellViewContext {
    std::string entry_name;
};

inline void Cleanse(ShellViewContext& context) {
    Cleanse(context.entry_name);
}

struct ShellBrowseSnapshot {
    bool active = false;
    std::string filter_term;
    std::string selected_name;
    std::vector<std::string> entry_names;
    std::string empty_message;
};

inline void Cleanse(ShellBrowseSnapshot& snapshot) {
    Cleanse(snapshot.filter_term);
    Cleanse(snapshot.selected_name);
    Cleanse(snapshot.entry_names);
    Cleanse(snapshot.empty_message);
}

struct ShellRuntimeState {
    FrontendStateMachine state_machine;
    std::optional<VaultSession> session;
    ShellBrowseState browse_state;
    ShellViewContext view_context;
};

inline void Cleanse(ShellRuntimeState& runtime) {
    Cleanse(runtime.browse_state);
    Cleanse(runtime.view_context);
}

struct OpenShellRuntimeResult {
    ShellRuntimeState runtime;
    std::optional<FrontendActionResult> startup_result;
};

inline void Cleanse(OpenShellRuntimeResult& result) {
    Cleanse(result.runtime);
    if (result.startup_result.has_value()) {
        Cleanse(*result.startup_result);
    }
}

std::optional<std::chrono::milliseconds> ReadShellIdleTimeout();

OpenShellRuntimeResult OpenOrInitializeShellRuntime();

FrontendActionResult ExecuteShellCommand(
    ShellRuntimeState& runtime,
    const FrontendCommand& command);

FrontendActionResult ExecutePreparedShellCommand(
    ShellRuntimeState& runtime,
    const FrontendCommand& command);

FrontendActionResult ShowCurrentShellBrowseView(
    ShellRuntimeState& runtime);

FrontendActionResult HandleShellIdleTimeout(ShellRuntimeState& runtime);

std::optional<FrontendActionResult> RecoverShellViewAfterFailure(
    ShellRuntimeState& runtime);

ShellBrowseSnapshot SnapshotShellBrowseState(const ShellRuntimeState& runtime);

bool ShellSessionUnlocked(const ShellRuntimeState& runtime) noexcept;
