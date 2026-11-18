// Folklore Hunter — Native x64 ASI Hunter's Journal (visuals only)
#include "JournalUI.h"
#include "JournalDiagnostics.h"
#include "HunterNative.h"
#include "HunterItemCatalog.h"
#include "HunterScares.h"
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace FHJ
{

    namespace Palette
    {
        constexpr ImU32 ink = IM_COL32(10, 14, 14, 255);
        constexpr ImU32 bark = IM_COL32(25, 28, 25, 255);
        constexpr ImU32 mist = IM_COL32(121, 131, 115, 255);
        constexpr ImU32 parchment = IM_COL32(227, 214, 184, 255);
        constexpr ImU32 gold = IM_COL32(192, 151, 94, 255);
        constexpr ImU32 moss = IM_COL32(112, 132, 101, 255);
        constexpr ImU32 blood = IM_COL32(153, 60, 51, 255);
        constexpr ImU32 border = IM_COL32(76, 68, 54, 255);
        inline const ImVec4 col_gold{0.753f, 0.592f, 0.369f, 1.0f};
        inline const ImVec4 col_paper{0.890f, 0.839f, 0.722f, 1.0f};
        inline const ImVec4 col_muted{0.545f, 0.581f, 0.527f, 1.0f};
    }

    enum class Page : int
    {
        Journal, Survivor, Equipment, Cryptids, Host, Physics, Lobby, Mischief, Wardrobe, Settings
    };

    constexpr const char* PageNames[] = {
        "FIELD JOURNAL", "SURVIVOR", "SUPPLIES", "CRYPTIDS", "HUNTING PARTY", "PHYSICS", "LOBBY", "MISCHIEF", "VANITY", "OPTIONS"
    };

    struct Preferences
    {
        float scale = 1.0f;
        bool compact = false;
        bool show_sdk_hints = true;
    };

    std::filesystem::path ConfigPath()
    {
        wchar_t buf[32768]{};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 32768);
        if (!n || n >= 32768) return {};
        return std::filesystem::path(buf) / L"FolkloreHunterJournal" / L"settings.ini";
    }

    void SavePrefs(const Preferences& p)
    {
        try {
            const auto path = ConfigPath();
            if (path.empty()) return;
            std::filesystem::create_directories(path.parent_path());
            const auto text = path.wstring();
            wchar_t num[64]{};
            swprintf_s(num, L"%d", static_cast<int>(p.scale * 100.0f));
            WritePrivateProfileStringW(L"Theme", L"ScalePercent", num, text.c_str());
            WritePrivateProfileStringW(L"Theme", L"Compact", p.compact ? L"1" : L"0", text.c_str());
            WritePrivateProfileStringW(L"Theme", L"ShowSdkHints", p.show_sdk_hints ? L"1" : L"0", text.c_str());
        } catch (...) { /* settings must never stop the menu from loading */ }
    }

    Preferences LoadPrefs()
    {
        Preferences p{};
        const auto path = ConfigPath();
        if (path.empty()) return p;
        const auto text = path.wstring();
        p.scale = std::clamp(GetPrivateProfileIntW(L"Theme", L"ScalePercent", 100, text.c_str()) / 100.0f, 0.8f, 1.5f);
        p.compact = GetPrivateProfileIntW(L"Theme", L"Compact", 0, text.c_str()) != 0;
        p.show_sdk_hints = GetPrivateProfileIntW(L"Theme", L"ShowSdkHints", 1, text.c_str()) != 0;
        return p;
    }

    struct StyleScope
    {
        StyleScope()
        {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.041f, 0.055f, 0.051f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(.075f, .105f, .08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(.16f, .21f, .13f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ImVec4(.06f, .08f, .06f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.078f, 0.090f, 0.078f, 0.94f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.090f, 0.110f, 0.093f, 0.99f));
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::col_paper);
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, Palette::col_muted);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.294f, 0.259f, 0.204f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.115f, 0.141f, 0.117f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.220f, 0.246f, 0.178f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.327f, 0.298f, 0.192f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.115f, 0.130f, 0.109f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.182f, 0.200f, 0.155f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.239f, 0.220f, 0.155f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.219f, 0.238f, 0.169f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.279f, 0.279f, 0.189f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_CheckMark, Palette::col_gold);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, Palette::col_gold);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f, 10.0f));
        }
        ~StyleScope()
        {
            ImGui::PopStyleVar(5);
            ImGui::PopStyleColor(19);
        }
    };

    // Subtle hand-drawn hunter mark (not a game asset; original geometry).
    void DrawWard(ImDrawList* draw, const ImVec2& c, float size)
    {
        draw->AddCircle(c, size * 0.75f, Palette::gold, 24, 1.4f);
        draw->AddCircle(c, size * 0.43f, Palette::moss, 16, 1.0f);
        draw->AddLine(ImVec2(c.x - size, c.y), ImVec2(c.x + size, c.y), Palette::gold, 1.0f);
        draw->AddLine(ImVec2(c.x, c.y - size), ImVec2(c.x, c.y + size), Palette::gold, 1.0f);
        const ImVec2 tri[3] = { {c.x, c.y-size*0.44f}, {c.x-size*0.25f, c.y+size*0.20f}, {c.x+size*0.25f, c.y+size*0.20f} };
        draw->AddPolyline(tri, 3, Palette::parchment, ImDrawFlags_Closed, 1.5f);
    }

    void BranchLine(ImDrawList* draw, ImVec2 from, ImVec2 to)
    {
        draw->AddLine(from, to, Palette::border, 1.3f);
        const ImVec2 mid{from.x+(to.x-from.x)*0.60f, from.y+(to.y-from.y)*0.60f};
        draw->AddLine(mid, ImVec2(mid.x-7,mid.y-9), Palette::border, 1.0f);
        draw->AddLine(mid, ImVec2(mid.x+9,mid.y-6), Palette::border, 1.0f);
    }

    void Heading(const char* title, const char* subtitle)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::col_gold);
        ImGui::SetWindowFontScale(1.35f);
        ImGui::TextUnformatted(title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s", subtitle);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    void InfoLine(const char* left, const char* right, bool highlight=false)
    {
        ImGui::TextDisabled("%s", left);
        ImGui::SameLine(218.0f);
        if (highlight) ImGui::TextColored(Palette::col_gold, "%s", right);
        else ImGui::TextUnformatted(right);
    }

    void Notice(const char* title, const char* detail, bool warning=false)
    {
        const auto cursor = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float width = std::max(180.0f, ImGui::GetContentRegionAvail().x);
        dl->AddRectFilled(cursor, ImVec2(cursor.x+width, cursor.y+67), Palette::bark, 3.0f);
        dl->AddRectFilled(cursor, ImVec2(cursor.x+3, cursor.y+67), warning ? Palette::blood : Palette::moss);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x+14,cursor.y+11));
        ImGui::TextColored(warning ? ImVec4(.79f,.37f,.33f,1) : Palette::col_gold, "%s", title);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x+14,cursor.y+35));
        ImGui::TextDisabled("%s", detail);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x,cursor.y+76));
    }

    class Journal final
    {
    public:
        Journal() : prefs_(LoadPrefs()) {}
        void Render()
        {
            StyleScope theme; // Must be active BEFORE Begin to style the title and whole window.
            const ImVec2 screen = ImGui::GetIO().DisplaySize;
            const float margin = 16.0f;
            const float width = std::min(1050.0f, std::max(340.0f, std::min(screen.x - 2.0f*margin, screen.x*0.66f)));
            const float height = std::min(835.0f, std::max(280.0f, std::min(screen.y - 2.0f*margin, screen.y*0.81f)));
            ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
            ImGui::SetNextWindowPos(ImVec2(std::max(margin, screen.x - width - margin), std::max(margin, (screen.y-height)*.5f)), ImGuiCond_Always);
            ImGui::Begin("THE HUNTER'S JOURNAL  //  FOLKLORE HUNTER", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            ImGui::TextColored(Palette::col_gold, "THE HUNTER'S JOURNAL");
            ImGui::SameLine();
            ImGui::TextDisabled("  /  FOLKLORE HUNTER    [F8: CLOSE]");
            ImGui::Separator();
            Draw();
            ImGui::End();
        }
    private:
        Preferences prefs_{};
        Page page_ = Page::Journal;
        int selectedHunter_ = -1;
        int DrawPlayerSelect(const char* id, bool includeLocal = true) {
            const auto roster=HunterNative::GetPlayers();
            std::string selected="Select a hunter";
            for(const auto& p:roster) {
                if(p.objectIndex==selectedHunter_)
                    selected=p.displayName+(p.local?" (you)":"");
            }
            if(ImGui::BeginCombo(id,selected.c_str())) {
                for(const auto& player:roster) {
                    if(!includeLocal && player.local)continue;
                    ImGui::PushID(player.objectIndex);
                    const std::string label=player.displayName+(player.local?" (you)":"");
                    if(ImGui::Selectable(label.c_str(),selectedHunter_==player.objectIndex))
                        selectedHunter_=player.objectIndex;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Live player actors: %d",static_cast<int>(roster.size()));
            return selectedHunter_;
        }
        char search_[96]{};

        void Navigation(float sidebarWidth)
        {
            ImGui::BeginChild("##HunterSidebar", ImVec2(sidebarWidth,0), true);
            auto* dl = ImGui::GetWindowDrawList();
            auto pos = ImGui::GetCursorScreenPos();
            DrawWard(dl, ImVec2(pos.x+27,pos.y+24), 17);
            ImGui::Dummy(ImVec2(1,48));
            ImGui::SetWindowFontScale(1.22f);
            ImGui::TextColored(Palette::col_paper, "FOLKLORE");
            ImGui::TextColored(Palette::col_gold, "HUNTER");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::TextDisabled("THE FIELD JOURNAL");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(1,7));
            for (int i = 0; i < static_cast<int>(sizeof(PageNames)/sizeof(PageNames[0])); ++i)
            {
                bool active = static_cast<int>(page_) == i;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.295f,.265f,.177f,1));
                ImGui::PushID(i);
                if (ImGui::Button(PageNames[i], ImVec2(-1,32))) page_ = static_cast<Page>(i);
                ImGui::PopID();
                if (active) ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(1,15));
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold, "HUNT  //  01");
            ImGui::TextDisabled("UE 5.5.4");
            ImGui::TextDisabled("Journal ASI v0.12");
            ImGui::EndChild();
        }

        void DrawDashboard()
        {
            Heading("THE HUNTER'S JOURNAL", "FIELD NOTES  /  FOLKLORE HUNTER  /  FIRST EDITION");
            Notice("THE JOURNAL HAS AWAKENED", "Native x64 ASI module active since game startup.");
            Notice("ENGINE ACCESS IS READ-ONLY", "Gameplay commands are not wired until runtime validated.");
            ImGui::Spacing();
            ImGui::TextColored(Palette::col_gold, "CURRENT EXPEDITION");
            InfoLine("Engine revision", "5.5.4 (SDK snapshot)");
            InfoLine("Generated SDK", "FolkloreHunter 5.5.4");
            InfoLine("ASI module", "LOADED", true);
            InfoLine("Overlay renderer", "AUTO DX11 / DX12", true);
            InfoLine("Native GameMode class", "SDK ONLY - NOT HOOKED");
            ImGui::Spacing();
            ImGui::TextDisabled("THE HUNT BEGINS IN THE DARK.  |  This is a native C++ framework, not a trainer.");
        }

        void DrawSurvivor()
        {
            Heading("SURVIVOR", "LIVE PLAYER STATUS  /  SAFE GAMEPLAY CONTROLS");
            const auto status = HunterNative::GetStatus();
            Notice(status.worldReady ? "EXPEDITION CONNECTED" : "NO ACTIVE EXPEDITION",
                   status.worldReady ? "Reading live Folklore Hunter world state." : "Enter a playable expedition to access character data.");
            if (status.localHealth >= 0) {
                ImGui::TextColored(Palette::col_gold, "HEALTH: %.0f / %.0f", status.localHealth, status.localMaxHealth);
                ImGui::ProgressBar(status.localMaxHealth > 0 ? static_cast<float>(status.localHealth/status.localMaxHealth) : 0, ImVec2(-1,10));
                ImGui::Text("Invulnerability: %s", status.invuln ? "YES" : "NO");
            } else ImGui::TextDisabled("Local player not ready (camera and character may still be loading).");
            if (!status.host) ImGui::BeginDisabled();
            if (ImGui::Button(status.localGodProtected?
                    "DISABLE GOD MODE [HOST]" : "ENABLE GOD MODE [HOST]",ImVec2(-1,38)))
                HunterNative::RequestGodMode();
            if (!status.host) ImGui::EndDisabled();
            ImGui::Text("Protection: %s  |  Protected players: %d",
                status.localGodProtected?"ACTIVE":"OFF",status.godProtectedPlayers);
            ImGui::TextDisabled("Host authoritative replicated Invuln + health-loss correction.");
            ImGui::TextDisabled("Invuln alone is not proof of immunity; verify health after damage.");
            ImGui::Separator();
            static bool noclip = false;
            if(ImGui::Checkbox("Local noclip / free-fly (experimental)", &noclip))
                HunterNative::RequestNoclip(noclip);
            ImGui::TextDisabled("Noclip: WASD moves horizontally relative to camera yaw, Space up, Ctrl down.");
            ImGui::TextDisabled("Hold Shift to boost. Uses direct actor movement; test while hosting.");
            ImGui::TextDisabled("ESP health values/bars only appear on PLAYERS and CRYPTIDS.");
            Notice("MULTIPLAYER AUTHORITY", status.host ? "Authoritative GameMode detected; host actions available." : "Client-only session: host actions are disabled.", !status.host);
        }

        void DrawEquipment()
        {
            Heading("SUPPLIES & ITEM SPAWNER", "THE QUARTERMASTER / CLICK TO SELECT / NO ITEM IDS REQUIRED");
            const auto status=HunterNative::GetStatus();
            static ImGuiTextFilter filter{};
            static int category=0;
            static int selected=0;
            static int quantity=1;
            const char* categories[]={"All", "Weapons", "Ammunition", "Equipment", "Survival", "Cosmetics"};
            ImGui::TextColored(Palette::col_gold,"%d ITEMS DISCOVERED IN THE UE 5.5.4 SDK",static_cast<int>(HunterItems::Catalog.size()));
            ImGui::TextDisabled("Choose your item and amount; the journal resolves its in-game class automatically.");
            filter.Draw("Search by name",260.0f);
            for (int i=0;i<6;++i) {
                ImGui::PushID(i);
                if (i && i%3) ImGui::SameLine();
                if (category==i) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.36f,.30f,.18f,1.f));
                if (ImGui::SmallButton(categories[i])) category=i;
                if (category==i) ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::BeginChild("##SpawnableItems",ImVec2(0,230),true)) {
                int shown=0;
                for (int i=0;i<static_cast<int>(HunterItems::Catalog.size());++i) {
                    const auto& item=HunterItems::Catalog[i];
                    if (category && strcmp(item.category,categories[category])!=0) continue;
                    if (!filter.PassFilter(item.label)) continue;
                    ImGui::PushID(i);
                    if (ImGui::Selectable(item.label,selected==i,0,ImVec2(0,29))) selected=i;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",item.category);
                    ImGui::PopID();
                    ++shown;
                }
                if (!shown) ImGui::TextDisabled("No matching items. Try All or a shorter search.");
            }
            ImGui::EndChild();
            const auto& chosen=HunterItems::Catalog[static_cast<size_t>(selected)];
            ImGui::TextColored(Palette::col_gold,"SELECTED: %s",chosen.label);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)",chosen.category);
            ImGui::SetNextItemWidth(130);
            ImGui::InputInt("Amount", &quantity);
            quantity=std::clamp(quantity,1,999);
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"RECIPIENT");
            const auto recipient=DrawPlayerSelect("Give item to player");
            const bool canGrant=status.worldReady && status.host;
            if (!canGrant) ImGui::BeginDisabled();
            if (ImGui::Button("GIVE SELECTED ITEM TO RECIPIENT",ImVec2(-1,38))) {
                if(recipient>=0) HunterNative::RequestTargetItem(recipient,chosen.className,quantity);
                else HunterNative::RequestCatalogItem(chosen.className,quantity);
            }
            if (!canGrant) ImGui::EndDisabled();
            if (!status.host)
                ImGui::TextDisabled("Start a solo or hosted expedition to enable inventory granting.");
            ImGui::Separator();
            ImGui::TextDisabled("Grant path: PlayerCharacter.GridInventoryComponent -> AskServer_AddItem.");
            ImGui::TextDisabled("The SDK confirms item classes and RPC layout; server acceptance and DLC items require in-game testing.");
        }

        void DrawCryptids()
        {
            Heading("CRYPTIDS, NPCS & WORLD OBJECTS", "REAL-TIME ESP / SPAWNED HUNTS / BECOME A CREATURE");
            const auto state=HunterNative::GetStatus();
            ImGui::Text("Cryptids: %d  Wildlife/NPCs: %d  Players: %d",state.activeCryptids,
                state.activeWildlife,state.activePlayers);
            ImGui::Text("Boxes/chests: %d  Quest: %d  Loot: %d",state.activeContainers,
                state.activeQuestObjects,state.activeLoot);
            ImGui::Text("Scanned: %d / %d   Loaded levels: %d",state.scanned,state.totalObjects,state.levelsScanned);
            ImGui::Text("Camera: %s   ESP candidates: %d",
                state.cameraReady?"READY":"NOT READY",state.espCandidates);
            if(state.levelsScanned==0)
                ImGui::TextColored(Palette::col_gold,"Active UWorld levels unavailable; ESP needs a loaded hunt.");
            HunterNative::DrawESPSettings();
            ImGui::Spacing();ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"CRYPTID SPAWNING [HOST]");
            static int wave=1,selectedCreature=-1;
            ImGui::SliderInt("Native spawner triggers",&wave,1,10);
            ImGui::Text("Queued requests: %d | Directly created: %d",
                state.pendingSpawnTriggers,state.confirmedSpawned);
            if(!state.host)ImGui::BeginDisabled();
            if(ImGui::Button("TRIGGER LOADED MOB SPAWNERS",ImVec2(-1,32)))
                HunterNative::RequestSpawnMobWave(wave,selectedCreature);
            if(!state.host)ImGui::EndDisabled();
            ImGui::TextDisabled("Verified cryptid classes use native deferred spawning;");
            ImGui::TextDisabled("other classes fall back to the map's TriggerSpawn rules.");
            ImGui::TextDisabled("Select a live cryptid below to override spawner MobClass for one trigger,");
            ImGui::TextDisabled("or leave it unselected for each map spawner's default species.");
            const auto creatures=HunterNative::GetCreatures();
            ImGui::TextColored(Palette::col_gold,"BECOME A LIVE CRYPTID [EXPERIMENTAL]");
            std::string selectedLabel="Select a live cryptid";
            bool isPawn=false;
            double cryptidHP=-1.0, cryptidMaxHP=-1.0;
            for(const auto& creature:creatures) if(creature.objectIndex==selectedCreature) {
                selectedLabel=creature.displayName+" #"+std::to_string(creature.objectIndex);
                isPawn=creature.isPawn;
                cryptidHP=creature.health;cryptidMaxHP=creature.maxHealth;
            }
            if(ImGui::BeginCombo("Cryptid",selectedLabel.c_str())) {
                for(const auto& creature:creatures) {
                    const auto name=creature.displayName+" #"+std::to_string(creature.objectIndex);
                    ImGui::PushID(creature.objectIndex);
                    if(ImGui::Selectable(name.c_str(),creature.objectIndex==selectedCreature))
                        selectedCreature=creature.objectIndex;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if(cryptidHP>=0) {
                if(cryptidMaxHP>0) {
                    ImGui::Text("CRYPTID HEALTH: %.0f / %.0f",cryptidHP,cryptidMaxHP);
                    ImGui::ProgressBar(std::clamp(
                        static_cast<float>(cryptidHP/cryptidMaxHP),0.f,1.f),
                        ImVec2(-1,10));
                } else ImGui::Text("CRYPTID HEALTH: %.0f / ?",cryptidHP);
            } else ImGui::TextDisabled("Cryptid HP: select a live cryptid to inspect");
            if(!state.host || selectedCreature<0 || !isPawn)ImGui::BeginDisabled();
            if(ImGui::Button("CONTROL CRYPTID [PUPPET MODE]",ImVec2(-1,32)))
                HunterNative::RequestPossessCryptid(selectedCreature);
            if(!state.host || selectedCreature<0 || !isPawn)ImGui::EndDisabled();
            if(!state.host)ImGui::BeginDisabled();
            if(ImGui::Button("RESTORE HUMAN CHARACTER",ImVec2(-1,30)))
                HunterNative::RequestRestoreHuman();
            if(!state.host)ImGui::EndDisabled();
            ImGui::TextDisabled("Remote cryptid control preserves the human pawn in the expedition.");
            ImGui::TextDisabled("WASD uses native AI navigation (swept fallback); Shift sprint.");
            ImGui::TextDisabled("Chase camera uses collision sweep to pull in near walls.");
            if(HunterNative::MonsterModeActive()) {
                ImGui::Separator();
                ImGui::TextColored(Palette::col_gold,"MONSTER ABILITIES");
                if(ImGui::Button("MELEE ATTACK [LMB]",ImVec2(-1,29)))
                    HunterNative::RequestMonsterAbility(0);
                const bool vampire=HunterNative::MonsterSpecies()==2;
                if(ImGui::Button(vampire?"VAMPIRE SCREAM [Q]":"CRYPTID ROAR / SCREAM [Q]",ImVec2(-1,29)))
                    HunterNative::RequestMonsterAbility(1);
                if(!vampire)ImGui::BeginDisabled();
                if(ImGui::Button("VAMPIRE INVISIBILITY [E]",ImVec2(-1,29)))
                    HunterNative::RequestMonsterAbility(2);
                if(ImGui::Button("VAMPIRE CHANGE STANCE [R]",ImVec2(-1,29)))
                    HunterNative::RequestMonsterAbility(3);
                if(ImGui::Button("VAMPIRE SCREAM AUDIO",ImVec2(-1,29)))
                    HunterNative::RequestMonsterAbility(4);
                if(!vampire)ImGui::EndDisabled();
                ImGui::TextDisabled("Abilities are selected from the cryptid's reflected UFunctions.");
                ImGui::TextDisabled("Unavailable methods and rejected signatures are logged in F9.");
            }
            ImGui::TextDisabled("F8 journal, F11 safe Return Human; hunter HUD hidden in monster mode.");
            Notice("SDK VALIDATION",
                "ESP animal/box/mission categories use class-name heuristics, not a verified complete object list. Native spawning and possession are untested in multiplayer.",true);
        }

        void DrawPhysics()
        {
            Heading("THE PHYSICS LAB","FALL PROTECTION  /  MOVEMENT  /  LOBBY GRAVITY");
            const auto state=HunterNative::GetStatus();
            static bool entireLobby=true;
            static float walk=1.f,jump=1.f,gravity=1.f;
            ImGui::TextColored(Palette::col_gold,"HOST-APPLIED MOVEMENT");
            ImGui::Checkbox("Apply to every known player (host authority)",&entireLobby);
            ImGui::TextDisabled("Move and jump fields aren't automatically replicated; clients may predict differently.");
            ImGui::Separator();
            bool noFall=state.fallProtectionEnabled;
            if(ImGui::Checkbox("NO FALL DAMAGE — HOST AND LOBBY",&noFall))
                HunterNative::RequestFallProtection(noFall);
            ImGui::Text("Protection: %s  |  Pawns with fall settings: %d",
                noFall?"ENABLED":"DISABLED",state.fallProtectedPlayers);
            ImGui::TextDisabled("Independent fall settings, temporary airborne invulnerability.");
            ImGui::TextDisabled("Replicated health restored after landing if client still reports damage.");
            ImGui::Separator();
            ImGui::SliderFloat("Walk / sprint multiplier",&walk,.25f,4.f,"%.2fx");
            if(ImGui::Button("APPLY MOVEMENT SPEED",ImVec2(-1,31)))
                HunterNative::RequestSpeedMultiplier(walk,entireLobby);
            ImGui::SliderFloat("Jump height multiplier",&jump,.25f,4.f,"%.2fx");
            if(ImGui::Button("APPLY JUMP HEIGHT",ImVec2(-1,31)))
                HunterNative::RequestJumpBoost(jump,entireLobby);
            if(ImGui::Button("RESET SPEED ONLY",ImVec2(-1,31))) {
                walk=1.f;
                HunterNative::RequestSpeedMultiplier(1.f,entireLobby);
            }
            if(ImGui::Button("RESET JUMP ONLY",ImVec2(-1,31))) {
                jump=1.f;
                HunterNative::RequestJumpBoost(1.f,entireLobby);
            }
            ImGui::TextDisabled("Speed modifies walk/sprint only; jump modifies JumpZVelocity only.");
            ImGui::TextDisabled("Gravity modifies only WorldSettings WorldGravityZ.");
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"WORLD GRAVITY  /  REPLICATED WORLDSETTINGS");
            ImGui::SliderFloat("Gravity multiplier",&gravity,0.f,3.f,"%.2fx");
            if(!state.host)ImGui::BeginDisabled();
            if(ImGui::Button("APPLY GRAVITY TO HOST WORLD",ImVec2(-1,34)))
                HunterNative::RequestGravity(gravity,true);
            if(ImGui::Button("RESTORE WORLD GRAVITY",ImVec2(-1,29))) {
                gravity=1.f;HunterNative::RequestGravity(1.f,true);
            }
            if(!state.host)ImGui::EndDisabled();
            Notice("HOSTING & REPLICATION",
                "WorldSettings.WorldGravityZ is a Net property in the SDK. Multiplayer sync and jump/fall authority still require an in-game test.",true);
        }

        void DrawHost()
        {
            Heading("HUNTING PARTY", "SERVER AUTHORITY  /  DEV COMMANDS");
            const auto status=HunterNative::GetStatus();
            Notice(status.host ? "GAME-MODE AUTHORITY DETECTED" : "NOT AN AUTHORITATIVE HOST",
                status.host ? "Native host commands can be queued on the game window thread." : "Start your own solo/private host session to access this page.",!status.host);
            static float hour=12.0f;
            static float timeScale=1.0f;
            static int xp=100, silver=100;
            if (!status.host) ImGui::BeginDisabled();
            if (ImGui::SliderFloat("Time of day",&hour,0,24,"%.1f"))
                hour=std::clamp(hour,0.f,24.f);
            if (ImGui::Button("SET TIME",ImVec2(-1,30))) HunterNative::RequestTime(hour);
            ImGui::SliderFloat("Game time scale",&timeScale,0.1f,4.f,"%.2fx");
            if (ImGui::Button("APPLY TIME SCALE",ImVec2(-1,30))) HunterNative::RequestTimeScale(timeScale);
            ImGui::InputInt("Add XP",&xp);
            xp=std::clamp(xp,1,100000);
            if (ImGui::Button("GRANT XP",ImVec2(-1,30))) HunterNative::RequestAddXP(xp);
            ImGui::InputInt("Silver",&silver);
            silver=std::clamp(silver,1,100000);
            if (ImGui::Button("GRANT SILVER",ImVec2(-1,30))) HunterNative::RequestAddSilver(silver);
            if (ImGui::Button("INFINITE DAMAGE (GAME CHEAT)",ImVec2(-1,30))) HunterNative::RequestInfiniteDamage();
            if (!status.host) ImGui::EndDisabled();
            Notice("HOST CONTROL VALIDATION", "These dispatch the reflected GameMode functions. Effects on other clients require real multiplayer tests.", true);
            ImGui::TextDisabled("Kicks/bans and player-specific RPCs are intentionally unavailable until ownership and FText contracts are verified.");
        }

        void DrawLobby()
        {
            Heading("HUNTING LOBBY", "LIVE PLAYER ROSTER / HOST ADMINISTRATION");
            const auto status=HunterNative::GetStatus();
            Notice(status.host?"HOST AUTHORITY AVAILABLE":"HOST ONLY",
                status.host?"Select a player to use verified reflected requests."
                :"Remote clients do not control other players. Start a hosted expedition.",!status.host);
            const auto id=DrawPlayerSelect("Hunter");
            const auto roster=HunterNative::GetPlayers();
            for(const auto& player:roster) if(player.objectIndex==id) {
                ImGui::TextColored(Palette::col_gold,"%s",player.displayName.c_str());
                ImGui::Text("Health: %.0f    %s    Godmode: %s",
                    player.health,player.alive?"ALIVE":"DOWN",player.invulnerable?"ON":"OFF");
            }
            const bool allowed=status.host && id>=0;
            if(!allowed)ImGui::BeginDisabled();
            static float health=100.f;
            ImGui::SliderFloat("Set health",&health,0.f,500.f,"%.0f HP");
            if(ImGui::Button("SET SELECTED HEALTH",ImVec2(-1,32)))
                HunterNative::RequestTargetHealth(id,health);
            if(ImGui::Button("HEAL SELECTED TO 100 HP",ImVec2(-1,32)))
                HunterNative::RequestTargetHealth(id,100);
            if(ImGui::Button("REVIVE SELECTED",ImVec2(-1,32)))
                HunterNative::RequestTargetRevive(id);
            ImGui::Separator();
            if(ImGui::Button("GODMODE ON",ImVec2(-1,30)))
                HunterNative::RequestTargetGodMode(id,true);
            if(ImGui::Button("GODMODE OFF",ImVec2(-1,30)))
                HunterNative::RequestTargetGodMode(id,false);
            ImGui::Separator();
            static bool confirmKill=false;
            ImGui::Checkbox("Confirm selected-player kill", &confirmKill);
            const bool disableKill=!confirmKill;
            if(disableKill)ImGui::BeginDisabled();
            if(ImGui::Button("KILL SELECTED PLAYER",ImVec2(-1,30))) {
                HunterNative::RequestTargetKill(id);
                confirmKill=false;
            }
            if(disableKill)ImGui::EndDisabled();
            if(!allowed)ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"KICK PLAYER");
            ImGui::TextDisabled("Uses FL_Gamemode_C.Server_Kick with engine-created FText.");
            static bool confirmKick=false;
            ImGui::Checkbox("Confirm kick (no ban)",&confirmKick);
            const bool disableKick=!allowed || !confirmKick;
            if(disableKick) ImGui::BeginDisabled();
            if(ImGui::Button("KICK SELECTED PLAYER",ImVec2(-1,30))) {
                HunterNative::RequestTargetKick(id);
                confirmKick=false;
            }
            if(disableKick) ImGui::EndDisabled();
            ImGui::TextDisabled("Host cannot kick themselves. Player disconnect must be verified.");
            Notice("TEST IN PRIVATE HOSTED MATCHES",
                   "Player health, revive and kill requests are experimental. Server rejection or no effect may occur.",true);
        }

        void DrawMischief()
        {
            Heading("THE MISCHIEF CABINET","JUMP SCARES  /  HAUNTED PRANKS  /  HOST HORROR");
            HunterScares::DrawControls();
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"THE HOST'S HAUNTINGS");

            const auto selectedPrankTarget=DrawPlayerSelect("Scare target (host actions)");
            ImGui::TextDisabled("Select a player for a client-side native sound or AI noise event.");
            const auto sounds=HunterNative::GetSounds();
            static int selectedSound=-1;
            static float selectedSoundVolume=1.f;
            std::string soundName="Choose a loaded game sound";
            for(const auto& sound:sounds)if(sound.objectIndex==selectedSound)
                soundName=sound.displayName;
            if(ImGui::BeginCombo("Native horror sound",soundName.c_str())) {
                for(const auto& sound:sounds) {
                    ImGui::PushID(sound.objectIndex);
                    if(ImGui::Selectable(sound.displayName.c_str(),
                        selectedSound==sound.objectIndex))selectedSound=sound.objectIndex;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Loaded in-game sounds found: %d. Catalog fills as assets are scanned.",
                static_cast<int>(sounds.size()));
            ImGui::SliderFloat("Remote sound volume",&selectedSoundVolume,.1f,2.f,"%.1fx");
            if(!HunterNative::GetStatus().host || selectedSound<0)ImGui::BeginDisabled();
            if(selectedPrankTarget<0)ImGui::BeginDisabled();
            if(ImGui::Button("PLAY SOUND ON SELECTED HUNTER",ImVec2(-1,30)))
                HunterNative::RequestTargetSound(selectedPrankTarget,selectedSound,selectedSoundVolume);
            if(selectedPrankTarget<0)ImGui::EndDisabled();
            if(ImGui::Button("PLAY SOUND ON ALL HUNTERS",ImVec2(-1,30)))
                HunterNative::RequestTargetSound(-1,selectedSound,selectedSoundVolume);
            if(!HunterNative::GetStatus().host || selectedSound<0)ImGui::EndDisabled();
            ImGui::TextDisabled("Uses PlayerController.ClientPlaySound (owning-client RPC).");
            const auto shakes=HunterNative::GetCameraShakes();
            static int selectedShake=-1;
            static float shakeIntensity=1.f;
            std::string shakeName="Choose a loaded camera shake class";
            for(const auto& item:shakes)if(item.objectIndex==selectedShake)
                shakeName=item.displayName;
            if(ImGui::BeginCombo("Online scare camera effect",shakeName.c_str())) {
                for(const auto& item:shakes) {
                    ImGui::PushID(item.objectIndex);
                    if(ImGui::Selectable(item.displayName.c_str(),
                        selectedShake==item.objectIndex))selectedShake=item.objectIndex;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Loaded native camera shakes: %d",
                static_cast<int>(shakes.size()));
            ImGui::SliderFloat("Scare shake intensity",&shakeIntensity,.1f,2.f,"%.1fx");
            if(!HunterNative::GetStatus().host || selectedShake<0)ImGui::BeginDisabled();
            if(selectedPrankTarget<0)ImGui::BeginDisabled();
            if(ImGui::Button("SHAKE SELECTED HUNTER'S CAMERA",ImVec2(-1,30)))
                HunterNative::RequestTargetCameraShake(selectedPrankTarget,selectedShake,shakeIntensity);
            if(selectedPrankTarget<0)ImGui::EndDisabled();
            if(ImGui::Button("SHAKE ALL HUNTERS' CAMERAS",ImVec2(-1,30)))
                HunterNative::RequestTargetCameraShake(-1,selectedShake,shakeIntensity);
            if(!HunterNative::GetStatus().host || selectedShake<0)ImGui::EndDisabled();
            ImGui::TextDisabled("Native shake + sound can reach owning clients; custom local ImGui art cannot.");
            ImGui::TextDisabled("For an online scare, trigger the sound and shake on the same target.");
            if(selectedPrankTarget>=0) {
                static float loudness=5.f, range=70.f;
                ImGui::SliderFloat("Noise loudness",&loudness,0.1f,25.f);
                ImGui::SliderFloat("Noise radius (meters)",&range,5.f,250.f);
                if(ImGui::Button("MAKE SELECTED HUNTER ATTRACT ATTENTION",ImVec2(-1,30)))
                    HunterNative::RequestTargetNoise(selectedPrankTarget,loudness,range);
            }
            const auto horrorHost=HunterNative::GetStatus();
            ImGui::TextDisabled("Game-native replicated horror events / host only.");
            if (!horrorHost.host) ImGui::BeginDisabled();
            if (ImGui::Button("VAMPIRE SCREAM  [HOST]",ImVec2(-1,32)))
                HunterNative::RequestVampireScream();
            if (ImGui::Button("FLICKER ALL HUNTERS' FLASHLIGHTS  [HOST]",ImVec2(-1,32)))
                HunterNative::RequestFlickerLights(false);
            if (ImGui::Button("RESTORE HUNTERS' FLASHLIGHTS  [HOST]",ImVec2(-1,32)))
                HunterNative::RequestFlickerLights(true);
            if (!horrorHost.host) ImGui::EndDisabled();
            ImGui::TextDisabled("Native host effects require loaded actors and two-client playback testing.");
            ImGui::Separator();

            const auto status=HunterNative::GetStatus();
            Notice(status.host?"THE HOST CONTROLS THE NIGHT":"HOST AUTHORITY REQUIRED",
                status.host?"Actions use Folklore Hunter's existing game/Blueprint functions."
                :"These effects are disabled for remote clients; host a private game to try them.",
                !status.host);
            const auto pairButtons=[](const char* a, bool(*fa)(), const char* b, bool(*fb)()) {
                float w=std::max(125.f,(ImGui::GetContentRegionAvail().x-12.f)*0.5f);
                if(ImGui::Button(a,ImVec2(w,30)))fa();
                ImGui::SameLine();
                if(ImGui::Button(b,ImVec2(w,30)))fb();
            };
            if(!status.host)ImGui::BeginDisabled();
            ImGui::TextColored(Palette::col_gold,"WEATHER SORCERY");
            pairButtons("DRIZZLE",[]()->bool{return HunterNative::RequestRain(.3);},
                        "SUDDEN STORM",[]()->bool{return HunterNative::RequestRain(1.0);});
            pairButtons("SNOWFALL",[]()->bool{return HunterNative::RequestSnow(.4);},
                        "BLIZZARD",[]()->bool{return HunterNative::RequestSnow(1.0);});
            pairButtons("CLEAR RAIN",[]()->bool{return HunterNative::RequestRain(0);},
                        "CLEAR SNOW",[]()->bool{return HunterNative::RequestSnow(0);});
            ImGui::TextDisabled("Weather tools require a live BP_WeatherTool_C actor; visibility to other players is not verified.");
            ImGui::Spacing(); ImGui::Separator();
            ImGui::TextColored(Palette::col_gold,"HIDDEN DEVELOPER MODES  /  EXPERIMENTAL");
            if (ImGui::Button("BEAN MODE",ImVec2(-1,28))) HunterNative::RequestBeanMode();
            pairButtons("RANGER MODE",[]()->bool{return HunterNative::RequestRangerMode();},
                        "S'MORE MODE",[]()->bool{return HunterNative::RequestSmoreMode();});
            ImGui::TextDisabled("SDK confirms these game-native developer modes:");
            ImGui::BulletText("Bean: FolkloreGamemode.EnableBeanMode (native Exec)");
            ImGui::BulletText("Ranger: FL_Gamemode.EnableRangerMode (Blueprint override)");
            ImGui::BulletText("S'more: FL_Gamemode.EnableSmoreMode (Blueprint override)");
            ImGui::TextDisabled("Their effects are NOT documented; dispatch is logged; gameplay must be tested.");
            ImGui::TextDisabled("Some developer modes may persist until restarting the session.");
            if(ImGui::Button("CLEAR ALL RAIN + SNOW",ImVec2(-1,30)))
                HunterNative::RequestClearWeather();
            ImGui::TextDisabled("Time of day and movement cheats were moved out of MISCHIEF.");
            ImGui::Spacing();
            if (!status.host) ImGui::EndDisabled();
            ImGui::Spacing();
            Notice("PRIVATE SESSIONS ONLY",
                "Environment effects are host-queued; do not assume they replicate to every unmodified client.",
                true);
        }

        void DrawVanity()
        {
            Heading("VANITY WARDROBE","LIVE COSMETIC CATALOG  /  SELECT & EQUIP");
            const auto status=HunterNative::GetStatus();
            const auto items=HunterNative::GetVanityItems();
            ImGui::TextColored(Palette::col_gold,"%d COSMETICS IN ACTIVE CHARACTER'S COLLECTION",static_cast<int>(items.size()));
            if(items.empty()) {
                Notice("WARDROBE UNAVAILABLE",
                    "No cosmetics exposed by the active FL_PlayerState > BP_CosmeticsManager. "
                    "Join a hunt, or check journal.log for missing PlayerState/manager.",true);
                ImGui::TextDisabled("The 5.5.4 SDK contains cosmetics entries, but not a complete static unlocked-item list.");
                return;
            }
            static int chosen=0;
            chosen=std::clamp(chosen,0,static_cast<int>(items.size())-1);
            static ImGuiTextFilter filter;
            filter.Draw("Search wardrobe",260.f);
            if(ImGui::BeginChild("##VanityAvailable",ImVec2(0,260),true)) {
                for(int i=0;i<static_cast<int>(items.size());++i) {
                    const auto& item=items[i];
                    if(!filter.PassFilter(item.label.c_str()))continue;
                    const auto text=item.label+(item.equipped?" [EQUIPPED]":"");
                    ImGui::PushID(i);
                    if(ImGui::Selectable(text.c_str(),chosen==i))chosen=i;
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::Text("Selected: %s",items[static_cast<size_t>(chosen)].label.c_str());
            if(!status.host)ImGui::BeginDisabled();
            if(ImGui::Button("EQUIP SELECTED COSMETIC",ImVec2(-1,36)))
                HunterNative::RequestEquipVanity(items[static_cast<size_t>(chosen)].className);
            if(!status.host)ImGui::EndDisabled();
            ImGui::TextDisabled("Calls the game cosmetics manager; server permissions/unlocks are unchanged.");
            ImGui::TextDisabled("Look for VANITY in journal.log to confirm the equipped class.");
        }

        void DrawSettings()
        {
            Heading("OPTIONS", "PERSONALIZE YOUR JOURNAL");
            bool changed = false;
            if (ImGui::SliderFloat("Interface scale", &prefs_.scale, 0.80f, 1.50f, "%.2fx")) changed = true;
            if (ImGui::Checkbox("Compact spacing", &prefs_.compact)) changed = true;
            if (ImGui::Checkbox("Show SDK technical hints", &prefs_.show_sdk_hints)) changed = true;
            bool diagnostic = FHJDiag::OverlayVisible();
            if (ImGui::Checkbox("Show in-game diagnostic console [F9]", &diagnostic))
                FHJDiag::ToggleOverlay();
            bool nativeConsole = FHJDiag::NativeConsoleVisible();
            if (ImGui::Checkbox("Show separate Windows console [F10]", &nativeConsole))
                FHJDiag::ToggleNativeConsole();
            ImGui::TextDisabled("To use only the in-game console, uncheck the Windows console.");
            if (changed) SavePrefs(prefs_);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(Palette::col_gold, "SKIN RECIPE");
            InfoLine("Canvas", "Smoke / charcoal");
            InfoLine("Typography", "Bone parchment");
            InfoLine("Accent", "Weathered brass / forest moss");
            InfoLine("Danger", "Dried blood red");
            if (ImGui::Button("Restore default theme")) { prefs_ = Preferences{}; SavePrefs(prefs_); }
            ImGui::Spacing();
            ImGui::TextDisabled("Options persist at %%LOCALAPPDATA%%\\FolkloreHunterJournal\\settings.ini");
        }

        void Draw()
        {
            // The enclosing window already owns the hunter theme. Apply only content scaling.
            ImGui::SetWindowFontScale(prefs_.scale);
            ImVec2 min = ImGui::GetContentRegionAvail();
            if (min.x < 370.0f) {
                ImGui::TextColored(Palette::col_gold, "Resize the journal to see the full field notes.");
                ImGui::SetWindowFontScale(1.0f);
                return;
            }
            auto* draw = ImGui::GetWindowDrawList();
            ImVec2 top = ImGui::GetCursorScreenPos();
            const float sidebarWidth = std::clamp(min.x*0.235f, 142.0f, 201.0f);
            BranchLine(draw, ImVec2(top.x+sidebarWidth+18,top.y+9), ImVec2(top.x+min.x-10,top.y+9));
            ImGui::Dummy(ImVec2(1,15));
            Navigation(sidebarWidth);
            ImGui::SameLine(0,13);
            ImGui::BeginChild("##HunterPages", ImVec2(0,0), true);
            switch(page_) {
                case Page::Journal: DrawDashboard(); break;
                case Page::Survivor: DrawSurvivor(); break;
                case Page::Equipment: DrawEquipment(); break;
                case Page::Cryptids: DrawCryptids(); break;
                case Page::Host: DrawHost(); break;
                case Page::Physics: DrawPhysics(); break;
                case Page::Lobby: DrawLobby(); break;
                case Page::Mischief: DrawMischief(); break;
                case Page::Wardrobe: DrawVanity(); break;
                case Page::Settings: DrawSettings(); break;
            }
            if (prefs_.show_sdk_hints && page_ != Page::Settings) {
                ImGui::Spacing(); ImGui::Separator();
                ImGui::TextDisabled("SOURCE: Dumper-7 UE 5.5.4 reflection, not gameplay runtime proof.");
            }
            ImGui::EndChild();
            ImGui::SetWindowFontScale(1.0f);
        }
    };
}


namespace FHJ {
void DrawJournal() { static Journal journal; journal.Render(); }
}