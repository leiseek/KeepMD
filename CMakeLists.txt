cmake_minimum_required(VERSION 3.25)
project(KeepMD VERSION 0.5.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- $<$<CONFIG:Release>:/O2>)
  # Some localized MSVC-only installations emit UTF-8 /showIncludes text while
  # CMake's compiler detection decodes it as the Windows ANSI code page.
  # Correct that detected mojibake so Ninja tracks headers, not just .cpp files.
  if(CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "娉ㄦ剰")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:")
  endif()
endif()
add_library(md4c STATIC third_party/md4c/md4c.c third_party/md4c/entity.c)
target_include_directories(md4c PUBLIC third_party/md4c)
add_library(flowchart STATIC third_party/tinta/mermaid.cpp)
target_include_directories(flowchart PUBLIC third_party/tinta)

# Product-neutral foundation: text and file I/O, native UI kit, Mermaid parsing and the visual Markdown
# editor. It never refers to KeepMD or KeepPrompt.
add_library(keep_shared STATIC src/shared/text.cpp src/shared/file_io.cpp src/shared/diagram.cpp
  src/shared/ui.cpp src/shared/icons.cpp src/shared/scrollbar.cpp src/shared/caption.cpp
  src/shared/visual_markdown.cpp src/shared/visual_editor.cpp src/shared/visual_diagram.cpp)
target_include_directories(keep_shared PUBLIC include/shared PRIVATE src/shared)
target_link_libraries(keep_shared PUBLIC md4c flowchart comctl32 dwmapi imm32 ole32 uxtheme)

# KeepMD and KeepPrompt are independent programs: neither links, includes, launches nor messages the other.
add_library(keepmd_core STATIC src/keepmd/document.cpp src/keepmd/html.cpp src/keepmd/html_image.cpp
  src/keepmd/settings.cpp src/keepmd/shell_integration.cpp src/keepmd/paths.cpp
  src/keepmd/remote_image.cpp src/keepmd/image_decode.cpp src/keepmd/image_cache.cpp)
target_include_directories(keepmd_core PUBLIC include/keepmd)
target_link_libraries(keepmd_core PUBLIC keep_shared advapi32 winhttp windowscodecs d2d1 bcrypt shell32)
add_executable(keepmd WIN32 src/keepmd/main.cpp src/keepmd/view.cpp src/keepmd/editor.cpp
  resources/keepmd/app.rc resources/keepmd/app.manifest)
target_link_libraries(keepmd PRIVATE keepmd_core d2d1 dwrite windowscodecs comdlg32 shell32 shlwapi psapi)

add_library(keepprompt_core STATIC src/keepprompt/prompt_model.cpp)
target_include_directories(keepprompt_core PUBLIC include/keepprompt)
target_link_libraries(keepprompt_core PUBLIC keep_shared advapi32)
add_executable(KeepPrompt WIN32 src/keepprompt/prompt_main.cpp src/keepprompt/prompt_window.cpp
  resources/keepprompt/app.rc resources/keepprompt/app.manifest)
target_link_libraries(KeepPrompt PRIVATE keepprompt_core comdlg32 shell32 shlwapi)

include(CTest)
if(BUILD_TESTING)
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE keepmd_core keepprompt_core)
  add_test(NAME core COMMAND core_tests)
endif()
install(TARGETS keepmd KeepPrompt RUNTIME DESTINATION .)
install(FILES third_party/md4c/LICENSE.md DESTINATION licenses RENAME MD4C.txt)
install(FILES third_party/tinta/LICENSE DESTINATION licenses RENAME Tinta.txt)
