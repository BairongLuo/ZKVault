#pragma once

#include <ostream>
#include <string_view>

#include "app/frontend_contract.hpp"
#include "crypto/secure_memory.hpp"

inline void PrintFrontendResult(
    std::ostream& stream,
    FrontendActionResult result) {
    auto result_guard = MakeScopedCleanse(result);
    std::string output = RenderFrontendActionResult(result);
    auto output_guard = MakeScopedCleanse(output);
    if (!output.empty()) {
        stream << output << '\n';
    }
}

inline void PrintFrontendError(
    std::ostream& stream,
    FrontendError error) {
    auto error_guard = MakeScopedCleanse(error);
    std::string output = RenderFrontendError(error);
    auto output_guard = MakeScopedCleanse(output);
    stream << output << '\n';
}

inline void PrintFrontendError(
    std::ostream& stream,
    std::string_view message) {
    PrintFrontendError(stream, ClassifyFrontendError(message));
}
