cmake_minimum_required(VERSION 3.24)
project(AgentChats LANGUAGES CXX)
set(AGENTCHATS_VERSION "0.1.0")

# Produce a genuinely portable Windows executable: the MSVC runtime is linked
# statically, while DirectX/WinHTTP/Crypto remain normal Windows system DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "AgentChats targets Windows (Win32 + DirectX 11).")
endif()

# --- Third-party libraries, fetched at configure time (pinned versions) ------
include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(imgui nlohmann_json)

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

# --- Application ---------------------------------------------------------------
add_executable(AgentChats WIN32
  resources/AgentChats.rc
  src/main.cpp
  src/App.cpp
  src/Store.cpp
  src/Settings.cpp
  src/OptionsModules.cpp
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
  src/CodeWorker.cpp
  src/GitHub.cpp)

target_link_libraries(AgentChats PRIVATE
  imgui
  nlohmann_json::nlohmann_json
  d3d11 dxgi d3dcompiler ole32 shell32 uuid winhttp crypt32 bcrypt ws2_32)

target_compile_definitions(AgentChats PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(AgentChats PRIVATE AGENTCHATS_VERSION="${AGENTCHATS_VERSION}")
target_compile_options(AgentChats PRIVATE /utf-8 /W4 /permissive-)

# --- Safety tests (path confinement, zones, mentions, tool parsing) ---------------------------
add_executable(AgentChatsTests
  tests/SafetyTests.cpp
  src/Tools.cpp
  src/Conductor.cpp
  src/Backends.cpp
  src/Process.cpp
  src/Http.cpp
  src/Platform.cpp
  src/Settings.cpp
  src/Secrets.cpp
  src/CloudSync.cpp
  src/ModelCatalog.cpp
  src/Store.cpp
  src/GitHub.cpp)
target_link_libraries(AgentChatsTests PRIVATE nlohmann_json::nlohmann_json ole32 shell32 uuid winhttp crypt32 ws2_32)
target_compile_definitions(AgentChatsTests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AgentChatsTests PRIVATE /utf-8 /W4 /permissive-)

enable_testing()
add_test(NAME AgentChatsSafety COMMAND AgentChatsTests)

install(TARGETS AgentChats RUNTIME DESTINATION .)
install(FILES README.md LICENSE CAHIER-DES-CHARGES.md PORTABILITY.md DESTINATION documentation)

set(CPACK_PACKAGE_NAME "Agents-Chat")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
