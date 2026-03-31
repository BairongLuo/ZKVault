#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <zkvault-binary>" >&2
    exit 1
fi

BIN="$1"
TMPDIR="$(mktemp -d /tmp/zkvault-ctest-XXXXXX)"
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

assert_not_contains() {
    local haystack="$1"
    local needle="$2"

    if [[ "$haystack" == *"$needle"* ]]; then
        echo "expected output to not contain: $needle" >&2
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

INIT_OUTPUT="$(run_ok $'test-master-password\ntest-master-password\n' init)"
assert_contains "$INIT_OUTPUT" "initialized .zkv_master"
[[ -f "$TMPDIR/.zkv_master" ]]

ADD_OUTPUT="$(run_ok $'test-master-password\nentry-password\ninitial note\n' add email)"
assert_contains "$ADD_OUTPUT" "saved to data/email.zkv"
[[ -f "$TMPDIR/data/email.zkv" ]]

LIST_OUTPUT="$(run_ok '' list)"
assert_contains "$LIST_OUTPUT" "email"

GET_OUTPUT="$(run_ok $'test-master-password\n' get email)"
assert_contains "$GET_OUTPUT" '"name": "email"'
assert_contains "$GET_OUTPUT" '"password": "entry-password"'
assert_contains "$GET_OUTPUT" '"note": "initial note"'

UPDATE_OUTPUT="$(run_ok $'test-master-password\nnew-entry-password\nupdated note\n' update email)"
assert_contains "$UPDATE_OUTPUT" "updated data/email.zkv"

UPDATED_GET_OUTPUT="$(run_ok $'test-master-password\n' get email)"
assert_contains "$UPDATED_GET_OUTPUT" '"password": "new-entry-password"'
assert_contains "$UPDATED_GET_OUTPUT" '"note": "updated note"'

CHANGE_MASTER_OUTPUT="$(run_ok $'test-master-password\nnew-master-password\nnew-master-password\n' change-master-password)"
assert_contains "$CHANGE_MASTER_OUTPUT" "updated .zkv_master"

OLD_PASSWORD_GET_OUTPUT="$(run_fail $'test-master-password\n' get email)"
assert_contains "$OLD_PASSWORD_GET_OUTPUT" "error: AES-256-GCM decryption failed"

NEW_PASSWORD_GET_OUTPUT="$(run_ok $'new-master-password\n' get email)"
assert_contains "$NEW_PASSWORD_GET_OUTPUT" '"password": "new-entry-password"'

INVALID_NAME_OUTPUT="$(run_fail $'new-master-password\nignored\nignored\n' add bad/name)"
assert_contains "$INVALID_NAME_OUTPUT" "entry name may only contain letters, digits, '.', '-' and '_'"
[[ ! -e "$TMPDIR/data/bad/name.zkv" ]]

DELETE_OUTPUT="$(run_ok '' delete email)"
assert_contains "$DELETE_OUTPUT" "deleted data/email.zkv"
[[ ! -f "$TMPDIR/data/email.zkv" ]]

LIST_AFTER_DELETE_OUTPUT="$(run_ok '' list)"
assert_not_contains "$LIST_AFTER_DELETE_OUTPUT" "email"
