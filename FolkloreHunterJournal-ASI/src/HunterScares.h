#pragma once
#include <imgui.h>

namespace HunterScares {
    // Cinematic scare visuals are LOCAL to this client. The game itself is never modified.
    void DrawControls();
    void DrawFrame(const ImVec2& screen);
    bool IsScareActive();
    bool ConsumeHideJournal();
}
