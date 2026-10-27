cmake_minimum_required(VERSION 3.25)
project(makine-live LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Ninja, Makefiles) build unoptimised without a type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link the C++ runtime into the DLL. The plugin host looks for DLLs only in the
# plugin's folder, the launcher's folder and System32, so a runtime DLL we do not
# ship would be the launcher's copy, in whatever version it has.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# nlohmann/json (MIT), header-only, pinned by hash.
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM)
FetchContent_MakeAvailable(json)

add_library(makine-live SHARED
    src/plugin.cpp
    src/worker.cpp
    src/capture.cpp
    src/ocr_rapid.cpp
    src/translator.cpp
    src/winutil.cpp
)
target_include_directories(makine-live PRIVATE include src)
# NOMINMAX: windows.h would otherwise turn std::min/std::max into macros.
target_compile_definitions(makine-live PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(makine-live PRIVATE nlohmann_json::nlohmann_json d3d11 dxgi winhttp gdi32 user32)
set_target_properties(makine-live PROPERTIES PREFIX "")   # makine-live.dll, not libmakine-live.dll
if(MINGW)
    target_compile_options(makine-live PRIVATE -Wall -Wextra)
    target_link_options(makine-live PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
    target_compile_options(makine-live PRIVATE /W4 /utf-8)   # Turkish text in string literals
endif()

# build/package/ is the plugin as Makine installs it:
#   manifest.json, bin/makine-live.dll (the manifest's "entry"), bin/RapidOcrOnnx.dll,
#   models/, licenses/
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" MANIFEST_JSON)
string(JSON PLUGIN_ENTRY GET "${MANIFEST_JSON}" entry)
configure_file(manifest.json "${PACKAGE_DIR}/manifest.json" COPYONLY)
configure_file(LICENSE "${PACKAGE_DIR}/licenses/Makine-Live-LICENSE.txt" COPYONLY)
configure_file(licenses/NOTICE.txt "${PACKAGE_DIR}/licenses/NOTICE.txt" COPYONLY)
configure_file("${json_SOURCE_DIR}/LICENSE.MIT" "${PACKAGE_DIR}/licenses/nlohmann-json-LICENSE.txt" COPYONLY)
add_custom_command(TARGET makine-live POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:makine-live>" "${PACKAGE_DIR}/${PLUGIN_ENTRY}"
    VERBATIM)

# The OCR runtime and models: python tools/fetch_ocr.py downloads them here.
set(LIVE_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ocr" CACHE PATH "Output of tools/fetch_ocr.py")
set(STAGE_OCR_VARS "-DSRC=${LIVE_OCR_DIR}" "-DDST=${PACKAGE_DIR}")
set(STAGE_OCR_SCRIPT -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_ocr.cmake")   # -D must come before -P
add_custom_target(stage-ocr ALL COMMAND "${CMAKE_COMMAND}" ${STAGE_OCR_VARS} ${STAGE_OCR_SCRIPT} VERBATIM)

# cmake --build build --target pack  ->  build/<id>-<version>.mkplugin
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(pack
        COMMAND "${CMAKE_COMMAND}" ${STAGE_OCR_VARS} -DREQUIRE=ON ${STAGE_OCR_SCRIPT}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/plugin_registry.py"
                pack "${PACKAGE_DIR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        VERBATIM)
    add_dependencies(pack makine-live)
else()
    message(STATUS "Python 3 not found: the pack target is not available")
endif()
