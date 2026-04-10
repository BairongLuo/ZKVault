cmake_minimum_required(VERSION 3.16)
project(ZKVault LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(OpenSSL REQUIRED)

add_library(zkvault_core
    src/app/frontend_contract.cpp
    src/app/vault_app.cpp
    src/app/vault_codec.cpp
    src/app/vault_session.cpp
    src/crypto/aead.cpp
    src/crypto/hex.cpp
    src/crypto/kdf.cpp
    src/crypto/random.cpp
    src/storage/master_key_storage.cpp
    src/storage/json_storage.cpp
    src/storage/atomic_file.cpp
)

target_include_directories(zkvault_core PUBLIC
    src
    external
)

target_link_libraries(zkvault_core PUBLIC OpenSSL::Crypto)

add_executable(zkvault
    src/main.cpp
    src/shell/interactive_shell.cpp
    src/terminal/display.cpp
    src/terminal/prompt.cpp
)

target_link_libraries(zkvault PRIVATE zkvault_core)

if(BUILD_TESTING)
    add_executable(zkvault_app_contract
        tests/app_contract_test.cpp
    )

    add_executable(zkvault_prompt_backspace
        tests/prompt_backspace_test.cpp
        src/terminal/prompt.cpp
    )

    add_executable(zkvault_terminal_display
        tests/terminal_display_test.cpp
        src/terminal/display.cpp
    )

    add_executable(zkvault_shell_idle_lock
        tests/shell_idle_lock_test.cpp
    )

    add_executable(zkvault_frontend_contract
        tests/frontend_contract_test.cpp
    )

    target_link_libraries(zkvault_app_contract PRIVATE zkvault_core)
    target_link_libraries(zkvault_frontend_contract PRIVATE zkvault_core)
    target_link_libraries(zkvault_prompt_backspace PRIVATE zkvault_core util)
    target_link_libraries(zkvault_shell_idle_lock PRIVATE util)
    target_link_libraries(zkvault_terminal_display PRIVATE util)
    target_include_directories(zkvault_terminal_display PRIVATE
        src
    )

    add_test(
        NAME zkvault_cli_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke_test.sh $<TARGET_FILE:zkvault>
    )
    add_test(
        NAME zkvault_cli_error_paths
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_error_paths_test.sh $<TARGET_FILE:zkvault>
    )
    add_test(
        NAME zkvault_shell_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_smoke_test.sh $<TARGET_FILE:zkvault>
    )
    add_test(
        NAME zkvault_app_contract
        COMMAND $<TARGET_FILE:zkvault_app_contract>
    )
    add_test(
        NAME zkvault_prompt_backspace
        COMMAND $<TARGET_FILE:zkvault_prompt_backspace>
    )
    add_test(
        NAME zkvault_terminal_display
        COMMAND $<TARGET_FILE:zkvault_terminal_display>
    )
    add_test(
        NAME zkvault_shell_idle_lock
        COMMAND $<TARGET_FILE:zkvault_shell_idle_lock> $<TARGET_FILE:zkvault>
    )
    add_test(
        NAME zkvault_frontend_contract
        COMMAND $<TARGET_FILE:zkvault_frontend_contract>
    )
endif()
