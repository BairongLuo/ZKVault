#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <zkvault-binary>" >&2
    exit 1
fi

BIN="$1"
TMPDIR="$(mktemp -d /tmp/zkvault-shell-ctest-XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

assert_contains() {
    local haystack="$1"
    local needle="$2"

    if [[ "$haystack" != *"$needle"* ]]; then
        echo "expected output to contain: $needle" >&2
        echo "actual output:" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

assert_occurrences() {
    local haystack="$1"
    local needle="$2"
    local expected="$3"
    local actual

    actual="$(printf '%s' "$haystack" | grep -F -o "$needle" | wc -l | tr -d '[:space:]')"
    if [[ "$actual" != "$expected" ]]; then
        echo "expected $expected occurrences of: $needle" >&2
        echo "actual occurrences: $actual" >&2
        echo "actual output:" >&2
        printf '%s\n' "$haystack" >&2
        exit 1
    fi
}

run_ok() {
    local input="$1"
    shift

    local output
    output="$(cd "$TMPDIR" && printf '%s' "$input" | "$BIN" "$@" 2>&1)"
    printf '%s' "$output"
}

run_fail() {
    local input="$1"
    shift

    local output
    set +e
    output="$(cd "$TMPDIR" && printf '%s' "$input" | "$BIN" "$@" 2>&1)"
    local status=$?
    set -e

    if [[ $status -eq 0 ]]; then
        echo "expected command to fail: $*" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi

    printf '%s' "$output"
}

SHELL_INIT_OUTPUT="$(run_ok $'y\ntest-master-password\ntest-master-password\nlist\nquit\n' shell)"
assert_contains "$SHELL_INIT_OUTPUT" "initialized .zkv_master"
assert_contains "$SHELL_INIT_OUTPUT" "shell ready; type help for commands"
assert_contains "$SHELL_INIT_OUTPUT" "(empty)"

SHELL_WORKFLOW_OUTPUT="$(run_ok $'test-master-password\nadd email\nentry-password\ninitial note\nlist\nshow email\nupdate email\nemail\nnew-entry-password\nupdated note\nshow email\nchange-master-password\nCHANGE\nnew-master-password\nnew-master-password\ndelete email\nemail\nlist\nquit\n' shell)"
assert_contains "$SHELL_WORKFLOW_OUTPUT" "saved to data/email.zkv"
assert_contains "$SHELL_WORKFLOW_OUTPUT" '"password": "entry-password"'
assert_contains "$SHELL_WORKFLOW_OUTPUT" "updated data/email.zkv"
assert_contains "$SHELL_WORKFLOW_OUTPUT" '"password": "new-entry-password"'
assert_contains "$SHELL_WORKFLOW_OUTPUT" "updated .zkv_master"
assert_contains "$SHELL_WORKFLOW_OUTPUT" "deleted data/email.zkv"
assert_contains "$SHELL_WORKFLOW_OUTPUT" "(empty)"

OLD_PASSWORD_OUTPUT="$(run_fail $'test-master-password\n' shell)"
assert_contains "$OLD_PASSWORD_OUTPUT" "error: AES-256-GCM decryption failed"

NEW_PASSWORD_OUTPUT="$(run_ok $'new-master-password\n\n   \nhelp\nquit\n' shell)"
assert_contains "$NEW_PASSWORD_OUTPUT" "shell ready; type help for commands"
assert_contains "$NEW_PASSWORD_OUTPUT" "Commands:"
assert_contains "$NEW_PASSWORD_OUTPUT" "find <term>"
assert_contains "$NEW_PASSWORD_OUTPUT" "lock"
assert_contains "$NEW_PASSWORD_OUTPUT" "unlock"

SHELL_CANCELLED_INPUT_OUTPUT="$(run_ok $'new-master-password\nadd email\n' shell)"
assert_contains "$SHELL_CANCELLED_INPUT_OUTPUT" "error: input cancelled"
[[ ! -f "$TMPDIR/data/email.zkv" ]]

SHELL_RECOVERY_OUTPUT="$(run_ok $'new-master-password\nadd email\nrecovery-password\nrecovery note\nupdate email\nwrong-name\nshow email\nquit\n' shell)"
assert_contains "$SHELL_RECOVERY_OUTPUT" "saved to data/email.zkv"
assert_contains "$SHELL_RECOVERY_OUTPUT" "error: entry overwrite cancelled"
assert_contains "$SHELL_RECOVERY_OUTPUT" '"password": "recovery-password"'

SHELL_CONFIRM_CANCEL_OUTPUT="$(run_ok $'new-master-password\nupdate email\n' shell)"
assert_contains "$SHELL_CONFIRM_CANCEL_OUTPUT" "error: input cancelled"

SHELL_REPEAT_CONFIRMATION_OUTPUT="$(run_ok $'new-master-password\nupdate email\nwrong-name\nupdate email\nstill-wrong\nshow email\nquit\n' shell)"
assert_occurrences "$SHELL_REPEAT_CONFIRMATION_OUTPUT" "error: entry overwrite cancelled" 2
assert_contains "$SHELL_REPEAT_CONFIRMATION_OUTPUT" '"password": "recovery-password"'

SHELL_FIND_OUTPUT="$(run_ok $'new-master-password\nfind MAIL\nfind bank\nquit\n' shell)"
assert_contains "$SHELL_FIND_OUTPUT" "email"
assert_contains "$SHELL_FIND_OUTPUT" "(no matches)"

SHELL_LOCK_UNLOCK_OUTPUT="$(run_ok $'new-master-password\nlock\nshow email\nunlock\nnew-master-password\nshow email\nquit\n' shell)"
assert_contains "$SHELL_LOCK_UNLOCK_OUTPUT" "vault locked"
assert_contains "$SHELL_LOCK_UNLOCK_OUTPUT" "error: vault is locked"
assert_contains "$SHELL_LOCK_UNLOCK_OUTPUT" "vault unlocked"
assert_contains "$SHELL_LOCK_UNLOCK_OUTPUT" '"password": "recovery-password"'

SHELL_LOCK_STATE_CONFLICT_OUTPUT="$(run_ok $'new-master-password\nlock\nlock\nunlock\nnew-master-password\nunlock\nquit\n' shell)"
assert_contains "$SHELL_LOCK_STATE_CONFLICT_OUTPUT" "vault locked"
assert_contains "$SHELL_LOCK_STATE_CONFLICT_OUTPUT" "error: vault is already locked"
assert_contains "$SHELL_LOCK_STATE_CONFLICT_OUTPUT" "vault unlocked"
assert_contains "$SHELL_LOCK_STATE_CONFLICT_OUTPUT" "error: vault is already unlocked"
