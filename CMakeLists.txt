cmake_minimum_required(VERSION 3.21)
project(Video-PLAYER LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributables
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code IntelliSense see the real flags

add_executable(Video-PLAYER WIN32
    src/main.cpp
    src/media/player.cpp
    src/media/subtitles.cpp
    src/platform/file_dialog.cpp
    src/platform/registration.cpp
    src/ui/overlay.cpp
    src/ui/popup_menu.cpp
    src/ui/window.cpp
    res/app.rc
    res/app.manifest)
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico)
target_include_directories(Video-PLAYER PRIVATE src)
target_compile_definitions(Video-PLAYER PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(Video-PLAYER PRIVATE /W4 /permissive- /utf-8 /sdl /guard:cf)
# List the libraries explicitly instead of CMake's defaults (which import ole32.dll). onecore_apiset comes
# last, so it only supplies the COM functions, which then bind straight to combase.dll.
set(CMAKE_CXX_STANDARD_LIBRARIES "")
target_link_libraries(Video-PLAYER PRIVATE delayimp kernel32 user32 gdi32 imm32 dwmapi advapi32 shell32 comdlg32
                      oleaut32 uuid mfplat mfreadwrite mfuuid avrt d3d11 dxgi dcomp d2d1 dwrite dxguid onecore_apiset)
# The file picker and dropped files load their DLLs on first use, and text drawing once the window lays out.
# The registry is only touched by --register / --unregister.
target_link_options(Video-PLAYER PRIVATE /DELAYLOAD:comdlg32.dll /DELAYLOAD:shell32.dll /DELAYLOAD:d2d1.dll
                    /DELAYLOAD:dwrite.dll /DELAYLOAD:advapi32.dll)
# Control-flow guard, shadow stack, and DLL imports from System32 only (no planted DLLs next to the exe).
target_link_options(Video-PLAYER PRIVATE /GUARD:CF /CETCOMPAT /DEPENDENTLOADFLAG:0x800)
# Spaces are not allowed in target names, only in the file name.
set_target_properties(Video-PLAYER PROPERTIES OUTPUT_NAME "Video-PLAYER")
