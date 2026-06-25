# meshcom-extradio-bridge — CMake build (M11b).
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(meshcom_extradio_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

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
  src/xr/xr_session.cpp
  src/xr/xr_connection.cpp
  src/xr/xr_server.cpp)
target_include_directories(bridge_core PUBLIC src)
target_link_libraries(bridge_core PUBLIC external_radio_protocol OpenSSL::Crypto)

add_executable(meshcom-extradio-bridge src/main.cpp)
target_link_libraries(meshcom-extradio-bridge PRIVATE bridge_core)

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
