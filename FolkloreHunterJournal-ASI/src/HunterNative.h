#pragma once
#include <Windows.h>
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>
namespace HunterNative {
    constexpr UINT kGameTick = WM_APP + 0x358;
    constexpr UINT kGameAction = WM_APP + 0x359;
    struct CreatureInfo {
        int objectIndex = -1;
        std::string displayName;
        bool isPawn = false;
        double health = -1.0;
        double maxHealth = -1.0;
    };
    struct CameraShakeInfo {
        int objectIndex = -1;
        std::string displayName;
    };
    struct SoundInfo {
        int objectIndex = -1;
        std::string displayName;
    };
    struct CryptidSpawnClass {
        int objectIndex = -1; // live GUObject index of validated UClass
        std::string displayName;
        bool mothman = false;
        bool presentAsLiveActor = false; // same UClass already active on this map
    };
    struct CryptidSpawnPoint {
        int objectIndex = -1; // actor index in active streamed UWorld
        std::string displayName;
    };
    struct CryptidCameraSettings {
        float yawSensitivity = .12f;
        float pitchSensitivity = .12f;
        float followRate = 32.f;
        bool invertPitch = false;
        bool clearBlurDuringPossession = true;
    };
    struct VanityItem {
        std::string label;
        std::string className;
        bool equipped = false;
    };
    struct PlayerInfo {
        int objectIndex = -1;
        std::string displayName;
        bool local = false;
        double health = -1;
        bool alive = true;
        bool invulnerable = false;
    };
    struct Status {
        bool worldReady = false;
        bool host = false;
        bool cameraReady = false;
        std::string activeHuntName;
        double localHealth = -1.0;
        double localMaxHealth = -1.0;
        bool invuln = false;
        bool localGodProtected = false;
        bool fallProtectionEnabled = false;
        int fallProtectedPlayers = 0;
        int godProtectedPlayers = 0;
        int knownActors = 0;
        int activeCryptids = 0;
        int activePlayers = 0;
        int scanned = 0;
        int totalObjects = 0;
        int levelsScanned = 0;
        int actorsExamined = 0;
        int espCandidates = 0;
        int activeWildlife = 0;
        int activeContainers = 0;
        int activeQuestObjects = 0;
        int activeLoot = 0;
        int activeMobSpawners = 0;
        int pendingSpawnTriggers = 0;
        int confirmedSpawned = 0;
    };
    void AttachWindow(HWND window);
    void RequestTick();
    void HandleWindowMessage(UINT message);
    Status GetStatus();
    std::vector<PlayerInfo> GetPlayers();
    std::vector<CreatureInfo> GetCreatures();
    std::vector<SoundInfo> GetSounds();
    std::vector<CameraShakeInfo> GetCameraShakes();
    std::vector<CryptidSpawnClass> GetSpawnableCryptidClasses();
    std::vector<CryptidSpawnPoint> GetActiveMapSpawners();
    CryptidCameraSettings GetCryptidCameraSettings();
    void SetCryptidCameraSettings(const CryptidCameraSettings& value);
    bool MothmanLocalSmokeReduction();
    void SetMothmanLocalSmokeReduction(bool enabled);
    std::vector<VanityItem> GetVanityItems();
    bool RequestEquipVanity(const std::string& className);
    // Gameplay changes are runtime experiments and may not replicate to clients.
    bool RequestFallProtection(bool enabled);
    bool RequestSpeedMultiplier(float factor, bool entireLobby);
    bool RequestJumpBoost(float multiplier, bool entireLobby = false);
    bool RequestGravity(float gravityScale, bool entireLobby);
    bool RequestSpawnMobWave(int count, int selectedLiveCryptidIndex = -1);
    // Spawn a class loaded in the active hunt at a selected native map
    // spawner (-1 = automatic; host-pawn fallback if the map has none).
    bool RequestSpawnCryptidClass(int classObjectIndex, int count,
                                  int mapSpawnerIndex = -1, bool autoControl = false);
    bool RequestSetMapSpawnerEnabled(int mapSpawnerIndex, bool enabled);
    bool RequestPossessCryptid(int objectIndex);
    bool RequestRestoreHuman();
    // Only abilities exposed by the matching creature's reflected class are invoked.
    bool RequestMonsterAbility(int slot);
    // -1 uses automatic native sound matching; otherwise a loaded USoundBase
    // GUObject index chosen from the Journal's sound catalog.
    void SetMonsterAttackSound(int soundObjectIndex);
    bool RequestNoclip(bool enabled);
    bool RequestTargetHealth(int targetObjectIndex, double value);
    bool RequestTargetItem(int targetObjectIndex, const std::string& itemClass, int amount);
    bool RequestTargetNoise(int targetObjectIndex, double loudness, double rangeMeters);
    // PlayerController.ClientPlaySound is a native owning-client RPC.
    // Target -1 sends one request per scanned player on the authoritative host.
    bool RequestTargetSound(int targetObjectIndex, int soundObjectIndex, float volume);
    bool RequestTargetCameraShake(int targetObjectIndex, int shakeClassObjectIndex, float scale);
    bool RequestTargetRevive(int targetObjectIndex);
    bool RequestTargetKill(int targetObjectIndex);
    bool RequestTargetKick(int targetObjectIndex);
    bool RequestTargetGodMode(int targetObjectIndex, bool enabled);
    bool ESPEnabled();
    bool MonsterModeActive();
    bool NoclipActive();
    int MonsterSpecies();
    void DrawMonsterHUD(ImVec2 size);
    void SetJournalOpen(bool isOpen);
    void FeedRawMouse(LPARAM lParam);
    void DrawESP(ImVec2 size);
    void DrawESPSettings();
    bool RequestGiveItem(const std::string& itemName, int amount);
    bool RequestGodMode();
    bool RequestAddXP(int xp);
    bool RequestAddLevels(int levels);
    bool RequestTime(float hour);
    bool RequestTimeScale(float scale);
    bool RequestAddSilver(int amount);
    bool RequestInfiniteDamage();
    // Item catalog uses the real UItem_*_C class names from the SDK.
    bool RequestCatalogItem(const std::string& className, int amount);
    bool RequestBeanMode();
    bool RequestRangerMode();
    bool RequestSmoreMode();
    bool RequestRain(double intensity);
    bool RequestSnow(double intensity);
    bool RequestClearWeather();
    bool RequestVampireScream();
    bool RequestFlickerLights(bool cancel);
}
