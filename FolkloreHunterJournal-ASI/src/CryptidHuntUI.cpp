// Cryptid Hunt — separate possession/versus-mode UI.
// Host-only role-draft foundation: remote client role handoff is NOT yet implemented.
// Live possession uses only the host's native creature-control bridge.
#include "JournalUI.h"
#include "HunterNative.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace FHJ {
void DrawCryptidHuntLoadBanner() {
    // Always rendered, including the untouched game's MAIN MENU.
    // This is a native ImGui overlay indicator, not an edit to the
    // cooked menu UMG Blueprint or persistent SaveGame data.
    const auto status=HunterNative::GetStatus();
    const auto viewport=ImGui::GetIO().DisplaySize;
    if(viewport.x<510 || viewport.y<250)return;
    const bool selected=HunterNative::CryptidHuntOptedIn();
    const ImVec2 tl{viewport.x-380.f,16.f};
    const ImVec2 br{viewport.x-16.f,88.f};
    auto draw=ImGui::GetForegroundDrawList();
    draw->AddRectFilled(tl,br,IM_COL32(11,15,19,225),8.f);
    draw->AddRect(tl,br,selected?
        IM_COL32(166,62,53,245):IM_COL32(114,129,142,215),8.f,0,1.5f);
    draw->AddText({tl.x+14.f,tl.y+9.f},
        IM_COL32(231,218,191,255),"CRYPTID HUNT  |  MOD LOADED");
    draw->AddText({tl.x+14.f,tl.y+29.f},
        selected?IM_COL32(208,98,87,255):IM_COL32(169,180,190,255),
        selected?"CUSTOM GAME MODE SELECTED":"NORMAL GAME / MOD INACTIVE");
    const auto creature=status.isGameLobby?status.mapCryptid:
        status.mapCryptid;
    std::string label="F8: SELECT GAME MODE";
    if(creature>0)
        label+="  |  MAP: "+std::string(
            HunterNative::CryptidHuntSpeciesName(creature));
    draw->AddText({tl.x+14.f,tl.y+49.f},
        IM_COL32(212,199,176,255),label.c_str());
}
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
    ImGui::SeparatorText("MAIN MENU / CUSTOM GAME MODE");
    bool custom=HunterNative::CryptidHuntOptedIn();
    if(ImGui::RadioButton("NORMAL GAME (NO CRYPTID HUNT RULES)",!custom))
        HunterNative::SetCryptidHuntOptedIn(false);
    if(ImGui::RadioButton("CUSTOM GAME MODE: CRYPTID HUNT",custom))
        HunterNative::SetCryptidHuntOptedIn(true);
    custom=HunterNative::CryptidHuntOptedIn();
    ImGui::TextWrapped(custom?
        "MODE SELECTED: start/join a normal Folklore Hunter lobby. The host chooses the map using the existing lobby. The chosen map determines the cryptid.":
        "NORMAL GAME: this ASI is loaded but its creature rules and possession are disabled. Enable Custom Cryptid Hunt above before starting a lobby.");
    ImGui::TextDisabled("Mode choice is in memory only; resets OFF at every game launch.");
    ImGui::TextDisabled("This ASI never changes selected contracts or calls SaveGame APIs.");
    ImGui::TextDisabled("The base game may still write its normal save/progression data.");
    ImGui::SeparatorText("STOCK LOBBY / MAP-LOCKED CREATURE");
    ImGui::Text("Host: %s | State: %s",status.host?"YES":"CLIENT / UNKNOWN",
        status.isGameLobby?"IN NORMAL LOBBY":
        status.activeHuntName.empty()?"MAIN MENU / LOADING":"ACTIVE HUNT");
    ImGui::Text("Lobby map: %s",status.selectedLobbyMap.empty()?
        "(choose in normal host lobby)":status.selectedLobbyMap.c_str());
    ImGui::Text("Running hunt: %s",status.activeHuntName.empty()?
        "(not started)":status.activeHuntName.c_str());
    ImGui::TextColored(ImVec4(.90f,.60f,.45f,1.f),
        "MAP ASSIGNS: %s",HunterNative::CryptidHuntSpeciesName(status.mapCryptid));
    ImGui::TextDisabled("Night of the Wendigo -> Wendigo | Bloodfang -> Vampire | Operation Blackwing -> Mothman.");
    ImGui::TextDisabled("No map override, save edit, or cross-map creature swapping.");
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
        if(!status.host || !custom || status.mapCryptid==0)ImGui::BeginDisabled();
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
        if(!status.host || !custom || status.mapCryptid==0)ImGui::EndDisabled();
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
    std::string creatureLabel="Select a map-native cryptid in this hunt";
    bool playable=false;
    auto speciesOf=[](std::string name) {
        std::transform(name.begin(),name.end(),name.begin(),
            [](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(name.find("wendigo")!=std::string::npos)return 3;
        if(name.find("vampire")!=std::string::npos)return 2;
        if(name.find("mothman")!=std::string::npos ||
           name.find("flyingpawn")!=std::string::npos)return 1;
        return 0;
    };
    for(const auto& creature:creatures)
        if(creature.objectIndex==selectedCreature) {
            creatureLabel=creature.displayName;
            playable=creature.isPawn &&
                speciesOf(creature.displayName)==status.mapCryptid;
        }
    if(ImGui::BeginCombo("Live creature to control",creatureLabel.c_str())) {
        for(const auto& creature:creatures) {
            if(!creature.isPawn ||
               speciesOf(creature.displayName)!=status.mapCryptid)continue;
            ImGui::PushID(creature.objectIndex);
            if(ImGui::Selectable(creature.displayName.c_str(),
                                 selectedCreature==creature.objectIndex))
                selectedCreature=creature.objectIndex;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if(!status.host || !custom || status.isGameLobby ||
       status.mapCryptid==0 || !playable || controlling)ImGui::BeginDisabled();
    if(ImGui::Button("HOST TEST: CONTROL SELECTED LIVE CRYPTID",ImVec2(-1,38)))
        HunterNative::RequestPossessCryptid(selectedCreature);
    if(!status.host || !custom || status.isGameLobby ||
       status.mapCryptid==0 || !playable || controlling)ImGui::EndDisabled();
    ImGui::SameLine();
    if(!controlling)ImGui::BeginDisabled();
    if(ImGui::Button("RETURN HUMAN (F11)"))
        HunterNative::RequestRestoreHuman();
    if(!controlling)ImGui::EndDisabled();

    if(ImGui::CollapsingHeader("WENDIGO // PRIVATE WATCH CAMERAS",
                             ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Place a private, native CameraActor watchpoint; no survivor camera feeds.");
        ImGui::Text("Wendigo watchpoints: %d / 4 | Remaining today: %d / 2",
            HunterNative::WendigoCameraCount(),HunterNative::WendigoCameraDayRemaining());
        ImGui::TextDisabled("Camera quota resets on native night -> day transition.");
        ImGui::TextDisabled("Wendigo hides during placement for ~1.6 s, then reappears.");
        const bool wendigo=custom && controlling &&
            HunterNative::MonsterSpecies()==3 && status.mapCryptid==3;
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
        ImGui::TextWrapped("Wendigo: cannot attack or use offensive abilities in daylight. Mild daytime visibility penalty, restored at night; may place up to two private watch cameras per day.");
        ImGui::TextWrapped("Vampire: nearly blinded in sunlight; LMB melee and Q scream disabled in daytime, restored at night.");
        ImGui::TextDisabled("Day/night uses the game state replicated IsNight (or host Director fallback).");
        ImGui::TextDisabled("If phase is unknown, abilities are not falsely classified as day/night.");
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
    ImGui::TextDisabled("Separate ASI: may coexist with the Journal through one shared DXGI hook-owner bridge.");
    ImGui::End();
}
}
