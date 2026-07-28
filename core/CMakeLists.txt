cmake_minimum_required(VERSION 3.20)
project(goldmine_core
    VERSION     1.0.0
    LANGUAGES   C CXX
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
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
)

# ===========================================================================
# Header-only library target
# ===========================================================================
add_library(goldmine_core_headers INTERFACE)
target_include_directories(goldmine_core_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../interfaces/cpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite>
)
target_compile_options(goldmine_core_headers INTERFACE ${GOLDMINE_COMPILE_FLAGS})

# ===========================================================================
# SQLite3 Static Library
# ===========================================================================
add_library(sqlite3 STATIC ../third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ../third_party/sqlite)

# ===========================================================================
# Main executable — the expert engine
# ===========================================================================
add_executable(expert_engine src/expert_engine.cpp)
target_link_libraries(expert_engine PRIVATE goldmine_core_headers sqlite3 pthread rt dl)
target_compile_options(expert_engine PRIVATE ${GOLDMINE_COMPILE_FLAGS})

# Output into core/build
set_target_properties(expert_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")

# Print build info
message(STATUS "Goldmine Core v${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD}")
message(STATUS "Flags: ${GOLDMINE_COMPILE_FLAGS}")
