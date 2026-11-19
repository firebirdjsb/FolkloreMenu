// Cryptid Hunt — separate possession/versus-mode UI.
// Host-only role-draft foundation: remote client role handoff is NOT yet implemented.
// Live possession uses only the host's native creature-control bridge.
#include "JournalUI.h"
#include "HunterNative.h"
#include <imgui.h>
#include <algorithm>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace FHJ {
void DrawJournal() {
    ImGui::SetNextWindowSize(ImVec2(750,690),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.98f);
    if(!ImGui::Begin("FOLKLORE HUNTER // CRYPTID HUNT",nullptr,
                     ImGuiWindowFlags_NoCollapse)) {
        ImGui::End(); return;
    }
    const auto status=HunterNative::GetStatus();
    const bool controlling=HunterNative::MonsterModeActive();
    const int phase=HunterNative::GetCryptidHuntNightState();
    ImGui::TextColored(ImVec4(.76f,.65f,.42f,1.f),
        "CRYPTID HUNT // SURVIVORS AGAINST THE NIGHT");
    ImGui::Text("Host: %s | Hunt: %s",status.host?"YES":"CLIENT / UNKNOWN",
                status.activeHuntName.empty()?"not in expedition":status.activeHuntName.c_str());
    ImGui::TextColored(phase==1?ImVec4(.54f,.77f,1.f,1.f):
        phase==0?ImVec4(1.f,.76f,.37f,1.f):ImVec4(.67f,.67f,.67f,1.f),
        "DIRECTOR TIME: %s",phase==1?"NIGHT":phase==0?"DAY":"UNKNOWN (NO HOST DIRECTOR)");
    ImGui::Separator();

    // Actual player inventory from Unreal. These checkboxes are host-local
    // draft eligibility, not networked client consent/ready messages yet.
    static std::unordered_set<int> eligible;
    static int selectedPlayer=-1;
    static std::string selectedPlayerName;
    static bool selectedIsLocal=false;
    const auto players=HunterNative::GetPlayers();
    if(ImGui::CollapsingHeader("ROUND LOBBY / RANDOM CRYPTID ROLE",
                             ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Stage 1: select connected survivors eligible for the host role draw.");
        ImGui::TextDisabled("Host roster selection is local only; networked join/ready and");
        ImGui::TextDisabled("remote owner possession need a client companion + server RPC.");
        for(const auto& player:players) {
            if(!player.alive)continue;
            bool ready=eligible.count(player.objectIndex)!=0;
            ImGui::PushID(player.objectIndex);
            std::string name=player.displayName;
            if(player.local)name+=" [LOCAL]";
            if(ImGui::Checkbox(name.c_str(),&ready)) {
                if(ready)eligible.insert(player.objectIndex);
                else eligible.erase(player.objectIndex);
                if(!ready && selectedPlayer==player.objectIndex)
                    selectedPlayer=-1;
            }
            ImGui::PopID();
        }
        if(!status.host)ImGui::BeginDisabled();
        if(ImGui::Button("HOST: DRAW RANDOM CRYPTID PLAYER",ImVec2(-1,36))) {
            std::vector<HunterNative::PlayerInfo> pool;
            for(const auto& p:players)
                if(p.alive && eligible.count(p.objectIndex))pool.push_back(p);
            if(!pool.empty()) {
                std::random_device entropy;
                std::mt19937_64 generator(
                    (static_cast<uint64_t>(entropy())<<32)|entropy());
                std::uniform_int_distribution<size_t> draw(0,pool.size()-1);
                const auto& winner=pool[draw(generator)];
                selectedPlayer=winner.objectIndex;
                selectedPlayerName=winner.displayName;
                selectedIsLocal=winner.local;
            }
        }
        if(!status.host)ImGui::EndDisabled();
        if(selectedPlayer>=0) {
            ImGui::TextColored(ImVec4(.90f,.67f,.30f,1.f),
                "SELECTED CRYPTID PLAYER: %s",selectedPlayerName.c_str());
            ImGui::TextDisabled(selectedIsLocal?
                "Host-local winner: manual native control can be activated below.":
                "Remote winner: selected only. Role assignment/possess RPC is not yet implemented.");
        } else ImGui::TextDisabled("No random role drawn in this session.");
        if(ImGui::Button("RESET ROUND DRAFT")) {
            selectedPlayer=-1;
            selectedPlayerName.clear();
            selectedIsLocal=false;
            eligible.clear();
        }
    }
    ImGui::Separator();

    const auto creatures=HunterNative::GetCreatures();
    static int selectedCreature=-1;
    std::string creatureLabel="Select a live cryptid in this hunt";
    bool playable=false;
    for(const auto& creature:creatures)
        if(creature.objectIndex==selectedCreature) {
            creatureLabel=creature.displayName;
            playable=creature.isPawn;
        }
    if(ImGui::BeginCombo("Live creature to control",creatureLabel.c_str())) {
        for(const auto& creature:creatures) {
            ImGui::PushID(creature.objectIndex);
            if(ImGui::Selectable(creature.displayName.c_str(),
                                 selectedCreature==creature.objectIndex))
                selectedCreature=creature.objectIndex;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if(!status.host || !playable || controlling)ImGui::BeginDisabled();
    if(ImGui::Button("HOST TEST: CONTROL SELECTED LIVE CRYPTID",ImVec2(-1,38)))
        HunterNative::RequestPossessCryptid(selectedCreature);
    if(!status.host || !playable || controlling)ImGui::EndDisabled();
    ImGui::SameLine();
    if(!controlling)ImGui::BeginDisabled();
    if(ImGui::Button("RETURN HUMAN (F11)"))
        HunterNative::RequestRestoreHuman();
    if(!controlling)ImGui::EndDisabled();

    if(ImGui::CollapsingHeader("WENDIGO // PRIVATE WATCH CAMERAS",
                             ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Place a private, native CameraActor watchpoint; no survivor camera feeds.");
        ImGui::Text("Wendigo watchpoints: %d / 4",HunterNative::WendigoCameraCount());
        const bool wendigo=controlling && HunterNative::MonsterSpecies()==3;
        if(!wendigo)ImGui::BeginDisabled();
        if(ImGui::Button("PLACE WATCH CAMERA (C)"))
            HunterNative::RequestWendigoDeployCamera();
        ImGui::SameLine();
        if(ImGui::Button("NEXT / CHASE VIEW (V)"))
            HunterNative::RequestWendigoCycleCamera();
        ImGui::SameLine();
        if(ImGui::Button("RETURN CHASE VIEW"))
            HunterNative::RequestWendigoExitCamera();
        if(!wendigo)ImGui::EndDisabled();
        ImGui::TextDisabled("Camera nodes are private viewpoints, not visible placed camera props yet.");
    }

    if(ImGui::CollapsingHeader("DAYLIGHT / SPECIES RULES",
                             ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Wendigo: mildly impaired vision during Director daytime; full sight at night.");
        ImGui::TextWrapped("Vampire: nearly blinded in sunlight; both LMB melee and Q scream are disabled during day, restored at night.");
        ImGui::TextDisabled("Rules require the host's live Director.IsNight field.");
        ImGui::TextDisabled("Missing Director state = no invented day/night state.");
    }
    if(ImGui::CollapsingHeader("POSSESSION CAMERA / LOCAL VISION")) {
        auto cam=HunterNative::GetCryptidCameraSettings();
        bool changed=false;
        changed|=ImGui::SliderFloat("Yaw sensitivity",&cam.yawSensitivity,.015f,.6f);
        changed|=ImGui::SliderFloat("Pitch sensitivity",&cam.pitchSensitivity,.015f,.6f);
        changed|=ImGui::SliderFloat("Follow rate",&cam.followRate,8.f,72.f);
        changed|=ImGui::Checkbox("Invert pitch",&cam.invertPitch);
        changed|=ImGui::Checkbox("Suppress blur and depth-of-field locally",
                                  &cam.clearBlurDuringPossession);
        if(changed)HunterNative::SetCryptidCameraSettings(cam);
        bool smoke=HunterNative::MothmanLocalSmokeReduction();
        if(ImGui::Checkbox("Reduce my Mothman smoke only",&smoke))
            HunterNative::SetMothmanLocalSmokeReduction(smoke);
    }
    ImGui::Separator();
    ImGui::TextDisabled("F8 game-mode panel | F11 emergency return | F9 diagnostics");
    ImGui::TextDisabled("Local test build: do not install with FolkloreHunterJournal.asi");
    ImGui::End();
}
}
