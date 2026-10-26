cmake_minimum_required(VERSION 3.21)
project(ClaudeUI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

include(FetchContent)

find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# --- Dear ImGui (docking branch, pinned tag) --------------------------------
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9-docking
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

set(CLAUDEUI_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)

# --- Core: PTY/VT terminal, session discovery, settings (no GUI) -----------
add_library(claudeui_core STATIC
  src/term/terminal.cpp
  src/net/route.cpp
  src/net/egress.cpp
  src/sessions/session_scanner.cpp
  src/sessions/live_sessions.cpp
  src/usage/usage.cpp
  src/config/app_settings.cpp
  src/config/font_catalog.cpp
  src/config/json_file.cpp
  src/config/model_access.cpp
  src/config/model_catalog.cpp
  src/config/settings_schema.cpp
  src/config/settings_store.cpp
)
target_include_directories(claudeui_core PUBLIC src)
target_link_libraries(claudeui_core PUBLIC nlohmann_json::nlohmann_json CURL::libcurl Threads::Threads)
target_compile_options(claudeui_core PRIVATE ${CLAUDEUI_WARNINGS})

# --- ClaudeUI ---------------------------------------------------------------
add_executable(claudeui
  src/app/main.cpp
  src/renderer/vulkan_context.cpp
  src/theme/theme.cpp
  src/ui/util.cpp
  src/ui/terminal_panel.cpp
  src/ui/session_tabs.cpp
  src/ui/session_list_panel.cpp
  src/ui/settings_panel.cpp
  src/ui/new_session_dialog.cpp
  src/ui/model_picker.cpp
  src/ui/fonts.cpp
  src/ui/usage_panel.cpp
  src/ui/layout_panel.cpp
  src/ui/icons.cpp
)
target_link_libraries(claudeui PRIVATE claudeui_core imgui)
target_compile_options(claudeui PRIVATE ${CLAUDEUI_WARNINGS})
target_compile_definitions(claudeui PRIVATE
  $<$<CONFIG:Debug>:CLAUDEUI_DEBUG>
  # Fallback when the binary runs from elsewhere than build/ (fonts, icons).
  CLAUDEUI_SOURCE_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
)

# --- Headless probes (see tools/*.cpp) ---------------------------------------
add_executable(term_probe tools/term_probe.cpp)
target_link_libraries(term_probe PRIVATE claudeui_core)

add_executable(config_probe tools/config_probe.cpp)
target_link_libraries(config_probe PRIVATE claudeui_core)

add_executable(net_probe tools/net_probe.cpp)
target_link_libraries(net_probe PRIVATE claudeui_core)

add_executable(usage_probe tools/usage_probe.cpp)
target_link_libraries(usage_probe PRIVATE claudeui_core)
