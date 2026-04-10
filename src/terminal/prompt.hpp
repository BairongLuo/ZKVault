#pragma once

#include <chrono>
#include <string>

enum class PromptReadStatus {
    kRead,
    kEof,
    kTimedOut
};

std::string ReadSecret(const std::string& prompt);

bool TryReadLine(const std::string& prompt, std::string& value);

PromptReadStatus TryReadLineWithTimeout(
    const std::string& prompt,
    std::string& value,
    std::chrono::milliseconds timeout);

std::string ReadLine(const std::string& prompt);

void RequireExactConfirmation(
    const std::string& prompt,
    const std::string& expected,
    const std::string& mismatch_error);

std::string ReadConfirmedSecret(
    const std::string& prompt,
    const std::string& confirm_prompt,
    const std::string& mismatch_error);
