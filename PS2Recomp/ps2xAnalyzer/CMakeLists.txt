cmake_minimum_required(VERSION 3.20)
project(PS2Analyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(PS2ANALYZER_LIB_SOURCES
    src/analysis_passes.cpp
    src/elf_analysis_context.cpp
    src/elf_analyzer.cpp
    src/function_classifier.cpp
    src/sce_symbol_scanner.cpp
    src/toml_generator.cpp
)

add_library(ps2_analyzer_lib STATIC ${PS2ANALYZER_LIB_SOURCES})

target_include_directories(ps2_analyzer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
)

# Optional private dependency, never included in a source/binary release.
set(PS2X_SCE_DATABASE_HEADER "" CACHE FILEPATH "Private hash-verified SDK database header")
if(PS2X_SCE_DATABASE_HEADER)
    if(NOT IS_ABSOLUTE "${PS2X_SCE_DATABASE_HEADER}" OR NOT EXISTS "${PS2X_SCE_DATABASE_HEADER}")
        message(FATAL_ERROR "PS2X_SCE_DATABASE_HEADER must be an existing absolute path")
    endif()
    file(SHA256 "${PS2X_SCE_DATABASE_HEADER}" sce_database_sha256)
    if(NOT sce_database_sha256 STREQUAL "0fdd1168a815c8af2529cae9076ae813a1304c5c3a41e48d88d38e21e7b5833d")
        message(FATAL_ERROR "SCE database hash mismatch")
    endif()
    target_compile_definitions(ps2_analyzer_lib PRIVATE
        PS2RECOMP_SCE_DATABASE_HEADER="${PS2X_SCE_DATABASE_HEADER}")
endif()

target_link_libraries(ps2_analyzer_lib PUBLIC
    ps2_recomp_lib
    nlohmann_json::nlohmann_json
)

add_executable(ps2_analyzer
    src/analyzer_main.cpp
)

target_link_libraries(ps2_analyzer PRIVATE
    ps2_analyzer_lib
)

install(TARGETS ps2_analyzer ps2_analyzer_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

include("${CMAKE_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_analyzer_lib)
    EnableFastReleaseMode(ps2_analyzer)
endif()
