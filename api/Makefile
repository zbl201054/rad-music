# Rad music API - 便捷构建（Windows / MSYS2-MinGW）
#
#   mingw32-make            编译核心库与测试
#   mingw32-make test       编译并运行全部测试
#   mingw32-make test FILTER=storage   只跑名字含 storage 的用例
#   mingw32-make clean
#
# 规则使用 cmd 语法（if not exist / rmdir），因此在 Windows 上不依赖 sh；
# 其他平台或想用 Posix shell 时请用 CMake：
#   cmake -S . -B build-cmake && cmake --build build-cmake

ifeq ($(OS),Windows_NT)
SHELL := cmd
endif

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
INCLUDES  = -Iinclude -Itests
BUILD     = build

# HTTP 传输层在 Windows 上需要 Winsock2；下载走 WinHTTP；shell32 供 ShellExecuteW（打开所在文件夹）
ifeq ($(OS),Windows_NT)
LDLIBS := -lws2_32 -lwinhttp -lshell32
else
LDLIBS :=
endif

CORE_SRC  := $(wildcard src/*.cpp)
CORE_OBJ  := $(patsubst src/%.cpp,$(BUILD)/%.o,$(CORE_SRC))

TEST_SRC  := $(wildcard tests/test_*.cpp)
TEST_OBJ  := $(patsubst tests/%.cpp,$(BUILD)/tests_%.o,$(TEST_SRC))

# 头文件依赖（-MMD -MP）：改了 .hpp 也会自动重编依赖它的 .o。
# 缺少这一步会导致「类布局不一致」的 .o 混链，表现为随机的堆损坏/段错误。
DEPS      := $(CORE_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

TEST_BIN  = $(BUILD)/radmusic_tests.exe
SERVER_BIN = $(BUILD)/radmusic_server.exe

.PHONY: all test clean dirs server

all: $(TEST_BIN) $(SERVER_BIN)

dirs:
	@if not exist "$(BUILD)" mkdir "$(BUILD)"

$(BUILD)/%.o: src/%.cpp | dirs
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD)/tests_%.o: tests/%.cpp | dirs
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(TEST_BIN): $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $(CORE_OBJ) $(TEST_OBJ) -o $@ $(LDLIBS)

test: $(TEST_BIN)
	@$(TEST_BIN) $(FILTER)

# 真实 HTTP 服务端：./build/radmusic_server.exe --root radmusic-data --port 8765 --demo
$(SERVER_BIN): tools/radmusic_server.cpp $(CORE_OBJ) | dirs
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(CORE_OBJ) -o $@ $(LDLIBS)

server: $(SERVER_BIN)

clean:
	@if exist "$(BUILD)" rmdir /s /q "$(BUILD)"

-include $(DEPS)
