cmake_minimum_required(VERSION 3.20)
project(goldmine
    VERSION     1.0.0
    LANGUAGES   C CXX
    DESCRIPTION "XAU/USD Algorithmic Expert System — Ultra-Low-Latency Trading Engine"
)

# ===========================================================================
# C++20 — strictly enforced, no fallback
# ===========================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===========================================================================
# Production compiler flags
# ===========================================================================
set(GOLDMINE_COMPILE_FLAGS
    -O3
    -march=native
    -mavx2
    -mfma
    -flto
    -ffast-math
    -funroll-loops
    -fno-rtti                   # no RTTI — we use CRTP, not virtual dispatch
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter       # lambdas in rule_registry may not use all params
)

# ===========================================================================
# Header-only library target (for tests and external consumers)
# ===========================================================================
add_library(goldmine_headers INTERFACE)
target_include_directories(goldmine_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(goldmine_headers INTERFACE ${GOLDMINE_COMPILE_FLAGS})

target_include_directories(goldmine_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite>
)

# ===========================================================================
# SQLite3 Static Library
# ===========================================================================
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)

# ===========================================================================
# Main executable — the expert engine
# ===========================================================================
add_executable(expert_engine src/expert_engine.cpp)
target_link_libraries(expert_engine PRIVATE goldmine_headers sqlite3)
target_link_libraries(expert_engine PRIVATE pthread rt dl)
target_compile_options(expert_engine PRIVATE ${GOLDMINE_COMPILE_FLAGS})

# ===========================================================================
# Tests & Backtest Harness
# ===========================================================================
enable_testing()

add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE goldmine_headers pthread rt)
target_compile_options(smoke_test PRIVATE ${GOLDMINE_COMPILE_FLAGS})
add_test(NAME smoke_test COMMAND smoke_test)

add_executable(backtest_harness tests/backtest_harness.cpp)
target_link_libraries(backtest_harness PRIVATE goldmine_headers pthread rt)
target_compile_options(backtest_harness PRIVATE ${GOLDMINE_COMPILE_FLAGS})

# ===========================================================================
# Custom targets
# ===========================================================================
add_custom_target(run
    COMMAND $<TARGET_FILE:expert_engine>
    DEPENDS expert_engine
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running Goldmine Expert Engine"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Print build info
message(STATUS "Goldmine v${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} | Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Flags: ${GOLDMINE_COMPILE_FLAGS}")
