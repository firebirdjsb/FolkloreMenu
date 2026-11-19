cmake_minimum_required(VERSION 3.25)
project(FolkloreHunterJournalASI LANGUAGES C CXX)
if(NOT WIN32 OR CMAKE_SIZEOF_VOID_P LESS 8)
    message(FATAL_ERROR "This mod targets Windows x64.")
endif()
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG v1.3.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui minhook)
add_library(imgui_journal STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui_journal PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_journal PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(imgui_journal PUBLIC d3d11 dxgi dwmapi imm32)
add_library(FolkloreHunterJournal SHARED src/JournalASI.cpp src/JournalUI.cpp src/JournalDiagnostics.cpp src/HunterNative.cpp src/HunterScares.cpp)
target_compile_features(FolkloreHunterJournal PRIVATE cxx_std_20)
target_include_directories(FolkloreHunterJournal PRIVATE src)
target_compile_definitions(FolkloreHunterJournal PRIVATE NOMINMAX)
target_link_libraries(FolkloreHunterJournal PRIVATE imgui_journal minhook d3d11 d3d12 dxgi user32 dwmapi imm32 winmm)
if(MSVC)
    target_compile_options(FolkloreHunterJournal PRIVATE /W4 /EHsc /utf-8 /Zi)
    target_link_options(FolkloreHunterJournal PRIVATE /DEBUG:FULL /MAP)
endif()
set_target_properties(FolkloreHunterJournal PROPERTIES PREFIX "" SUFFIX ".asi")

# A separate distributable ASI. Shares the verified native reflection bridge
# for now, but has its own restricted game-mode interface and rule compilation.
# The two render-hook ASIs must not be active in the same game process until
# a shared overlay dispatch/hook owner has been implemented.
add_library(FolkloreCryptidHunt SHARED
    src/JournalASI.cpp
    src/CryptidHuntUI.cpp
    src/JournalDiagnostics.cpp
    src/HunterNative.cpp
    src/HunterScares.cpp)
target_compile_features(FolkloreCryptidHunt PRIVATE cxx_std_20)
target_include_directories(FolkloreCryptidHunt PRIVATE src)
target_compile_definitions(FolkloreCryptidHunt PRIVATE NOMINMAX FCH_STANDALONE=1)
target_link_libraries(FolkloreCryptidHunt PRIVATE imgui_journal minhook d3d11 d3d12 dxgi user32 dwmapi imm32 winmm)
if(MSVC)
    target_compile_options(FolkloreCryptidHunt PRIVATE /W4 /EHsc /utf-8 /Zi)
    target_link_options(FolkloreCryptidHunt PRIVATE /DEBUG:FULL /MAP)
endif()
set_target_properties(FolkloreCryptidHunt PROPERTIES PREFIX "" SUFFIX ".asi")
