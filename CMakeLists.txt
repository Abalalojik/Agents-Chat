cmake_minimum_required(VERSION 3.24)
project(AgentChats LANGUAGES CXX)
set(AGENTCHATS_VERSION "0.1.0")

# Produce a genuinely portable Windows executable: the MSVC runtime is linked
# statically, while DirectX/WinHTTP/Crypto remain normal Windows system DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "AgentChats targets Windows and Linux.")
endif()

# --- Third-party libraries, fetched at configure time (pinned versions) ------
include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(imgui nlohmann_json)

# --- Logic shared by the application and the tests (no user interface) ------------------------
set(AGENTCHATS_CORE_SOURCES
  src/Store.cpp
  src/Settings.cpp
  src/Updater.cpp
  src/ModelCatalog.cpp
  src/Platform.cpp
  src/Process.cpp
  src/Http.cpp
  src/Secrets.cpp
  src/CloudSync.cpp
  src/Backends.cpp
  src/Tools.cpp
  src/Conductor.cpp
  src/ShortContext.cpp
  src/Console.cpp
  src/CodeWorker.cpp
  src/GitHub.cpp
  src/ReleaseSignature.cpp)

if(WIN32)
  set(AGENTCHATS_PLATFORM_LIBS ole32 shell32 uuid winhttp crypt32 bcrypt ws2_32)
  set(AGENTCHATS_DEFINITIONS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  set(AGENTCHATS_WARNINGS /utf-8 /W4 /permissive-)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
  find_package(CURL REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  set(AGENTCHATS_PLATFORM_LIBS PkgConfig::LIBSECRET CURL::libcurl OpenSSL::Crypto Threads::Threads)
  set(AGENTCHATS_DEFINITIONS "")
  set(AGENTCHATS_WARNINGS -Wall -Wextra)
endif()

# --- Application ---------------------------------------------------------------
if(WIN32)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
  target_compile_options(imgui PRIVATE /utf-8)

  add_executable(AgentChats WIN32
    resources/AgentChats.rc
    src/main.cpp
    src/App.cpp
    src/OptionsModules.cpp
    ${AGENTCHATS_CORE_SOURCES})
  target_link_libraries(AgentChats PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    d3d11 dxgi d3dcompiler ${AGENTCHATS_PLATFORM_LIBS})
  target_compile_definitions(AgentChats PRIVATE ${AGENTCHATS_DEFINITIONS} AGENTCHATS_VERSION="${AGENTCHATS_VERSION}")
  target_compile_options(AgentChats PRIVATE ${AGENTCHATS_WARNINGS})
  install(TARGETS AgentChats RUNTIME DESTINATION .)
else()
  # Linux window host: SDL2 + OpenGL 3.
  find_package(SDL2 REQUIRED)
  find_package(OpenGL REQUIRED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
  target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL ${CMAKE_DL_LIBS})

  add_executable(AgentChats
    src/main_linux.cpp
    src/App.cpp
    src/OptionsModules.cpp
    ${AGENTCHATS_CORE_SOURCES})
  target_link_libraries(AgentChats PRIVATE imgui nlohmann_json::nlohmann_json ${AGENTCHATS_PLATFORM_LIBS})
  target_compile_definitions(AgentChats PRIVATE AGENTCHATS_VERSION="${AGENTCHATS_VERSION}")
  target_compile_options(AgentChats PRIVATE ${AGENTCHATS_WARNINGS})
  install(TARGETS AgentChats RUNTIME DESTINATION bin)
endif()

# --- Safety tests (path confinement, zones, mentions, tool parsing) ---------------------------
add_executable(AgentChatsTests tests/SafetyTests.cpp ${AGENTCHATS_CORE_SOURCES})
target_link_libraries(AgentChatsTests PRIVATE nlohmann_json::nlohmann_json ${AGENTCHATS_PLATFORM_LIBS})
target_compile_definitions(AgentChatsTests PRIVATE ${AGENTCHATS_DEFINITIONS} AGENTCHATS_VERSION="${AGENTCHATS_VERSION}")
target_compile_options(AgentChatsTests PRIVATE ${AGENTCHATS_WARNINGS})

enable_testing()
add_test(NAME AgentChatsSafety COMMAND AgentChatsTests)

# --- Release signing tool (maintainer only, never shipped; Windows, the key is DPAPI-protected) ---
if(WIN32)
  add_executable(AgentChatsRelease tools/ReleaseTool.cpp src/ReleaseSignature.cpp src/Secrets.cpp src/Platform.cpp)
  target_include_directories(AgentChatsRelease PRIVATE src)
  target_link_libraries(AgentChatsRelease PRIVATE bcrypt crypt32 ole32 shell32 uuid)
  target_compile_definitions(AgentChatsRelease PRIVATE ${AGENTCHATS_DEFINITIONS})
  target_compile_options(AgentChatsRelease PRIVATE ${AGENTCHATS_WARNINGS})
endif()

install(FILES README.md LICENSE CAHIER-DES-CHARGES.md PORTABILITY.md DESTINATION documentation)

set(CPACK_PACKAGE_NAME "Agents-Chat")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
