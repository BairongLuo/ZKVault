cmake_minimum_required(VERSION 3.16)
project(ZKVault LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(OpenSSL REQUIRED)

add_executable(zkvault
    src/main.cpp
    src/crypto/aead.cpp
    src/crypto/hex.cpp
    src/crypto/kdf.cpp
    src/crypto/random.cpp
    src/storage/master_key_storage.cpp
    src/storage/json_storage.cpp
)

target_include_directories(zkvault PRIVATE
    src
    external
)

target_link_libraries(zkvault PRIVATE OpenSSL::Crypto)

if(BUILD_TESTING)
    add_test(
        NAME zkvault_cli_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke_test.sh $<TARGET_FILE:zkvault>
    )
endif()
