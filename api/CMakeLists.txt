cmake_minimum_required(VERSION 3.16)
project(radmusic_api LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RADMUSIC_BUILD_TESTS "Build radmusic test suite" ON)
option(RADMUSIC_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
# 版本号（检查更新用）：发布流水线用 git tag 覆盖，例如 -DRADMUSIC_VERSION=0.12.0
set(RADMUSIC_VERSION "0.11.0-dev" CACHE STRING "Version string embedded in the server binary")

# ---------------------------------------------------------------- 核心库
# 零第三方依赖：自带 JSON / SHA-256 / MD5 / HMAC / PBKDF2 / AES-256-GCM
file(GLOB RADMUSIC_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(radmusic_core STATIC ${RADMUSIC_CORE_SOURCES})
target_include_directories(radmusic_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(radmusic_core PUBLIC RADMUSIC_VERSION="${RADMUSIC_VERSION}")

if(MSVC)
  target_compile_options(radmusic_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(radmusic_core PRIVATE -Wall -Wextra)
  if(RADMUSIC_WARNINGS_AS_ERRORS)
    target_compile_options(radmusic_core PRIVATE -Werror)
  endif()
endif()

# HTTP 传输层在 Windows 上需要 Winsock2（ws2_32）；下载走 WinHTTP；
# shell32 供 ShellExecuteW（「打开所在文件夹」）
if(WIN32)
  target_link_libraries(radmusic_core PUBLIC ws2_32 winhttp shell32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(radmusic_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- HTTP 服务端
file(GLOB RADMUSIC_TOOL_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.cpp")

if(RADMUSIC_TOOL_SOURCES)
  add_executable(radmusic_server ${RADMUSIC_TOOL_SOURCES})
  target_include_directories(radmusic_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(radmusic_server PRIVATE radmusic_core)
endif()

# ---------------------------------------------------------------- 测试
if(RADMUSIC_BUILD_TESTS)
  file(GLOB RADMUSIC_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")

  add_executable(radmusic_tests ${RADMUSIC_TEST_SOURCES})
  target_include_directories(radmusic_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(radmusic_tests PRIVATE radmusic_core)

  if(NOT MSVC)
    target_compile_options(radmusic_tests PRIVATE -Wall -Wextra)
  endif()

  enable_testing()
  add_test(NAME radmusic_tests COMMAND radmusic_tests)

  # 过滤运行：cmake --build . --target run_tests
  add_custom_target(run_tests
    COMMAND $<TARGET_FILE:radmusic_tests>
    DEPENDS radmusic_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "运行 radmusic 测试套件")
endif()
