#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace FHJDiag {
    // Execute only from the worker thread, never from DllMain.
    void Start(HMODULE selfModule);
    void Log(const wchar_t* message);
    void Stage(const char* stage, const wchar_t* explanation);
    void SetStage(const char* stage);
    bool SafeMode();
    void DrawOverlay(const char* renderer, float fps);
    bool OverlayVisible();
    void ToggleOverlay();
    void ToggleNativeConsole();
    bool NativeConsoleVisible();
    void PollGameLog();
}
