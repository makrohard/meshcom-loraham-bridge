# meshcom-loraham-bridge — CMake build.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(meshcom_loraham_bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wformat-security)

# --- hardening -------------------------------------------------------------
# Cheap, portable exploit-mitigation defaults. Kept simple: stack canaries,
# hardened libstdc++ bounds assertions, position-independent executables, full
# RELRO + immediate binding, and a non-executable stack. _FORTIFY_SOURCE needs
# an optimised build, so it is applied only to optimised configs (and never with
# sanitizers, which intercept the same libc entry points).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-fstack-protector-strong -D_GLIBCXX_ASSERTIONS)
add_link_options(-pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)

# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer for tests/CI:
#   cmake -B build -DMEBRIDGE_SANITIZE=ON && cmake --build build && ctest
option(MEBRIDGE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(MEBRIDGE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
else()
  add_compile_options($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=2>)
endif()

find_package(OpenSSL REQUIRED)  # libcrypto: HMAC-SHA256, RAND_bytes, CRYPTO_memcmp

# Vendored, hardware-/transport-independent XR protocol codec (author-relicensed MIT).
add_library(external_radio_protocol STATIC
  third_party/external_radio_protocol/external_radio_protocol.cpp)
target_include_directories(external_radio_protocol PUBLIC
  third_party/external_radio_protocol)

# Bridge core (everything except main()), so tests can link it directly.
add_library(bridge_core STATIC
  src/auth/hmac_auth.cpp
  src/backend/fake_backend.cpp
  src/backend/loraham/loraham_config.cpp
  src/backend/loraham/loraham_framing.cpp
  src/backend/loraham/loraham_posix_transport.cpp
  src/backend/loraham/loraham_backend.cpp
  src/xr/xr_session.cpp
  src/xr/xr_connection.cpp
  src/xr/xr_server.cpp)
target_include_directories(bridge_core PUBLIC src)
target_link_libraries(bridge_core PUBLIC external_radio_protocol OpenSSL::Crypto)

add_executable(meshcom-loraham-bridge src/main.cpp)
target_link_libraries(meshcom-loraham-bridge PRIVATE bridge_core)
target_compile_definitions(meshcom-loraham-bridge PRIVATE
  MEBRIDGE_VERSION="${PROJECT_VERSION}")

# --- tests -----------------------------------------------------------------
enable_testing()

function(add_bridge_test name)
  add_executable(${name} tests/${name}.cpp)
  target_include_directories(${name} PRIVATE tests)
  target_link_libraries(${name} PRIVATE bridge_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_bridge_test(test_protocol_server)
add_bridge_test(test_xr_session)
add_bridge_test(test_fake_backend)
add_bridge_test(test_loraham_config)
add_bridge_test(test_loraham_framing)
add_bridge_test(test_loraham_backend)
add_bridge_test(test_loraham_posix_transport)
