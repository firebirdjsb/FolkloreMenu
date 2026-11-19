// Folklore Hunter UE5.5.4 reflection bridge; no third-party injection framework.
// Exact pointers and layouts originate from the uploaded Dumper-7 SDK snapshot.
// All function execution is marshaled to the hooked game-window message thread,
// which must be confirmed to be UE's game thread before broad multiplayer use.
#define WIN32_LEAN_AND_MEAN
#include "HunterNative.h"
#include "JournalDiagnostics.h"
#include <imgui.h>
#include <Windows.h>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <iterator>

namespace {
    constexpr uintptr_t WORLD_RVA=0x08E17260;
    constexpr uintptr_t OBJECTS_RVA=0x08C9A970;
    constexpr uintptr_t NAME_APPEND_RVA=0x01253180;
    constexpr size_t PROCESS_EVENT_INDEX=0x4F;
    constexpr DWORD kScanIntervalMs=110;
    constexpr int kMaxActors=180;
    constexpr int kScanPerTick=1600;
    struct Vec3 { double x{},y{},z{}; };
    struct Rot { double pitch{},yaw{},roll{}; };
    struct Name { int32_t index{}; uint32_t number{}; };
    struct FStringView { wchar_t* data{}; int32_t size{},capacity{}; };
    struct Marker {
        Vec3 pos{}; int kind{};
        double health{-1.0},maxHealth{-1.0};
        bool mothman=false; // APawn species has no base-cryptid HP fields
    };
    struct Camera { Vec3 pos{}; Rot rot{}; float fov{}; };
    struct ActorEntry { uintptr_t address{}; int index{}; int kind{}; };
    struct LevelView { uintptr_t data{}; int32_t count{}; int32_t start{}; };
    enum class ActionType { GiveItem, CatalogItem, GodMode, AddXP, AddLevels, Time, TimeScale, AddSilver, InfiniteDamage, BeanMode, RangerMode, SmoreMode, Rain, Snow, ClearWeather, VampireScream, FlickerLights, CancelFlicker, TargetHealth, TargetItem, TargetNoise, TargetRevive, TargetKill, TargetKick, TargetGodMode, TargetSound, TargetCameraShake, NoclipOn, NoclipOff, FallProtection, SpeedChange, JumpBoost, GravityChange, SpawnMob, SpawnSpecies, SpawnerToggle, PossessCryptid, RestoreHuman, MonsterAbility, EquipVanity };
    struct Action { ActionType type{}; std::string item; int amount{}; float value{}; int target{-1}; double precise{}; };
    std::mutex gMutex;
    std::mutex gActionsMutex;
    HWND gWindow=nullptr;
    ULONGLONG gLastTick=0;
    std::atomic_bool gTickPending{false};
    std::atomic_bool gActionPending{false};
    std::vector<Action> gActions;
    std::vector<ActorEntry> gKnown;
    std::vector<ActorEntry> gKnownSpawners;
    std::vector<HunterNative::CryptidSpawnClass> gSpawnClassCandidates,gSpawnClassCatalog;
    std::vector<HunterNative::CryptidSpawnPoint> gSpawnPointCatalog;
    int gSpawnClassScanCursor=0;
    std::vector<Marker> gMarkers;
    std::vector<HunterNative::PlayerInfo> gRoster;
    std::vector<HunterNative::CreatureInfo> gCreatures;
    std::vector<HunterNative::SoundInfo> gSounds, gSoundCandidates;
    std::vector<HunterNative::CameraShakeInfo> gCameraShakes, gCameraShakeCandidates;
    int gSoundScanCursor=0;
    struct VanityChoice { uintptr_t klass{}; std::string name; bool equipped{}; };
    std::vector<VanityChoice> gVanityChoices;
    // Saved original values for correct reversibility, keyed to live pawn identity.
    struct PhysicsBackup {
        int32_t index = -1;
        double walk = 0, sprint = 0;
        double safeFall = 0, maxFallDistance = 0, maxFallDamage = 0;
        float jump = 0, gravityScale = 0, maxWalk = 0;
    };
    std::unordered_map<uintptr_t, PhysicsBackup> gPhysicsBackups;
    // Damage protection is independent of optional movement-component reads.
    struct FallBackup {
        int32_t index = -1;
        double safe=0, maximum=0, damage=0;
        bool airborne=false;
        bool verified=false;
        bool priorInvuln=false;
        double healthBeforeFall=-1;
        ULONGLONG lastRestore=0;
        ULONGLONG lastVerifyWarning=0;
        ULONGLONG landingGraceUntil=0;
    };
    struct GodBackup {
        int32_t index = -1;
        bool priorInvuln = false;
        double protectedHealth = -1;
        ULONGLONG lastCorrection = 0;
    };
    std::unordered_map<uintptr_t, FallBackup> gFallBackups;
    std::unordered_map<uintptr_t, GodBackup> gGodBackups;
    uintptr_t gHumanPawn = 0;
    int32_t gHumanIndex = -1;
    uintptr_t gHumanInputController=0;
    bool gHumanInputDisabled=false, gHumanIgnoreMovement=false, gHumanIgnoreLook=false;
    bool gPossessedCryptid = false;
    // One map spawner can be used for many requests. Pace native TriggerSpawn
    // invocations so a one-spawner level doesn't silently cap a wave at one.
    struct SpawnWave {
        int remaining=0, requested=0, dispatched=0, speciesIndex=-1;
        ULONGLONG nextTick=0;int created=0;
        int classObjectIndex=-1, spawnerObjectIndex=-1;
        bool controlFirst=false;
    };
    SpawnWave gSpawnWave{};
    std::atomic_bool gWildlife{true}, gContainers{true}, gQuest{true}, gLoot{true};
    
    uintptr_t gNoclipPawn = 0;
    uint8_t gPriorMovement = 1;
    bool gPriorCollision = true;
    bool gNoclipEnabled = false;
    // Minecraft-style free-flight holds its own position every simulation
    // update. CharacterMovement/Blueprint velocity cannot create idle drift.
    Vec3 gNoclipPosition{};
    bool gNoclipPositionValid=false;
    uintptr_t gNoclipController=0;
    bool gNoclipIgnoredMove=false;
    float gOriginalFlySpeed=0.f;
    uintptr_t gOriginalFlyMovement=0;
    bool gFlySpeedSaved=false;
    uintptr_t gMonsterAIController=0, gMonsterBrain=0;
    bool gMonsterBrainWasRunning=false;
    uintptr_t gMonsterMovement=0;
    float gMonsterOriginalWalkSpeed=0.f, gMonsterOriginalFlySpeed=0.f;
    float gMonsterNativeWalkSpeed=0.f, gMonsterNativeRunSpeed=0.f;
    bool gMonsterSpeedsSaved=false;
    bool gMonsterModeSaved=false;
    struct GroundMovementSettings {
        bool saved=false;
        uintptr_t pawn=0,movement=0;
        int32_t pawnId=-1;
        uint8_t rotationBefore=0,pathBefore=0,pawnYawBefore=0;
    } gGroundMovementSettings;
    struct GroundMovementDrive {
        uintptr_t pawn=0;
        int32_t pawnId=-1;
        bool useNavFallback=false;
        bool sampleValid=false;
        Vec3 samplePosition{};
        ULONGLONG sampleAt=0;
    } gGroundDrive;
    bool gMonsterFlightActive=false;
    bool gGroundJumpHeld=false;
    // AI path following accepts movement for AI-possessed cryptids; a regular
    // Pawn.AddMovementInput call can be ignored by their controller.
    ULONGLONG gMonsterLastMoveRequest=0;
    bool gMonsterPathActive=false;
    double gMonsterMoveDirX=0.0,gMonsterMoveDirY=0.0;
    Vec3 gMonsterMoveGoal{};
    std::atomic<float> gCryptidYawSensitivity{0.12f};
    std::atomic<float> gCryptidPitchSensitivity{0.12f};
    std::atomic<float> gCryptidCameraFollow{32.0f};
    std::atomic_bool gCryptidInvertPitch{false};
    std::atomic_bool gCryptidClearLocalBlur{true};
    // The two motion-blur CVars and depth-of-field CVar are viewport-
    // local, not replicated. Save before changing and restore on return.
    struct PossessionClarityBackup {
        bool active=false;
        uintptr_t camera=0;
        int32_t cameraIndex=-1;
        bool cameraSaved=false;
        uint8_t previousOverrides=0;
        float previousWeight=0.f,previousBlurAmount=0.f,previousBlurMax=0.f;
        std::array<int32_t,3> originalCvars{};
        std::array<bool,3> cvarsSaved{};
    } gPossessionClarity;
    ULONGLONG gLastPossessionClarityRefresh=0;
    std::atomic_bool gMothmanReduceLocalSmoke{true};
    struct LocalMothmanSmoke {
        uintptr_t pawn=0,component=0;
        int32_t pawnIndex=-1,componentIndex=-1;
        bool previouslyHidden=false;
    } gLocalMothmanSmoke;
    struct LocalMothmanRender {
        struct Part {
            uintptr_t component=0;
            int32_t id=-1;
            bool originallyHidden=false;
            bool modified=false;
        };
        uintptr_t pawn=0;
        int32_t pawnId=-1;
        std::array<Part,3> visibleParts{};
        uintptr_t postProcess=0;
        int32_t postId=-1;
        bool postEnabled=false,postModified=false;
        bool lastStealthed=false,stealthLogged=false;
    } gMothRender;
    struct NativeMothmanFlight {
        uintptr_t pawn=0;
        int32_t objectIndex=-1;
        Vec3 smoothedVelocity{},lastPosition{};
        ULONGLONG lastMotionSeen=0;
        bool positionValid=false;
        bool loggedFallback=false;
    } gMothFlight;
    std::unordered_map<std::wstring,uintptr_t> gMonsterFunctions;
    ULONGLONG gLastCreatureMethodWarning=0;
    uint8_t gMonsterPriorMovement=1;
    uintptr_t gHumanHUD=0;
    int32_t gHumanHUDIndex=-1;
    uint8_t gPriorHumanHudVisibility=0;
    bool gHumanHudHidden=false;
    bool gHudRestorePending=false;
    int gVampireStance=0;
    bool gVampireInvisible=false;
    struct VampireInvisibilityVerification {
        uintptr_t pawn=0;
        int32_t actorIndex=-1;
        bool wanted=false;
        ULONGLONG verifyAt=0;
    } gVampireVerify;
    std::array<ULONGLONG,5> gAbilityTicks{};
    std::array<bool,3> gAbilityKeyDown{};
    std::atomic_int gMonsterAttackSoundIndex{-1};
    ULONGLONG gLastAbilityTick=0;
    Vec3 gChaseSmoothedPos{};
    bool gChaseSmoothedValid=false;
    bool gPuppeteering=false;
    bool gOriginalCameraAutoManage=true, gCameraAutoManageSaved=false;
    std::atomic_bool gMonsterActive{false}, gJournalOpen{false};
    uintptr_t gMonsterPawn=0, gChaseCamera=0;
    int32_t gMonsterIndex=-1, gCameraIndex=-1;
    double gMonsterYaw=0.0, gMonsterPitch=0.0;
    double gFlightSpeed=1100.0;
    // Controlled creature identity survives the old UObject being destroyed.
    // After a game-world countdown a native map spawner is triggered once,
    // and the camera/HUD are rebound only to a verified new cryptid actor.
    uintptr_t gControlledCryptidClass=0;
    uintptr_t gControlledRespawnSpawner=0;
    int32_t gControlledRespawnSpawnerIndex=-1;
    std::unordered_set<int32_t> gControlledExistingIds;
    double gControlledRespawnStart=-1.0, gControlledRespawnRemaining=0.0;
    bool gControlledSpawnRequested=false;
    ULONGLONG gNextControlledRespawnScan=0;
    Vec3 gControlledLastPosition{};
    ULONGLONG gLastMotionTick=0, gLastScanTick=0, gLastAttackTick=0;
    std::mutex gMonsterStatusMutex;
    struct MonsterHUDState {
        std::string label{"CRYPTID"};
        int species=0;
        double health=0.0, maxHealth=0.0;
        bool cameraReady=false;
        bool possessConfirmed=false;
        bool respawning=false;
        float respawnSeconds=0.f;
        // Only the current cryptid's callable, reflected skills appear.
        std::array<bool,5> available{};
        std::array<std::string,5> skills{};
    } gMonsterHUD;
    struct WorldGravityBackup {
        uintptr_t worldSettings=0;
        float worldGravity=-980.f,globalGravity=-980.f;
        uint8_t flags=0;
    };
    WorldGravityBackup gGravityBackup{};
    std::atomic_bool gProtectFall{false},gProtectLobby{false},gSpeedLobby{false},gJumpLobby{false};
    std::atomic<float> gJumpMultiplier{1.f},gSpeedMultiplier{1.f};
    Camera gCamera{};
    HunterNative::Status gStatus{};
    uintptr_t gPrevWorld=0;
    int gScanIndex=0;
    std::unordered_map<uintptr_t,int> gKinds;
    std::unordered_map<uintptr_t,std::wstring> gClassNames;
    std::unordered_map<std::wstring,uintptr_t> gFunctionCache;
    std::atomic_bool gEsp{false}, gCryptids{true}, gPlayers{true};
    std::atomic_int gMaxDistanceMeters{350};
    std::once_flag gEspPrefsOnce;
    uintptr_t LocalPawn();
    void ReapplyMovementForNewPlayers();
    std::vector<LevelView> GetWorldLevels(uintptr_t world,int& total);
    template<typename T> bool TryWrite(uintptr_t address,const T& value) noexcept;
    bool ApplyGodProtection(uintptr_t pawn,bool enabled);
    void MaintainGodProtection();
    void ForceActorNetUpdate(uintptr_t pawn);
    void FreeflightTick();
    void RestoreLocalMothmanSmoke();
    void SyncLocalMothmanSmoke();
    void RestoreLocalMothmanRender();
    void SyncLocalMothmanRender();
    void ApplyPossessionClarity(uintptr_t pc,uintptr_t cameraComponent);
    void RestorePossessionClarity(uintptr_t pc);
    void RefreshPossessionClarity(uintptr_t pc);
    void VerifyVampireInvisibility(ULONGLONG now);
    void TickNativeMothmanFlight(uintptr_t pawn,const Vec3& desiredInput,
                                  bool boosting,double dt,ULONGLONG now);
    uintptr_t CreatureSkillFunction(int species,int slot);
    std::string CreatureSkillLabel(int species,int slot,uintptr_t fn);
    void AdvanceSpawnWave();
    void PossessCreature(int index,bool restore);
    void RefreshSpawnableClassCatalog();
    std::atomic_bool gAutoJump{false};
    int gLastReported=0;
    constexpr double kPi=3.1415926535897932384626433832795;

    template<class T> bool Read(uintptr_t address,T& out) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (address<0x10000 || address>0x00007FFFFFFFFFFFULL) return false;
        __try { std::memcpy(&out,reinterpret_cast<const void*>(address),sizeof(T)); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    uintptr_t ReadPtr(uintptr_t address) {
        uintptr_t p{};
        return Read(address,p) && p>=0x10000 && p<0x00007FFFFFFFFFFFULL ? p:0;
    }
    uintptr_t Base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
    bool FetchFName(uintptr_t obj, Name& name) { return Read(obj+0x18,name); }
    // Separate SEH wrapper; don't permit an engine exception to terminate the game.
    bool AppendName(const Name& name,wchar_t* output,int len) noexcept {
        if (!output || len<2) return false;
        struct Plain { Name key; FStringView text; } args{};
        args.key=name;
        args.text=FStringView{output,0,len};
        output[0]=0;
        using Append = void(*)(const Name*,FStringView&);
        auto fn=reinterpret_cast<Append>(Base()+NAME_APPEND_RVA);
        __try { fn(&args.key,args.text); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        output[len-1]=0;
        return output[0]!=0;
    }
    std::wstring ObjectName(uintptr_t obj) {
        Name n{}; wchar_t buffer[256]{};
        if (!FetchFName(obj,n) || !AppendName(n,buffer,256)) return {};
        return buffer;
    }
    std::wstring ClassName(uintptr_t cls) {
        if (!cls) return {};
        if (auto it=gClassNames.find(cls);it!=gClassNames.end()) return it->second;
        const auto name=ObjectName(cls);
        if (!name.empty()) gClassNames.emplace(cls,name);
        return name;
    }
    std::wstring Lower(std::wstring text) {
        std::transform(text.begin(),text.end(),text.begin(),
            [](wchar_t ch){return static_cast<wchar_t>(towlower(ch));});
        return text;
    }
    bool Matches(const std::wstring& text,std::initializer_list<const wchar_t*> tokens) {
        for(auto value:tokens) if(text.find(value)!=std::wstring::npos) return true;
        return false;
    }
    bool IsPawnClass(uintptr_t cls) {
        for(int i=0;i<25 && cls;++i) {
            const auto name=ClassName(cls);
            if(name==L"Pawn" || name==L"Character")return true;
            auto parent=ReadPtr(cls+0x40);
            if(!parent || parent==cls)break;
            cls=parent;
        }
        return false;
    }
    bool GetObjects(int& count,uintptr_t& chunks);
    uintptr_t ObjectAt(int index,uintptr_t chunks);
    int ClassKind(uintptr_t cls);
    bool IsCameraShakeClass(uintptr_t klass) {
        if(ClassName(klass)==L"CameraShakeBase")return false; // abstract base
        for(int depth=0;klass && depth<25;++depth) {
            if(ClassName(klass)==L"CameraShakeBase")return depth>0;
            const auto parent=ReadPtr(klass+0x40);
            if(!parent || parent==klass)break;
            klass=parent;
        }
        return false;
    }
    bool IsSoundAssetClass(uintptr_t cls) {
        for(int depth=0;cls && depth<25;++depth) {
            const auto name=ClassName(cls);
            if(name==L"SoundBase")return true;
            const auto parent=ReadPtr(cls+0x40);
            if(!parent || parent==cls)break;
            cls=parent;
        }
        return false;
    }
    void RefreshSpawnableClassCatalog() {
        // Incremental GUObject enumeration: Blueprint classes can be loaded
        // independently of the level's live actors. Do not inspect 49k
        // UObjects in one 16ms frame.
        int count=0;uintptr_t chunks=0;
        if(!GetObjects(count,chunks))return;
        static ULONGLONG lastCatalogCycle=0;
        const auto now=GetTickCount64();
        if(gSpawnClassScanCursor>=count &&
           now-lastCatalogCycle>12000) {
            lastCatalogCycle=now;
            gSpawnClassScanCursor=0;
            // Already-discovered classes remain in the catalog, but their
            // indices are checked again before a spawn is accepted.
        }
        for(int n=0;n<350 && gSpawnClassScanCursor<count;++n,++gSpawnClassScanCursor) {
            const int id=gSpawnClassScanCursor;
            const auto klass=ObjectAt(id,chunks);
            if(!klass)continue;
            const auto meta=ClassName(ReadPtr(klass+0x10));
            if(meta!=L"BlueprintGeneratedClass" && meta!=L"Class")continue;
            if(ClassKind(klass)!=2 || !IsPawnClass(klass))continue;
            const auto name=ObjectName(klass);
            // Full three-map SDK comparison: BP_Cryptid_Base_C and
            // BP_VampireBase_C are inherited base classes, not the
            // complete gameplay actors. Offering them for spawning caused
            // empty models / missing AI on foreign maps. Only concrete
            // species encountered as actual PersistentLevel actors qualify.
            if(name!=L"BP_FlyingPawnTest_C" &&
               name!=L"BP_Wendigo_C" && name!=L"BP_Vampire_C")continue;
            int32_t liveId=-1;
            if(!Read(klass+0x0C,liveId) || liveId!=id)continue;
            const bool already=std::any_of(gSpawnClassCandidates.begin(),
                gSpawnClassCandidates.end(),
                [id](const auto& entry){return entry.objectIndex==id;});
            if(already)continue;
            const bool mothman=name==L"BP_FlyingPawnTest_C";
            const auto title=mothman?"Mothman / BP_FlyingPawnTest_C":
                std::string(name.begin(),name.end());
            gSpawnClassCandidates.push_back({id,title,mothman});
        }
    }
    void ScanLoadedSounds() {
        if(gSoundCandidates.size()>=256 && gCameraShakeCandidates.size()>=96)return;
        int total=0;uintptr_t chunks=0;
        if(!GetObjects(total,chunks))return;
        for(int n=0;n<600 && gSoundScanCursor<total;++n,++gSoundScanCursor) {
            const auto sound=ObjectAt(gSoundScanCursor,chunks);
            if(!sound)continue;
            const auto type=ClassName(ReadPtr(sound+0x10));
            const bool asset=gSoundCandidates.size()<256 &&
                IsSoundAssetClass(ReadPtr(sound+0x10));
            const bool shakeClass=gCameraShakeCandidates.size()<96 &&
                (type==L"Class" || type==L"BlueprintGeneratedClass") &&
                IsCameraShakeClass(sound);
            if(!asset && !shakeClass)continue;
            auto name=ObjectName(sound);
            if(name.empty())continue;
            int32_t index=-1;
            if(!Read(sound+0x0C,index) || index!=gSoundScanCursor)continue;
            if(asset)gSoundCandidates.push_back({index,std::string(name.begin(),name.end())});
            if(shakeClass)gCameraShakeCandidates.push_back(
                {index,std::string(name.begin(),name.end())});
        }
    }
    int ClassKind(uintptr_t cls) {
        if (!cls) return 0;
        if (auto it=gKinds.find(cls);it!=gKinds.end()) return it->second;
        uintptr_t current=cls;
        int result=0;
        for (int depth=0;depth<24 && current;++depth) {
            const auto name=ClassName(current);
            if (name==L"PlayerCharacter_C") {result=1;break;}
            // SDK: Mothman is BP_FlyingPawnTest_C : APawn, not a
            // BP_Cryptid_Base_C/ACryptidBase descendant. Treat it as a
            // cryptid for the live roster/ESP/possession selection.
            if (name==L"BP_Cryptid_Base_C" || name==L"CryptidBase" ||
                name==L"BP_FlyingPawnTest_C") {result=2;break;}
            const auto parent=ReadPtr(current+0x40);
            if (!parent || parent==current)break;
            current=parent;
        }
        if(!result) {
            const auto name=Lower(ClassName(cls));
            // Class-name categories are heuristics. This supplements actual inheritance checks.
            // Exclude level-only trigger/volume components to reduce false markers.
            if(Matches(name,{L"collision",L"trigger",L"volume",L"widget",L"component",L"preview"})) {
                if(!name.empty())gKinds.emplace(cls,0);
                return 0;
            }
            if(Matches(name,{L"wolf",L"deer",L"bear",L"boar",L"animal",L"wildlife",L"npc",L"mob_",L"creature"}))
                result=3;
            else if(Matches(name,{L"chest",L"crate",L"container",L"lootbox",L"loot_box",L"box_"}))
                result=4;
            else if(Matches(name,{L"quest",L"mission",L"objective",L"contract_",L"collectible"}))
                result=5;
            else if(Matches(name,{L"droppeditem",L"pickup",L"loot_",L"itemdrop",L"placeable",L"supply"}))
                result=6;
        }
        // Once a UClass name resolves, its inherited class and heuristic
        // category are stable until the world cache is invalidated. Caching
        // non-cryptid classes too avoids repeated ancestry walks on every
        // streamed-level scan and improves possession frame pacing.
        if(result || !ClassName(cls).empty())gKinds.emplace(cls,result);
        return result;
    }
    bool GetObjects(int& count,uintptr_t& chunks) {
        const uintptr_t array=Base()+OBJECTS_RVA;
        count=0;
        chunks=ReadPtr(array);
        int n=0,nchunks=0;
        if (!chunks || !Read(array+0x14,n) || !Read(array+0x1C,nchunks) ||
            n<0 || n>2000000 || nchunks<1 || nchunks>64) return false;
        count=n;return true;
    }
    uintptr_t ObjectAt(int index,uintptr_t chunks) {
        if (index<0) return 0;
        const uintptr_t chunk=ReadPtr(chunks+sizeof(void*)*static_cast<size_t>(index/65536));
        return chunk ? ReadPtr(chunk+static_cast<size_t>(index%65536)*0x18) : 0;
    }
    uintptr_t World() { return ReadPtr(Base()+WORLD_RVA); }
    Camera GetCamera(uintptr_t world) {
        Camera c{};
        auto instance=ReadPtr(world+0x1D8);
        auto players=ReadPtr(instance+0x38);
        int32_t n=0;
        if (!players || !Read(instance+0x40,n) || n<1 || n>8) return c;
        auto local=ReadPtr(players);
        auto pc=ReadPtr(local+0x30);
        auto manager=ReadPtr(pc+0x360);
        if (!manager) return c;
        constexpr uintptr_t pov=0x1410+0x10;
        if (!Read(manager+pov,c.pos) ||
            !Read(manager+pov+0x18,c.rot) ||
            !Read(manager+pov+0x30,c.fov)) return {};
        if (!std::isfinite(c.fov) || c.fov<20.f || c.fov>155.f ||
            !std::isfinite(c.pos.x) || !std::isfinite(c.rot.yaw)) return {};
        return c;
    }
    std::wstring SettingsPath() {
        wchar_t path[MAX_PATH]{};
        DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",path,MAX_PATH);
        return n&&n<MAX_PATH ? std::wstring(path)+L"\\FolkloreHunterJournal\\journal.ini" : std::wstring();
    }
    void LoadPreferences() {
        std::call_once(gEspPrefsOnce, [] {
            const auto f=SettingsPath();
            if (f.empty()) return;
            gEsp.store(GetPrivateProfileIntW(L"ESP",L"Enabled",0,f.c_str())!=0);
            gCryptids.store(GetPrivateProfileIntW(L"ESP",L"Cryptids",1,f.c_str())!=0);
            gPlayers.store(GetPrivateProfileIntW(L"ESP",L"Players",1,f.c_str())!=0);
            gWildlife.store(GetPrivateProfileIntW(L"ESP",L"Wildlife",1,f.c_str())!=0);
            gContainers.store(GetPrivateProfileIntW(L"ESP",L"Containers",1,f.c_str())!=0);
            gQuest.store(GetPrivateProfileIntW(L"ESP",L"Quest",1,f.c_str())!=0);
            gLoot.store(GetPrivateProfileIntW(L"ESP",L"Loot",1,f.c_str())!=0);
            gMaxDistanceMeters.store(std::clamp(static_cast<int>(
                GetPrivateProfileIntW(L"ESP",L"MaxMeters",350,f.c_str())),25,750));
            gCryptidYawSensitivity.store(std::clamp(
                GetPrivateProfileIntW(L"CryptidCamera",L"YawMilli",120,f.c_str())*.001f,.015f,.60f));
            gCryptidPitchSensitivity.store(std::clamp(
                GetPrivateProfileIntW(L"CryptidCamera",L"PitchMilli",120,f.c_str())*.001f,.015f,.60f));
            gCryptidCameraFollow.store(std::clamp(
                static_cast<float>(GetPrivateProfileIntW(L"CryptidCamera",L"FollowRate",32,f.c_str())),
                8.f,72.f));
            gCryptidInvertPitch.store(
                GetPrivateProfileIntW(L"CryptidCamera",L"InvertPitch",0,f.c_str())!=0);
            gCryptidClearLocalBlur.store(
                GetPrivateProfileIntW(L"CryptidCamera",L"ClearBlurWhilePossessed",1,f.c_str())!=0);
            gMothmanReduceLocalSmoke.store(
                GetPrivateProfileIntW(L"Mothman",L"ReduceLocalSmoke",1,f.c_str())!=0);
        });
    }
    void SavePreferences() {
        const auto f=SettingsPath();
        if(f.empty()) return;
        WritePrivateProfileStringW(L"ESP",L"Enabled",gEsp.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Cryptids",gCryptids.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Players",gPlayers.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Wildlife",gWildlife.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Containers",gContainers.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Quest",gQuest.load()?L"1":L"0",f.c_str());
        WritePrivateProfileStringW(L"ESP",L"Loot",gLoot.load()?L"1":L"0",f.c_str());
        wchar_t buf[32]{};
        swprintf_s(buf,L"%d",gMaxDistanceMeters.load());
        WritePrivateProfileStringW(L"ESP",L"MaxMeters",buf,f.c_str());
    }
    uintptr_t FindFunction(const std::wstring&,const std::wstring&);
    bool Invoke(uintptr_t,uintptr_t,void*) noexcept;
    bool Position(uintptr_t actor, Vec3& loc) {
        // UE SDK Engine.Actor.K2_GetActorLocation returns FVector3d at param offset 0.
        // A root component's RelativeLocation is NOT a world position for attached actors.
        // Negative lookups can be expensive against a 49k-object GUObjectArray.
        // Retry no more than once every ten seconds if streaming is incomplete.
        static uintptr_t resolved=0;
        static ULONGLONG nextLookup=0;
        const auto tick=GetTickCount64();
        if(!resolved && tick>=nextLookup) {
            nextLookup=tick+10000;
            resolved=FindFunction(L"Actor",L"K2_GetActorLocation");
        }
        const auto f=resolved;
        if(f) {
            alignas(8) Vec3 returnValue{};
            if(Invoke(actor,f,&returnValue) &&
                std::isfinite(returnValue.x) && std::isfinite(returnValue.y) &&
                std::isfinite(returnValue.z) &&
                std::abs(returnValue.x)<1e9 && std::abs(returnValue.y)<1e9 &&
                std::abs(returnValue.z)<1e9) {
                loc=returnValue;return true;
            }
        }
        // Fallback for unusual non-reflected engine builds; not valid for attachments.
        const auto root=ReadPtr(actor+0x1B8);
        return root && Read(root+0x128,loc) &&
            std::isfinite(loc.x) && std::isfinite(loc.y) && std::isfinite(loc.z) &&
            std::abs(loc.x)<1e9 && std::abs(loc.y)<1e9 && std::abs(loc.z)<1e9;
    }
    std::vector<LevelView> GetWorldLevels(uintptr_t world,int& total) {
        std::vector<LevelView> out;
        total=0;
        uintptr_t levels=ReadPtr(world+0x178);
        int32_t n=0;
        if (!levels || !Read(world+0x180,n) || n<1 || n>128) return out;
        out.reserve(static_cast<size_t>(n));
        for(int i=0;i<n;++i) {
            uintptr_t level=ReadPtr(levels+static_cast<size_t>(i)*8);
            auto actors=ReadPtr(level+0xA0); // SDK ULevel::Actors
            int32_t count=0;
            if (!actors || !Read(level+0xA8,count) || count<1 || count>40000) continue;
            if (total+count>200000) break;
            out.push_back({actors,count,total});
            total+=count;
        }
        return out;
    }
    std::string PlayerLabel(uintptr_t pawn,int objIndex) {
        // PlayerCharacter_C::PlayerName, 0x8E0 is FString (UTF-16 on Windows).
        const auto text=ReadPtr(pawn+0x8E0);
        int32_t len=0;
        if(text && Read(pawn+0x8E8,len) && len>1 && len<=80) {
            wchar_t buf[81]{};
            bool good=true;
            for(int i=0;i<len && i<80;++i) {
                if (!Read(text+static_cast<size_t>(i)*sizeof(wchar_t),buf[i])) {good=false;break;}
            }
            if(good) {
                const auto chars=WideCharToMultiByte(CP_UTF8,0,buf,-1,nullptr,0,nullptr,nullptr);
                if(chars>1 && chars<256) {
                    std::string name(static_cast<size_t>(chars),'\0');
                    WideCharToMultiByte(CP_UTF8,0,buf,-1,name.data(),chars,nullptr,nullptr);
                    if(!name.empty() && name.back()=='\0')name.pop_back();
                    if(!name.empty())return name;
                }
            }
        }
        return "Hunter #"+std::to_string(objIndex);
    }
    void Tick() {
        gTickPending.store(false,std::memory_order_release);
        LoadPreferences();
        // Low-latency free-flight; keep expensive ESP/roster scans on their old cadence.
        FreeflightTick();
        const ULONGLONG scanTime=GetTickCount64();
        if(scanTime-gLastScanTick<kScanIntervalMs)return;
        gLastScanTick=scanTime;
        const uintptr_t world=World();
        if (gPrevWorld!=world) {
            // A previous possession can end during level travel. Once a
            // valid new world/player exists, restore the old local CVars.
            if(gPossessionClarity.active && world) {
                const auto gi=ReadPtr(world+0x1D8);
                const auto locals=ReadPtr(gi+0x38);
                const auto local=ReadPtr(locals);
                const auto pc=ReadPtr(local+0x30);
                if(pc)RestorePossessionClarity(pc);
            }
            gPrevWorld=world;
            gScanIndex=0;
            gKinds.clear();gClassNames.clear();gFunctionCache.clear();
            gKnown.clear();gKnownSpawners.clear();
            gSpawnClassCandidates.clear();gSpawnClassCatalog.clear();
            gSpawnPointCatalog.clear();gSpawnClassScanCursor=0;
            gSoundScanCursor=0;gSoundCandidates.clear();gCameraShakeCandidates.clear();
            gMonsterAttackSoundIndex.store(-1);
            gPhysicsBackups.clear();gFallBackups.clear();gGodBackups.clear();
            // Level transition invalidates actor pointers. Never restore input to
            // a destroyed body; clear references and capture state instead.
            gHumanPawn=0;gHumanIndex=-1;gPossessedCryptid=false;
            gHumanInputController=0;gHumanInputDisabled=false;
            gHumanIgnoreMovement=false;gHumanIgnoreLook=false;
            gMonsterModeSaved=false;
            gMonsterActive.store(false);gMonsterPawn=0;gChaseCamera=0;
            // Old UObjects are invalid after level transition. Do not call
            // render functions on them; the scene is destroyed by the engine.
            gLocalMothmanSmoke=LocalMothmanSmoke{};
            gMothRender=LocalMothmanRender{};
            gMothFlight=NativeMothmanFlight{};
            gCameraAutoManageSaved=false;
            gMonsterIndex=-1;gCameraIndex=-1;gLastMotionTick=0;
            gMonsterAIController=0;gMonsterBrain=0;gMonsterBrainWasRunning=false;
            gMonsterMovement=0;gMonsterSpeedsSaved=false;gPuppeteering=false;
            gGroundMovementSettings=GroundMovementSettings{};
            gGroundDrive=GroundMovementDrive{};
            gMonsterPathActive=false;gMonsterLastMoveRequest=0;
            gMonsterFunctions.clear();
            gVampireVerify=VampireInvisibilityVerification{};
            gControlledCryptidClass=0;
            gControlledRespawnSpawner=0;gControlledRespawnSpawnerIndex=-1;
            gControlledExistingIds.clear();
            gControlledRespawnStart=-1.0;gControlledRespawnRemaining=0.0;
            gControlledSpawnRequested=false;gNextControlledRespawnScan=0;
            gHumanHUD=0;gHumanHudHidden=false;gHudRestorePending=false;
            gChaseSmoothedValid=false;
            gGravityBackup=WorldGravityBackup{};
            gSpawnWave=SpawnWave{};
            gNoclipPawn=0;gNoclipEnabled=false;
            gNoclipPositionValid=false;gNoclipController=0;gNoclipIgnoredMove=false;
            FHJDiag::Log(L"World changed: reset ESP and player cache.");
        }
        AdvanceSpawnWave();
        if(world) {
            ScanLoadedSounds();
            RefreshSpawnableClassCatalog();
        }
        HunterNative::Status status{};
        status.pendingSpawnTriggers=gSpawnWave.remaining;
        status.confirmedSpawned=gSpawnWave.created;
        Camera cam{};
        std::vector<Marker> markers;
        std::vector<HunterNative::PlayerInfo> playersOut;
        std::vector<HunterNative::CreatureInfo> creaturesOut;
        std::vector<HunterNative::CryptidSpawnPoint> spawnPointsOut;
        status.worldReady=world!=0;
        if(world) {
            status.host=ReadPtr(world+0x158)!=0;
            const auto gameState=ReadPtr(world+0x160); // Engine.UWorld::GameState
            if(gameState && ClassName(ReadPtr(gameState+0x10))==L"FL_Gamestate_C") {
                const auto huntClass=ReadPtr(gameState+0x340); // FL_Gamestate_C::HuntData
                const auto hunt=ObjectName(huntClass);
                if(!hunt.empty())
                    status.activeHuntName.assign(hunt.begin(),hunt.end());
            }
            cam=GetCamera(world);
            status.cameraReady=cam.fov>=20.f;
            const auto gi=ReadPtr(world+0x1D8);
            const auto locals=ReadPtr(gi+0x38);
            const auto local=ReadPtr(locals);
            const auto pc=ReadPtr(local+0x30);
            const auto localPawn=ReadPtr(pc+0x350);
            if(localPawn) {
                Read(localPawn+0x8A0,status.localHealth);
                Read(localPawn+0x8A8,status.localMaxHealth);
                Read(localPawn+0x8B9,status.invuln);
                const auto god=gGodBackups.find(localPawn);
                status.localGodProtected=god!=gGodBackups.end();
            }
            int totalActors=0;
            const auto levels=GetWorldLevels(world,totalActors);
            status.levelsScanned=static_cast<int>(levels.size());
            status.totalObjects=totalActors;
            // Discover active ULevel::Actors directly, not every global UObject.
            // This fixes worlds where the global array contains no *live* actor of interest
            // in the current scan slice, or where stale UObject slots are common.
            const bool esp=gEsp.load(std::memory_order_relaxed);
            const int maxScan=esp ? 900 : 600; // quicker Mothman/cryptid roster discovery
            for(int i=0;i<maxScan && totalActors>0;++i) {
                if(gScanIndex>=totalActors)gScanIndex=0;
                const int slot=gScanIndex++;
                const auto found=std::lower_bound(levels.begin(),levels.end(),slot,
                    [](const LevelView& level,int index){return level.start+level.count<=index;});
                if(found==levels.end())continue;
                const auto actor=ReadPtr(found->data+static_cast<size_t>(slot-found->start)*8);
                if(!actor)continue;
                ++status.actorsExamined;
                int32_t objIndex=-1;
                if(!Read(actor+0xC,objIndex) || objIndex<0)continue;
                const auto cls=ReadPtr(actor+0x10);
                if(ClassName(cls)==L"BP_MobSpawner_C" &&
                   gKnownSpawners.size()<100 &&
                   std::none_of(gKnownSpawners.begin(),gKnownSpawners.end(),
                       [actor](const ActorEntry& e){return e.address==actor;}))
                    gKnownSpawners.push_back({actor,objIndex,7});
                const int kind=ClassKind(cls);
                if(kind && gKnown.size()<400 &&
                    std::none_of(gKnown.begin(),gKnown.end(),
                        [actor](const ActorEntry& e){return e.address==actor;}))
                    gKnown.push_back({actor,objIndex,kind});
            }
            status.scanned=gScanIndex;
            gKnown.erase(std::remove_if(gKnown.begin(),gKnown.end(),
                [](const ActorEntry& a){
                    int32_t actual=-1;
                    return !Read(a.address+0xC,actual) || actual!=a.index;
                }),gKnown.end());
            gKnownSpawners.erase(std::remove_if(gKnownSpawners.begin(),gKnownSpawners.end(),
                [](const ActorEntry& a) {
                    int32_t id=-1;
                    return !Read(a.address+0x0C,id) || id!=a.index ||
                        ClassName(ReadPtr(a.address+0x10))!=L"BP_MobSpawner_C";
                }),gKnownSpawners.end());
            status.activeMobSpawners=static_cast<int>(gKnownSpawners.size());
            std::vector<HunterNative::CryptidSpawnPoint> points;
            points.reserve(gKnownSpawners.size());
            for(const auto& point:gKnownSpawners) {
                // Avoid expensive ProcessEvent world-position reads on
                // every scan; a large map can have many mob spawners.
                char label[80]{};
                snprintf(label,sizeof(label),"Current-map spawner #%d",point.index);
                points.push_back({point.index,label});
            }
            spawnPointsOut=std::move(points);
            status.knownActors=static_cast<int>(gKnown.size());
            markers.reserve(kMaxActors);
            for(const auto& actor:gKnown) {
                if(actor.kind==1) {
                    ++status.activePlayers;
                    HunterNative::PlayerInfo player;
                    player.objectIndex=actor.index;
                    player.displayName=PlayerLabel(actor.address,actor.index);
                    player.local=actor.address==localPawn;
                    Read(actor.address+0x8A0,player.health);
                    Read(actor.address+0x8B8,player.alive);
                    Read(actor.address+0x8B9,player.invulnerable);
                    playersOut.push_back(std::move(player));
                } else if(actor.kind==2) {
                    ++status.activeCryptids;
                    HunterNative::CreatureInfo creature;
                    creature.objectIndex=actor.index;
                    auto cls=ReadPtr(actor.address+0x10);
                    creature.isPawn=IsPawnClass(cls);
                    const auto name=ClassName(cls);
                    if(name==L"BP_FlyingPawnTest_C") {
                        // This is the Mothman pawn from the exact SDK. Its
                        // 0x710/0x718 bytes are NOT Health/MaxHealth.
                        creature.health=-1.0;creature.maxHealth=-1.0;
                        creature.displayName="Mothman [alive / HP unavailable]";
                    } else {
                        // Health only belongs to BP_Cryptid_Base_C-derived actors.
                        if(!Read(actor.address+0x710,creature.health) ||
                           !std::isfinite(creature.health) || creature.health<0)
                            creature.health=-1.0;
                        if(!Read(actor.address+0x718,creature.maxHealth) ||
                           !std::isfinite(creature.maxHealth) || creature.maxHealth<=0)
                            creature.maxHealth=-1.0;
                        creature.displayName=std::string(name.begin(),name.end());
                    }
                    creaturesOut.push_back(std::move(creature));
                } else if(actor.kind==3) ++status.activeWildlife;
                else if(actor.kind==4) ++status.activeContainers;
                else if(actor.kind==5) ++status.activeQuestObjects;
                else if(actor.kind==6) ++status.activeLoot;
                if(!esp || !status.cameraReady ||
                    (actor.kind==1 && !gPlayers.load()) ||
                    (actor.kind==2 && !gCryptids.load()) ||
                    (actor.kind==3 && !gWildlife.load()) ||
                    (actor.kind==4 && !gContainers.load()) ||
                    (actor.kind==5 && !gQuest.load()) ||
                    (actor.kind==6 && !gLoot.load()) ||
                    markers.size()>=kMaxActors)continue;
                Vec3 loc{};
                if(!Position(actor.address,loc))continue;
                const double distance=std::hypot(std::hypot(loc.x-cam.pos.x,
                    loc.y-cam.pos.y),loc.z-cam.pos.z);
                if(distance>gMaxDistanceMeters.load()*100.0)continue;
                double actorHealth=-1.0,actorMaxHealth=-1.0;
                if(actor.kind==2) {
                    // Never read BP_Cryptid_Base health offsets from Mothman.
                    if(ClassName(ReadPtr(actor.address+0x10))!=L"BP_FlyingPawnTest_C") {
                        if(!Read(actor.address+0x710,actorHealth) ||
                           !std::isfinite(actorHealth) || actorHealth<0)actorHealth=-1.0;
                        if(!Read(actor.address+0x718,actorMaxHealth) ||
                           !std::isfinite(actorMaxHealth) || actorMaxHealth<=0)actorMaxHealth=-1.0;
                    }
                } else if(actor.kind==1) {
                    if(!Read(actor.address+0x8A0,actorHealth) ||
                       !std::isfinite(actorHealth) || actorHealth<0)actorHealth=-1.0;
                    if(!Read(actor.address+0x8A8,actorMaxHealth) ||
                       !std::isfinite(actorMaxHealth) || actorMaxHealth<=0)actorMaxHealth=-1.0;
                }
                markers.push_back({loc,actor.kind,actorHealth,actorMaxHealth,
                    actor.kind==2 &&
                    ClassName(ReadPtr(actor.address+0x10))==L"BP_FlyingPawnTest_C"});
            }
            status.espCandidates=static_cast<int>(markers.size());
            status.fallProtectionEnabled=gProtectFall.load();
            for(const auto& pair:gFallBackups)
                if(pair.second.verified)++status.fallProtectedPlayers;
            status.godProtectedPlayers=static_cast<int>(gGodBackups.size());
            static ULONGLONG lastPhysicsApply=0;
            const auto nowPhysics=GetTickCount64();
            const bool protecting=gProtectFall.load() || !gGodBackups.empty();
            const ULONGLONG interval=protecting?110:1500;
            if(nowPhysics-lastPhysicsApply>=interval) {
                lastPhysicsApply=nowPhysics;
                if(gProtectFall.load() || !gFallBackups.empty() ||
                   gSpeedMultiplier.load()!=1.f || gJumpMultiplier.load()!=1.f)
                    ReapplyMovementForNewPlayers();
                if(!gGodBackups.empty())
                    MaintainGodProtection();
            }
        }
        std::vector<VanityChoice> vanity;
        // FL_PlayerState_C::BP_CosmeticsManager +0x368 and
        // BP_CosmeticsManager_C::AllCosmeticItems +0xF8 (TArray<UClass*>).
        if(world) {
            const auto pawn=LocalPawn();
            const auto playerState=ReadPtr(pawn+0x2B0); // APawn::PlayerState
            if(ClassName(ReadPtr(playerState+0x10))==L"FL_PlayerState_C") {
                const auto manager=ReadPtr(playerState+0x368);
                if(manager && ClassName(ReadPtr(manager+0x10))==L"BP_CosmeticsManager_C") {
                    const auto data=ReadPtr(manager+0xF8);
                    int32_t count=0;
                    uintptr_t equipped=ReadPtr(manager+0xC8);
                    if(data && Read(manager+0x100,count) && count>=0 && count<=256) {
                        for(int j=0;j<count;++j) {
                            const auto klass=ReadPtr(data+static_cast<size_t>(j)*8);
                            if(!klass)continue;
                            auto name=ObjectName(klass);
                            if(name.empty())continue;
                            VanityChoice item{klass,std::string(name.begin(),name.end()),klass==equipped};
                            vanity.push_back(std::move(item));
                        }
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gStatus=status;gCamera=cam;gMarkers=std::move(markers);
            gRoster=std::move(playersOut);
            gCreatures=std::move(creaturesOut);
            gSpawnClassCatalog=gSpawnClassCandidates;
            // A loaded UClass alone isn't a playable actor. Mark whether
            // that exact species is represented by a live ULevel actor in
            // this map. A foreign-class spawn remains experimental.
            for(auto& spawnType:gSpawnClassCatalog) {
                spawnType.presentAsLiveActor=false;
                int count=0;uintptr_t chunks=0;
                if(!GetObjects(count,chunks) ||
                   spawnType.objectIndex>=count)continue;
                const auto wanted=ObjectAt(spawnType.objectIndex,chunks);
                for(const auto& actor:gKnown) {
                    if(actor.kind==2 &&
                       ReadPtr(actor.address+0x10)==wanted) {
                        spawnType.presentAsLiveActor=true;
                        break;
                    }
                }
            }
            gSpawnPointCatalog=std::move(spawnPointsOut);
            gVanityChoices=std::move(vanity);
            gSounds=gSoundCandidates;
            gCameraShakes=gCameraShakeCandidates;
        }
        static ULONGLONG lastReport=0;
        const auto now=GetTickCount64();
        if(now-lastReport>6500 && world) {
            lastReport=now;
            wchar_t out[300]{};
            swprintf_s(out,L"Scanner: levels=%d, live actors=%d, discovered=%d, cryptids=%d, players=%d, camera=%s, ESP candidates=%d",
                status.levelsScanned,status.totalObjects,status.knownActors,
                status.activeCryptids,status.activePlayers,
                status.cameraReady?L"ready":L"missing",status.espCandidates);
            FHJDiag::Log(out);
        }
    }
    bool Project(const Vec3& point,const Camera& cam,ImVec2 display,ImVec2& result,double& meters) {
        const double deltaX=point.x-cam.pos.x,deltaY=point.y-cam.pos.y,deltaZ=point.z-cam.pos.z;
        meters=std::hypot(std::hypot(deltaX,deltaY),deltaZ)/100.0;
        const double yaw=cam.rot.yaw*kPi/180.0,pitch=cam.rot.pitch*kPi/180.0;
        const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
        const double front=deltaX*cp*cy+deltaY*cp*sy+deltaZ*sp;
        if(front<10.0) return false;
        const double right=-deltaX*sy+deltaY*cy;
        const double up=-deltaX*sp*cy-deltaY*sp*sy+deltaZ*cp;
        const double focal=(display.x*.5)/std::tan(cam.fov*.5*kPi/180.0);
        result.x=static_cast<float>(display.x*.5+right*focal/front);
        result.y=static_cast<float>(display.y*.5-up*focal/front);
        return std::isfinite(result.x)&&std::isfinite(result.y) &&
            result.x>=8 && result.y>=8 && result.x<display.x-8 && result.y<display.y-8;
    }
    uintptr_t FindFunction(const std::wstring& outerName,const std::wstring& name) {
        const std::wstring key=outerName+L"." + name;
        if (auto it=gFunctionCache.find(key);it!=gFunctionCache.end()) return it->second;
        int count=0;uintptr_t chunks=0;
        if (!GetObjects(count,chunks)) return 0;
        for (int i=0;i<count;++i) {
            uintptr_t obj=ObjectAt(i,chunks);
            if (!obj) continue;
            uintptr_t cls=ReadPtr(obj+0x10);
            if (ClassName(cls)!=L"Function") continue;
            if (ObjectName(obj)!=name) continue;
            uintptr_t outer=ReadPtr(obj+0x20);
            if (outer && ObjectName(outer)==outerName) {
                gFunctionCache.emplace(key,obj);
                return obj;
            }
        }
        return 0;
    }
    bool Invoke(uintptr_t target,uintptr_t func,void* params) noexcept {
        if (!target || !func) return false;
        uintptr_t vt=0,call=0;
        if (!Read(target,vt)||!Read(vt+PROCESS_EVENT_INDEX*sizeof(uintptr_t),call)||!call) return false;
        using PE=void(*)(void*,void*,void*);
        __try { reinterpret_cast<PE>(call)(reinterpret_cast<void*>(target),reinterpret_cast<void*>(func),params); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }
    uintptr_t FindItemClass(const std::string& className) {
        // Query the real UE class object, not a user-supplied memory address.
        int count=0; uintptr_t chunks=0;
        if (className.rfind("Item_",0)!=0 || className.size()>96 ||
            !GetObjects(count,chunks)) return 0;
        std::wstring wanted(className.begin(),className.end());
        for (int i=0;i<count;++i) {
            auto obj=ObjectAt(i,chunks);
            if (!obj) continue;
            const auto meta=ClassName(ReadPtr(obj+0x10));
            if (meta!=L"BlueprintGeneratedClass" && meta!=L"Class") continue;
            if (ObjectName(obj)!=wanted) continue;
            // Verify inheritance through UStruct::SuperStruct.
            auto cls=obj;
            for (int depth=0;depth<20 && cls;++depth) {
                const auto current=ObjectName(cls);
                if (current==L"ItemData_C") return obj;
                if (current==L"Item_VanityCosmeticBase_C") return obj;
                auto base=ReadPtr(cls+0x40);
                if (!base || base==cls) break;
                cls=base;
            }
        }
        return 0;
    }
    uintptr_t SelectedPawn(int index) {
        if (index<0) return 0;
        // The host can control their own pawn even before the level scanner has
        // populated the roster or when the local pawn isn't in the current scan.
        const auto localWorld=World();
        const auto gi=ReadPtr(localWorld+0x1D8), locals=ReadPtr(gi+0x38);
        const auto local=ReadPtr(locals), pc=ReadPtr(local+0x30);
        const auto localPawn=ReadPtr(pc+0x350);
        int32_t localIndex=-1;
        if(localPawn && Read(localPawn+0xC,localIndex) && localIndex==index) return localPawn;
        for(const auto& e:gKnown) {
            if(e.kind==1 && e.index==index) {
                int32_t liveIndex=-1;
                if(Read(e.address+0xC,liveIndex) && liveIndex==index) return e.address;
            }
        }
        return 0;
    }
    uintptr_t LocalPawn() {
        const auto world=World(),gi=ReadPtr(world+0x1D8);
        const auto locals=ReadPtr(gi+0x38), local=ReadPtr(locals);
        const auto pc=ReadPtr(local+0x30);
        return ReadPtr(pc+0x350);
    }
    uintptr_t LocalInventory() {
        const auto world=World();
        const auto instance=ReadPtr(world+0x1D8);
        const auto players=ReadPtr(instance+0x38);
        int32_t count=0;
        if (!players || !Read(instance+0x40,count) || count<1 || count>8) return 0;
        const auto local=ReadPtr(players);
        const auto controller=ReadPtr(local+0x30);
        const auto pawn=ReadPtr(controller+0x350);
        if (!pawn) return 0;
        return ReadPtr(pawn+0x720); // PlayerCharacter_C::GridInventoryComponent (SDK)
    }
    bool GiveCatalogItem(const Action& a) {
        // Keep item identifiers out of the user interface, but include them in diagnostics.
        const std::wstring displayName(a.item.begin(),a.item.end());
        const std::wstring requestLog=L"Catalog spawn: " + displayName + L" x" + std::to_wstring(a.amount);
        FHJDiag::Log(requestLog.c_str());
        const uintptr_t itemClass=FindItemClass(a.item);
        if (!itemClass) {
            FHJDiag::Log(L"Catalog item class is not currently loaded or does not inherit ItemData_C.");
            return false;
        }
        // Host may request an item on a selected character's inventory.
        // In non-host sessions only the owning local inventory is used.
        const auto world=World();
        const bool host=ReadPtr(world+0x158)!=0;
        const uintptr_t target=(a.target>=0 && host) ? SelectedPawn(a.target) : LocalPawn();
        const uintptr_t inventory=target ? ReadPtr(target+0x720) : 0;
        if (!inventory) {
            FHJDiag::Log(L"Catalog item grant unavailable: local GridInventoryComponent not found.");
            return false;
        }
        const uintptr_t request=FindFunction(L"GridInventoryComponent_C",L"AskServer_AddItem");
        if (!request) {
            FHJDiag::Log(L"Catalog item grant unavailable: AskServer_AddItem not found in reflection.");
            return false;
        }
        // Params::GridInventoryComponent_C_AskServer_AddItem: size 0x28
        // UClass* at +0x00, Amount at +0x08, JSON metadata at +0x10, bool at +0x20.
        alignas(8) unsigned char args[0x28]{};
        std::memcpy(args,&itemClass,sizeof(itemClass));
        std::memcpy(args+0x08,&a.amount,sizeof(a.amount));
        args[0x20]=1; // prefer quick-access hotbar if space exists
        if (!Invoke(inventory,request,args)) {
            FHJDiag::Log(L"Item grant RPC could not be dispatched.");
            return false;
        }
        FHJDiag::Log(L"Item grant dispatched through owned inventory AskServer_AddItem. Verify server acceptance in-game.");
        return true;
    }
    // Access the weather tool owned by the map's active sky actor when possible.
    uintptr_t FindWeatherActor() {
        int total=0;
        const auto levels=GetWorldLevels(World(),total);
        for(const auto& level:levels)for(int i=0;i<level.count;++i) {
            const auto object=ReadPtr(level.data+static_cast<size_t>(i)*8);
            if(!object)continue;
            const auto klass=ClassName(ReadPtr(object+0x10));
            if(klass==L"Ultra_Dynamic_Sky_BP_C") {
                const auto tool=ReadPtr(object+0x678); // Ultra_Dynamic_Sky_BP_C::WeatherTool
                if(tool && ClassName(ReadPtr(tool+0x10))==L"BP_WeatherTool_C")return tool;
            }
        }
        for(const auto& level:levels)for(int i=0;i<level.count;++i) {
            const auto object=ReadPtr(level.data+static_cast<size_t>(i)*8);
            if(object && ClassName(ReadPtr(object+0x10))==L"BP_WeatherTool_C")
                return object;
        }
        return 0;
    }
    bool ExecuteWeather(const Action& a) {
        const auto actor=FindWeatherActor();
        if(!actor) {
            FHJDiag::Log(L"WEATHER: No live BP_WeatherTool_C in current UWorld/sky. No effect can be applied here.");
            return false;
        }
        const bool rain=a.type==ActionType::Rain;
        const auto func=FindFunction(L"BP_WeatherTool_C",
            rain?L"Set Overall Rain Intensity":L"Set Overall Snow Intensity");
        if(!func) {
            FHJDiag::Log(L"WEATHER: expected Set Overall Rain/Snow Intensity UFunction not available.");
            return false;
        }
        const double intensity=std::clamp(static_cast<double>(a.value),0.0,1.0);
        // Unreal native SDK confirms these are separate local enable flags
        // and double-precision intensity inputs. Neither is Net replicated.
        const auto flag=actor+(rain?0x300:0x390);
        const bool enabled=intensity>0.001;
        alignas(8) uint8_t args[0x30]{};
        std::memcpy(args,&intensity,sizeof(intensity));
        if(!Invoke(actor,func,args)) {
            FHJDiag::Log(L"WEATHER: UFunction invocation failed.");
            return false;
        }
        // The setter may turn the effect back on internally. Apply its enable
        // flag *after* the setter, reconfigure particles on both enable/clear,
        // then assert the final state after setup (which can rewrite flags).
        const bool changed=TryWrite(flag,enabled);
        const auto setup=FindFunction(L"BP_WeatherTool_C",L"Weather Setup Function");
        if(setup)Invoke(actor,setup,nullptr);
        if(!enabled)TryWrite(flag,false);
        bool readFlag=!enabled;
        const bool verified=Read(flag,readFlag) && readFlag==enabled;
        FHJDiag::Log(changed && verified ?
            (enabled?L"WEATHER: native intensity set and enable flag verified.":
                     L"WEATHER: intensity zeroed, particle setup refreshed, enable flag cleared.") :
            L"WEATHER: could not verify final Enable_Rain/Snow flag after native setter.");
        return changed && verified;
    }
    bool ExecuteClearWeather() {
        const auto actor=FindWeatherActor();
        if(!actor) {
            FHJDiag::Log(L"WEATHER CLEAR: active BP_WeatherTool_C not found.");return false;
        }
        const auto rain=FindFunction(L"BP_WeatherTool_C",L"Set Overall Rain Intensity");
        const auto snow=FindFunction(L"BP_WeatherTool_C",L"Set Overall Snow Intensity");
        if(!rain || !snow) {
            FHJDiag::Log(L"WEATHER CLEAR: one or both native intensity setters missing.");return false;
        }
        alignas(8) uint8_t args[0x30]{};
        const double zero=0.0;
        std::memcpy(args,&zero,sizeof(zero));
        const bool rainSent=Invoke(actor,rain,args);
        const bool snowSent=Invoke(actor,snow,args);
        // Atomically clear both toggles after both setters. The native setup
        // may revive a weather flag; assert OFF again afterwards.
        const bool a=TryWrite(actor+0x300,false);
        const bool b=TryWrite(actor+0x390,false);
        const auto setup=FindFunction(L"BP_WeatherTool_C",L"Weather Setup Function");
        if(setup)Invoke(actor,setup,nullptr);
        TryWrite(actor+0x300,false);
        TryWrite(actor+0x390,false);
        bool rainEnabled=true,snowEnabled=true;
        const bool verified=Read(actor+0x300,rainEnabled) &&
            Read(actor+0x390,snowEnabled) && !rainEnabled && !snowEnabled;
        FHJDiag::Log(rainSent && snowSent && a && b && verified ?
            L"WEATHER CLEAR: rain/snow intensity zeroed and BOTH enable flags verified off.":
            L"WEATHER CLEAR: native requests sent but reset incomplete; inspect live WeatherTool.");
        return rainSent && snowSent && a && b && verified;
    }
    bool IsVampire(uintptr_t actor) {
        auto klass=ReadPtr(actor+0x10);
        for(int i=0;i<25 && klass;++i) {
            if(ClassName(klass)==L"BP_VampireBase_C")return true;
            auto parent=ReadPtr(klass+0x40);
            if(!parent || parent==klass)break;
            klass=parent;
        }
        return false;
    }
    bool TriggerVampireScream() {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"VAMPIRE: authoritative host required.");return false;
        }
        const auto fn=FindFunction(L"BP_VampireBase_C",L"AUTH_Scream");
        if(!fn) {FHJDiag::Log(L"VAMPIRE: native AUTH_Scream UFunction not found.");return false;}
        int total=0;
        const auto levels=GetWorldLevels(World(),total);
        for(const auto& level:levels)for(int i=0;i<level.count;++i) {
            auto actor=ReadPtr(level.data+static_cast<size_t>(i)*8);
            if(!actor || !IsVampire(actor))continue;
            if(Invoke(actor,fn,nullptr)) {
                FHJDiag::Log(L"VAMPIRE: AUTH_Scream invoked on live vampire actor. Check actual sound and player effect.");
                return true;
            }
        }
        FHJDiag::Log(L"VAMPIRE: no live vampire character found in this level. Cannot produce native scream.");
        return false;
    }
    bool TriggerFlashlightPrank(bool cancel) {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"FLASHLIGHT: host authority required.");return false;
        }
        const auto multicast=FindFunction(L"FlashlightManager_C",
            cancel?L"All_CancelFlicker":L"All_FlickerFlashlight");
        const auto force=FindFunction(L"FlashlightManager_C",L"ForceFlicker");
        if(!multicast && !force) {
            FHJDiag::Log(L"FLASHLIGHT: neither multicast nor ForceFlicker UFunction exists.");
            return false;
        }
        int count=0;
        auto apply=[&](uintptr_t pawn) {
            const auto manager=ReadPtr(pawn+0x710); // PlayerCharacter_C.FlashlightManager
            if(!manager || ClassName(ReadPtr(manager+0x10))!=L"FlashlightManager_C")return;
            bool result=false;
            if(force) {
                // SDK: bool StartFlicker; bool ReduceIntensity; 6 pad; double NewIntensity
                alignas(8) uint8_t params[0x28]{};
                params[0]=cancel?0:1;
                params[1]=cancel?0:1;
                const double intensity=cancel?1.0:0.4;
                std::memcpy(params+8,&intensity,8);
                result|=Invoke(manager,force,params);
            }
            if(multicast) result|=Invoke(manager,multicast,nullptr);
            if(result)++count;
        };
        apply(LocalPawn());
        for(const auto& actor:gKnown) {
            if(actor.kind!=1 || actor.address==LocalPawn())continue;
            apply(actor.address);
        }
        wchar_t message[200]{};
        swprintf_s(message,L"FLASHLIGHT: invoked native component flicker on %d players (network visibility unverified).",count);
        FHJDiag::Log(message);
        return count>0;
    }
    void ExecuteClientCameraShake(const Action& a) {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"CLIENT SCARE: authoritative host required.");return;
        }
        int count=0;uintptr_t chunks=0;
        if(!GetObjects(count,chunks) || a.amount<0 || a.amount>=count)return;
        const auto shake=ObjectAt(a.amount,chunks);
        int32_t actual=-1;
        const auto metaclass=ClassName(ReadPtr(shake+0x10));
        if(!shake || !Read(shake+0x0C,actual) || actual!=a.amount ||
           (metaclass!=L"Class" && metaclass!=L"BlueprintGeneratedClass") ||
           !IsCameraShakeClass(shake)) {
            FHJDiag::Log(L"CLIENT SCARE: selected camera shake subclass not loaded.");return;
        }
        const auto fn=FindFunction(L"PlayerController",L"ClientStartCameraShake");
        if(!fn) {
            FHJDiag::Log(L"CLIENT SCARE: native ClientStartCameraShake RPC missing.");return;
        }
        std::vector<uintptr_t> controllers;
        const auto add=[&](uintptr_t pawn) {
            const auto pc=ReadPtr(pawn+0x2D8);
            if(pc && std::find(controllers.begin(),controllers.end(),pc)==controllers.end())
                controllers.push_back(pc);
        };
        if(a.target>=0) {
            const auto pawn=SelectedPawn(a.target);
            if(pawn)add(pawn);
        } else {
            add(LocalPawn());
            for(const auto& player:gKnown)if(player.kind==1)add(player.address);
        }
        int sent=0;
        for(const auto pc:controllers) {
            // UE5.5 ClientStartCameraShake(TSubclassOf, float Scale,
            // ECameraShakePlaySpace, FRotator). FRotator is 3 doubles.
            alignas(8) uint8_t args[0x30]{};
            const float intensity=std::clamp(a.value,0.1f,2.f);
            std::memcpy(args,&shake,sizeof(shake));
            std::memcpy(args+8,&intensity,sizeof(intensity));
            args[0xC]=0; // CameraLocal; zeroed UserPlaySpaceRot
            if(Invoke(pc,fn,args))++sent;
        }
        wchar_t msg[200]{};
        swprintf_s(msg,L"CLIENT SCARE: native camera shake sent to %d/%d selected controllers; playback awaits multiplayer test.",
            sent,static_cast<int>(controllers.size()));
        FHJDiag::Log(msg);
    }
    void ExecuteClientSound(const Action& a) {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"CLIENT AUDIO: authoritative session host required.");return;
        }
        int count=0;uintptr_t chunks=0;
        if(!GetObjects(count,chunks) || a.amount<0 || a.amount>=count) {
            FHJDiag::Log(L"CLIENT AUDIO: invalid sound object index.");return;
        }
        const auto asset=ObjectAt(a.amount,chunks);
        int32_t liveIndex=-1;
        if(!asset || !Read(asset+0x0C,liveIndex) || liveIndex!=a.amount ||
           !IsSoundAssetClass(ReadPtr(asset+0x10))) {
            FHJDiag::Log(L"CLIENT AUDIO: sound asset unloaded or not USoundBase.");return;
        }
        const auto clientPlay=FindFunction(L"PlayerController",L"ClientPlaySound");
        if(!clientPlay) {
            FHJDiag::Log(L"CLIENT AUDIO: native PlayerController.ClientPlaySound RPC unavailable.");return;
        }
        std::vector<uintptr_t> targets;
        const auto addPawn=[&](uintptr_t pawn) {
            const auto controller=ReadPtr(pawn+0x2D8);
            if(controller &&
                std::find(targets.begin(),targets.end(),controller)==targets.end())
                targets.push_back(controller);
        };
        if(a.target>=0) {
            const auto pawn=SelectedPawn(a.target);
            if(pawn)addPawn(pawn);
        } else {
            addPawn(LocalPawn());
            for(const auto& actor:gKnown)if(actor.kind==1)addPawn(actor.address);
        }
        if(targets.empty()) {
            FHJDiag::Log(L"CLIENT AUDIO: no live selected/roster player controller.");return;
        }
        int dispatched=0;
        for(const auto controller:targets) {
            // UE5.5 native ClientPlaySound(USoundBase*,float,float) is
            // an owning-client RPC: server sends to each chosen controller.
            alignas(8) uint8_t args[0x10]{};
            const float volume=std::clamp(a.value,0.05f,2.0f);
            const float pitch=1.0f;
            std::memcpy(args,&asset,sizeof(asset));
            std::memcpy(args+8,&volume,sizeof(volume));
            std::memcpy(args+12,&pitch,sizeof(pitch));
            if(Invoke(controller,clientPlay,args))++dispatched;
        }
        wchar_t msg[230]{};
        swprintf_s(msg,L"CLIENT AUDIO: dispatched PlayerController.ClientPlaySound on %d/%d controller(s), USoundBase index %d. Client playback needs in-game verification.",
            dispatched,static_cast<int>(targets.size()),a.amount);
        FHJDiag::Log(msg);
    }
    void ExecuteTargetedAction(const Action& a) {
        const auto world=World();
        if(!world || !ReadPtr(world+0x158)) {
            FHJDiag::Log(L"Targeted server action requires an authoritative host.");
            return;
        }
        const uintptr_t pawn=SelectedPawn(a.target);
        if(!pawn) {
            FHJDiag::Log(L"Selected player is no longer present; action skipped.");
            return;
        }
        if (a.type==ActionType::TargetRevive) {
            const auto f=FindFunction(L"PlayerCharacter_C",L"Delayed_Resurrect");
            if (!f) {FHJDiag::Log(L"Delayed_Resurrect not found in live SDK.");return;}
            FHJDiag::Log(Invoke(pawn,f,nullptr) ?
                L"Revive dispatched on target; check IsAlive and health after game update." :
                L"Revive dispatcher rejected target.");
            return;
        }
        if (a.type==ActionType::TargetKill) {
            auto f=FindFunction(L"PlayerCharacter_C",L"Server_KillSelf");
            if (!f) {FHJDiag::Log(L"Server_KillSelf not found in live SDK.");return;}
            FHJDiag::Log(Invoke(pawn,f,nullptr) ?
                L"Kill command invoked on target pawn; verify on host." :
                L"Kill command invocation failed.");
            return;
        }
        if (a.type==ActionType::TargetGodMode) {
            // On the host, set the authoritative Net property directly.
            // Dispatching NetServer on a remote pawn does not guarantee an update.
            ApplyGodProtection(pawn,a.amount!=0);
            return;
        }
        if (a.type==ActionType::TargetKick) {
            const auto controller=ReadPtr(pawn+0x2D8); // APawn::Controller
            if(!controller || controller==ReadPtr(LocalPawn()+0x2D8)) {
                FHJDiag::Log(L"Kick blocked: target controller unavailable or is the host.");
                return;
            }
            // FText must be constructed by the engine, not zero-initialized.
            const auto textFunction=FindFunction(L"KismetTextLibrary",L"Conv_StringToText");
            const auto kickFunction=FindFunction(L"FL_Gamemode_C",L"Server_Kick");
            if(!textFunction || !kickFunction) {
                FHJDiag::Log(L"Kick unavailable: FText conversion or Server_Kick function missing.");
                return;
            }
            uintptr_t textDefault=0;
            int objCount=0;uintptr_t chunks=0;
            if(GetObjects(objCount,chunks)) {
                for(int i=0;i<objCount;++i) {
                    auto candidate=ObjectAt(i,chunks);
                    if(candidate && ObjectName(candidate)==L"Default__KismetTextLibrary") {
                        textDefault=candidate;break;
                    }
                }
            }
            if(!textDefault) {
                FHJDiag::Log(L"Kick unavailable: KismetTextLibrary default object not loaded.");
                return;
            }
            wchar_t reasonText[]=L"Removed by the host";
            FStringView reason{reasonText,static_cast<int32_t>(std::size(reasonText)),static_cast<int32_t>(std::size(reasonText))};
            alignas(8) uint8_t textArgs[0x20]{};
            std::memcpy(textArgs,&reason,sizeof(reason));
            if(!Invoke(textDefault,textFunction,textArgs)) {
                FHJDiag::Log(L"Kick failed: could not create native FText reason.");
                return;
            }
            const auto gameMode=ReadPtr(world+0x158);
            alignas(8) uint8_t kickArgs[0x20]{};
            std::memcpy(kickArgs+0x8,&controller,sizeof(controller));
            std::memcpy(kickArgs+0x10,textArgs+0x10,0x10); // engine-created FText
            FHJDiag::Log(Invoke(gameMode,kickFunction,kickArgs) ?
                L"Host kick sent for selected controller. Confirm player disconnected." :
                L"Host kick ProcessEvent failed.");
            return;
        }
        if(a.type==ActionType::TargetHealth) {
            const auto f=FindFunction(L"PlayerCharacter_C",L"AskServer_SetHealth");
            if(!f) {FHJDiag::Log(L"AskServer_SetHealth not found.");return;}
            alignas(8) unsigned char args[0x10]{};
            std::memcpy(args,&a.precise,sizeof(double));
            FHJDiag::Log(Invoke(pawn,f,args) ?
                L"Health request dispatched on selected character; verify actual health/replication." :
                L"Health request dispatch failed.");
        } else if(a.type==ActionType::TargetNoise) {
            const auto f=FindFunction(L"PlayerCharacter_C",L"Server_MakeNoise");
            if(!f) {FHJDiag::Log(L"Server_MakeNoise not found.");return;}
            alignas(8) unsigned char args[0x20]{};
            const double rangeCm=static_cast<double>(a.value)*100.0;
            std::memcpy(args,&a.precise,sizeof(double));
            std::memcpy(args+8,&rangeCm,sizeof(double));
            FHJDiag::Log(Invoke(pawn,f,args) ?
                L"Noise event dispatched for selected character; resulting AI reaction unverified." :
                L"Noise event dispatch failed.");
        }
    }
    void ExecuteNoclip(const bool enabled) {
        const auto world=World();
        if(!world) return;
        const auto pawn=LocalPawn();
        if(!pawn) {FHJDiag::Log(L"Noclip requires a live local player pawn.");return;}
        const auto movement=ReadPtr(pawn+0x330); // SDK ACharacter::CharacterMovement
        if(!movement) {FHJDiag::Log(L"Noclip: CharacterMovement missing.");return;}
        const auto moveFunc=FindFunction(L"CharacterMovementComponent",L"SetMovementMode");
        const auto collisionFunc=FindFunction(L"Actor",L"SetActorEnableCollision");
        if(!moveFunc || !collisionFunc) {
            FHJDiag::Log(L"Noclip: engine movement/collision UFunctions unavailable.");return;
        }
        if(enabled) {
            if(!gNoclipEnabled || gNoclipPawn!=pawn) {
                Read(movement+0x221,gPriorMovement);
                uint8_t flags=0;Read(pawn+0x5C,flags);
                gPriorCollision=(flags&0x80)!=0;
                if(!Position(pawn,gNoclipPosition)) {
                    FHJDiag::Log(L"NOCLIP: failed to capture current location; free-flight not enabled.");
                    return;
                }
                gNoclipPositionValid=true;
                gNoclipPawn=pawn;gNoclipEnabled=true;
                gOriginalFlyMovement=movement;
                const auto gi=ReadPtr(world+0x1D8);
                const auto locals=ReadPtr(gi+0x38);
                const auto local=ReadPtr(locals);
                gNoclipController=ReadPtr(local+0x30);
                const auto ignore=FindFunction(L"Controller",L"SetIgnoreMoveInput");
                if(ignore && gNoclipController) {
                    bool preventGameInput=true;
                    gNoclipIgnoredMove=Invoke(gNoclipController,ignore,&preventGameInput);
                }
                const auto stop=FindFunction(L"MovementComponent",L"StopMovementImmediately");
                if(stop)Invoke(movement,stop,nullptr);
                gFlySpeedSaved=Read(movement+0x274,gOriginalFlySpeed);
                if(gFlySpeedSaved) {
                    const float speed=1150.f;
                    TryWrite(movement+0x274,speed); // SDK MaxFlySpeed
                }
            }
        } else if(gNoclipPawn!=pawn) {
            // Do not touch a stale actor after respawn.
            gNoclipEnabled=false;gNoclipPawn=0;
            gNoclipPositionValid=false;gNoclipIgnoredMove=false;gNoclipController=0;
            gFlySpeedSaved=false;gOriginalFlyMovement=0;
            FHJDiag::Log(L"Noclip: original pawn was replaced; cleared stale free-fly state.");return;
        }
        alignas(8) unsigned char moveArgs[0x8]{};
        moveArgs[0]= enabled?5:gPriorMovement; // UE MOVE_Flying=5; restore prior mode
        moveArgs[1]=0;
        bool canCollide=enabled?false:gPriorCollision;
        const bool modeInvoked=Invoke(movement,moveFunc,moveArgs);
        const bool collisionInvoked=Invoke(pawn,collisionFunc,&canCollide);
        if(enabled && (!modeInvoked || !collisionInvoked)) {
            uint8_t restore[2]{gPriorMovement,0};
            Invoke(movement,moveFunc,restore);
            bool previousCollision=gPriorCollision;
            Invoke(pawn,collisionFunc,&previousCollision);
            if(gFlySpeedSaved)TryWrite(movement+0x274,gOriginalFlySpeed);
            gNoclipEnabled=false;gNoclipPawn=0;gNoclipPositionValid=false;
            if(gNoclipIgnoredMove && gNoclipController) {
                const auto allow=FindFunction(L"Controller",L"SetIgnoreMoveInput");
                if(allow){bool no=false;Invoke(gNoclipController,allow,&no);}
            }
            gNoclipController=0;gNoclipIgnoredMove=false;
            gFlySpeedSaved=false;gOriginalFlyMovement=0;
            FHJDiag::Log(L"Noclip: native movement/collision setup failed; original values restored.");
            return;
        }
        if(!enabled) {
            if(gFlySpeedSaved && gOriginalFlyMovement==movement)
                TryWrite(movement+0x274,gOriginalFlySpeed);
            if(gNoclipIgnoredMove && gNoclipController) {
                const auto allow=FindFunction(L"Controller",L"SetIgnoreMoveInput");
                if(allow){bool no=false;Invoke(gNoclipController,allow,&no);}
            }
            gNoclipController=0;gNoclipIgnoredMove=false;
            gFlySpeedSaved=false;gOriginalFlyMovement=0;
            gNoclipEnabled=false;gNoclipPawn=0;gNoclipPositionValid=false;
        }
        FHJDiag::Log(enabled?L"Local noclip: flying/no collision enabled; WASD uses direct world-position movement.":
            L"Local noclip disabled; movement and collision restoration requested.");
    }

    // All parameter sizes below are from Engine_parameters.hpp in the matching 5.5.4 SDK.
    template<typename T> bool TryWrite(uintptr_t addr,const T& value) noexcept;
    struct alignas(16) PaddedVec3 { double x{}, y{}, z{}, pad{}; };
    struct Transform64 {
        double rotation[4]{0,0,0,1};
        PaddedVec3 location{}, scale{1,1,1,0};
    };
    static_assert(sizeof(Transform64)==0x60);

    bool SetActorLocationSafe(uintptr_t actor,const Vec3& next,
                              bool sweep=false,bool teleport=true) {
        const auto f=FindFunction(L"Actor",L"K2_SetActorLocation");
        if(!actor || !f)return false;
        alignas(8) uint8_t args[0x128]{};
        std::memcpy(args,&next,sizeof(next));
        args[0x18]=sweep?1:0; // collision sweep for cryptid fallback
        args[0x120]=teleport?1:0; // teleport remains appropriate for camera/noclip
        if(!Invoke(actor,f,args))return false;
        return args[0x121]!=0;
    }
    bool SetActorRotationSafe(uintptr_t actor,const Rot& rotation) {
        const auto f=FindFunction(L"Actor",L"K2_SetActorRotation");
        if(!actor || !f)return false;
        alignas(8) uint8_t args[0x20]{};
        std::memcpy(args,&rotation,sizeof(rotation));
        args[0x18]=1;
        return Invoke(actor,f,args) && args[0x19]!=0;
    }
    bool SetCameraViewTarget(uintptr_t pc,uintptr_t target) {
        const auto f=FindFunction(L"PlayerController",L"SetViewTargetWithBlend");
        if(!pc || !target || !f)return false;
        alignas(8) uint8_t args[0x18]{};
        std::memcpy(args,&target,sizeof(target));
        return Invoke(pc,f,args);
    }
    uintptr_t FindObjectByName(const std::wstring& name) {
        int total=0;uintptr_t chunks=0;
        if(!GetObjects(total,chunks))return 0;
        for(int i=0;i<total;++i) {
            auto obj=ObjectAt(i,chunks);
            if(obj && ObjectName(obj)==name)return obj;
        }
        return 0;
    }

    // Sphere trace between cryptid shoulder and camera. A SpringArm collision
    // probe is the native UE solution; here the already-spawned CameraActor
    // gets an equivalent reflected sweep, without a guessed SpringArm offset.
    // Exact UE5.5 FHitResult(0xE8) and SphereTraceSingle parameter layout are
    // verified against UFunction size/return metadata before invocation.
    bool ConstrainChaseCamera(const Vec3& pivot,const Vec3& wanted,Vec3& clipped) {
        static uintptr_t sphere=0,line=0,defaultSystem=0;
        static ULONGLONG lookupRetry=0;
        const auto tick=GetTickCount64();
        if((!sphere || !defaultSystem) && tick>=lookupRetry) {
            lookupRetry=tick+10000;
            if(!sphere)sphere=FindFunction(L"KismetSystemLibrary",L"SphereTraceSingle");
            if(!line)line=FindFunction(L"KismetSystemLibrary",L"LineTraceSingle");
            if(!defaultSystem)
                defaultSystem=FindObjectByName(L"Default__KismetSystemLibrary");
        }
        const bool lineFallback=!sphere && line;
        const auto fn=sphere?sphere:line;
        if(!fn || !defaultSystem)return false;
        uint16_t parmsSize=0,returnOffset=0;
        if(!Read(fn+0xB6,parmsSize) || !Read(fn+0xB8,returnOffset) ||
           parmsSize<0x169 || parmsSize>0x178 || returnOffset!=0x168) {
            static bool logged=false;
            if(!logged) {
                logged=true;
                FHJDiag::Log(L"CHASE CAMERA: SphereTraceSingle SDK layout differs; using close-camera fallback.");
            }
            return false;
        }
        alignas(8) uint8_t args[0x178]{};
        const uintptr_t ctx=gMonsterPawn;
        std::memcpy(args,&ctx,8);
        std::memcpy(args+0x08,&pivot,sizeof(pivot));
        std::memcpy(args+0x20,&wanted,sizeof(wanted));
        if(lineFallback) {
            args[0x38]=1; // TraceTypeQuery2: camera collision
            args[0x39]=0;
        } else {
            const float radius=22.f;
            std::memcpy(args+0x38,&radius,sizeof(radius));
            args[0x3C]=1; // TraceTypeQuery2: camera collision
            args[0x3D]=0;
        }
        uintptr_t ignored[1]{gMonsterPawn};
        struct ActorArray{uintptr_t data;int32_t num,max;};
        const ActorArray ignore{reinterpret_cast<uintptr_t>(ignored),1,1};
        static_assert(sizeof(ActorArray)==16);
        std::memcpy(args+0x40,&ignore,sizeof(ignore));
        args[0x50]=0;  // DrawDebugTrace::None
        args[0x140]=1; // bIgnoreSelf
        if(!Invoke(defaultSystem,fn,args))return false;
        clipped=wanted;
        // HitResult::Time (0x04) is the trace fraction. Both the reflected
        // return value and blocking-hit flag must be true before clamping.
        if(args[0x168] && (args[0x58+0xAD]&1)) {
            float impact=1.f;
            std::memcpy(&impact,args+0x58+0x04,sizeof(impact));
            if(!std::isfinite(impact) || impact<0.f || impact>1.f)return false;
            const double dx=wanted.x-pivot.x,dy=wanted.y-pivot.y,
                         dz=wanted.z-pivot.z;
            const double distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            const double backoff=distance>1.0?26.0/distance:1.0;
            const double alpha=std::clamp(static_cast<double>(impact)-backoff,0.0,1.0);
            clipped={pivot.x+dx*alpha,pivot.y+dy*alpha,pivot.z+dz*alpha};
        }
        return true;
    }
    // SDK verified: KismetSystemLibrary.GetConsoleVariableIntValue has
    // FString(0x10) + int return(0x10), ParmsSize=0x18. Console-command
    // params are WorldContext 8 / FString 16 / PlayerController 8.
    constexpr const wchar_t* kLocalClarityCVars[3]{
        L"r.MotionBlurQuality",
        L"r.DefaultFeature.MotionBlur",
        L"r.DepthOfFieldQuality"
    };
    bool ReadLocalRenderCVar(const wchar_t* name,int32_t& result) {
        const auto system=FindObjectByName(L"Default__KismetSystemLibrary");
        const auto getter=FindFunction(L"KismetSystemLibrary",
                                      L"GetConsoleVariableIntValue");
        uint16_t size=0,ret=0;
        if(!name || !system || !getter ||
           !Read(getter+0xB6,size) || size!=0x18 ||
           !Read(getter+0xB8,ret) || ret!=0x10)return false;
        wchar_t variable[64]{};
        wcscpy_s(variable,name);
        FStringView view{variable,static_cast<int32_t>(wcslen(variable)+1),
                         static_cast<int32_t>(wcslen(variable)+1)};
        alignas(8) uint8_t args[0x18]{};
        std::memcpy(args,&view,sizeof(view));
        if(!Invoke(system,getter,args))return false;
        std::memcpy(&result,args+0x10,sizeof(result));
        return true;
    }
    bool SetLocalRenderCVar(uintptr_t pc,const wchar_t* name,int32_t value) {
        const auto console=FindFunction(L"KismetSystemLibrary",
                                        L"ExecuteConsoleCommand");
        const auto system=FindObjectByName(L"Default__KismetSystemLibrary");
        const auto world=World();
        uint16_t size=0;
        if(!pc || !console || !system || !world ||
           !Read(console+0xB6,size) || size!=0x20)return false;
        wchar_t command[96]{};
        swprintf_s(command,L"%ls %d",name,value);
        FStringView view{command,static_cast<int32_t>(wcslen(command)+1),
                         static_cast<int32_t>(wcslen(command)+1)};
        alignas(8) uint8_t args[0x20]{};
        std::memcpy(args,&world,8);
        std::memcpy(args+8,&view,sizeof(view));
        std::memcpy(args+0x18,&pc,8);
        return Invoke(system,console,args);
    }
    // CameraComponent.PostProcessSettings 0x300, blend weight 0x2D0.
    // FPostProcessSettings.bOverride_MotionBlurAmount is bit 6 at 0x1C;
    // bOverride_MotionBlurMax is bit 7 at 0x1C. Float values are 0x674
    // and 0x678 inside the struct. All offsets from the user's exact SDK.
    void SetCameraBlurOverrides(uintptr_t cameraComponent) {
        if(!cameraComponent)return;
        uint8_t flags=0;
        if(!Read(cameraComponent+0x31C,flags))return;
        const uint8_t enabled=static_cast<uint8_t>(flags|0xC0);
        const float zero=0.f;
        if(!TryWrite(cameraComponent+0x31C,enabled) ||
           !TryWrite(cameraComponent+0x974,zero) ||
           !TryWrite(cameraComponent+0x978,zero))return;
        const auto weightFn=FindFunction(L"CameraComponent",
                                         L"SetPostProcessBlendWeight");
        uint16_t size=0;
        float full=1.f;
        if(weightFn && Read(weightFn+0xB6,size) && size==4)
            Invoke(cameraComponent,weightFn,&full);
        else TryWrite(cameraComponent+0x2D0,full);
    }
    void ApplyPossessionClarity(uintptr_t pc,uintptr_t cameraComponent) {
        if(!gCryptidClearLocalBlur.load() || !pc || !cameraComponent ||
           gPossessionClarity.active)return;
        PossessionClarityBackup saved{};
        saved.active=true;
        saved.camera=cameraComponent;
        Read(cameraComponent+0x0C,saved.cameraIndex);
        saved.cameraSaved=
            saved.cameraIndex>=0 &&
            Read(cameraComponent+0x31C,saved.previousOverrides) &&
            Read(cameraComponent+0x2D0,saved.previousWeight) &&
            Read(cameraComponent+0x974,saved.previousBlurAmount) &&
            Read(cameraComponent+0x978,saved.previousBlurMax);
        // Capture the original values BEFORE changing any render state.
        // Render CVars are local to this game's process; remote clients
        // retain their own view and any cinematic effects.
        for(size_t i=0;i<3;++i) {
            saved.cvarsSaved[i]=ReadLocalRenderCVar(
                kLocalClarityCVars[i],saved.originalCvars[i]);
            // Unknown/missing CVars may report sentinel values. Never
            // overwrite or later restore a setting we cannot validate.
            const int32_t v=saved.originalCvars[i];
            if(v<0 || v>8 || (i==1 && v>1))
                saved.cvarsSaved[i]=false;
        }
        gPossessionClarity=saved;
        SetCameraBlurOverrides(cameraComponent);
        int requested=0,verified=0;
        for(size_t i=0;i<3;++i) {
            if(!saved.cvarsSaved[i])continue;
            if(SetLocalRenderCVar(pc,kLocalClarityCVars[i],0)) {
                ++requested;
                int32_t readback=-1;
                if(ReadLocalRenderCVar(kLocalClarityCVars[i],readback) &&
                   readback==0)++verified;
            }
        }
        wchar_t result[260]{};
        swprintf_s(result,L"CRYPTID CAMERA: local blur clarity enabled; camera snapshot=%s, CVars dispatched=%d/3, immediate zero readback=%d/3. Originals are restored when possession ends; actual rendering must be checked in game.",
            saved.cameraSaved?L"YES":L"NO",requested,verified);
        FHJDiag::Log(result);
    }
    void RestorePossessionClarity(uintptr_t pc) {
        if(!gPossessionClarity.active || !pc || !World())return;
        const auto saved=gPossessionClarity;
        // Idempotent and never touch a recycled component on a new level.
        int32_t index=-1;
        if(saved.cameraSaved && saved.camera &&
           Read(saved.camera+0x0C,index) && index==saved.cameraIndex) {
            TryWrite(saved.camera+0x31C,saved.previousOverrides);
            TryWrite(saved.camera+0x974,saved.previousBlurAmount);
            TryWrite(saved.camera+0x978,saved.previousBlurMax);
            const auto weight=FindFunction(L"CameraComponent",
                                          L"SetPostProcessBlendWeight");
            uint16_t size=0;
            float originalWeight=saved.previousWeight;
            if(weight && Read(weight+0xB6,size) && size==4)
                Invoke(saved.camera,weight,&originalWeight);
            else TryWrite(saved.camera+0x2D0,originalWeight);
        }
        for(size_t i=0;i<3;++i)
            if(saved.cvarsSaved[i])
                SetLocalRenderCVar(pc,kLocalClarityCVars[i],
                                   saved.originalCvars[i]);
        gPossessionClarity=PossessionClarityBackup{};
        FHJDiag::Log(L"CRYPTID CAMERA: restored original local motion-blur / DOF console values and camera post-process settings.");
    }
    void RefreshPossessionClarity(uintptr_t pc) {
        if(!gCryptidClearLocalBlur.load()) {
            RestorePossessionClarity(pc);
            return;
        }
        if(!gPossessionClarity.active) {
            if(gChaseCamera) {
                const auto cameraComponent=ReadPtr(gChaseCamera+0x2B0);
                ApplyPossessionClarity(pc,cameraComponent);
            }
            return;
        }
        // Check every ~600ms, not every frame. Some camera systems may
        // reset camera blend weight during chase camera updates.
        const auto component=gPossessionClarity.camera;
        int32_t id=-1;
        if(!component || !Read(component+0x0C,id) ||
           id!=gPossessionClarity.cameraIndex)return;
        uint8_t flags=0;
        float amount=0.f,maxBlur=0.f,weight=0.f;
        if(Read(component+0x31C,flags) &&
           Read(component+0x974,amount) &&
           Read(component+0x978,maxBlur) &&
           Read(component+0x2D0,weight) &&
           ((flags&0xC0)!=0xC0 || amount!=0.f ||
            maxBlur!=0.f || weight<0.99f))
            SetCameraBlurOverrides(component);
    }
    bool CreateMonsterCamera(uintptr_t pc,const Vec3& position) {
        const auto world=World();
        auto cls=FindObjectByName(L"CameraActor");
        if(cls && ClassName(ReadPtr(cls+0x10))!=L"Class") cls=0;
        auto cdo=FindObjectByName(L"Default__GameplayStatics");
        const auto begin=FindFunction(L"GameplayStatics",L"BeginDeferredActorSpawnFromClass");
        const auto finish=FindFunction(L"GameplayStatics",L"FinishSpawningActor");
        if(!world||!cls||!cdo||!begin||!finish) {
            FHJDiag::Log(L"Chase camera: CameraActor/GameplayStatics reflection unavailable.");
            return false;
        }
        Transform64 tx{};
        tx.location.x=position.x;
        tx.location.y=position.y;
        tx.location.z=position.z;
        alignas(8) uint8_t args[0x90]{};
        std::memcpy(args,&world,8);
        std::memcpy(args+0x08,&cls,8);
        std::memcpy(args+0x10,&tx,sizeof(tx));
        args[0x70]=1; // AlwaysSpawn
        if(!Invoke(cdo,begin,args)) {
            FHJDiag::Log(L"Chase camera BeginDeferredActorSpawnFromClass failed.");
            return false;
        }
        uintptr_t camera=0;
        std::memcpy(&camera,args+0x88,8);
        if(!camera) {
            FHJDiag::Log(L"Chase camera returned a null actor.");
            return false;
        }
        alignas(8) uint8_t finishArgs[0x80]{};
        std::memcpy(finishArgs,&camera,8);
        std::memcpy(finishArgs+0x10,&tx,sizeof(tx));
        if(!Invoke(cdo,finish,finishArgs)) {
            FHJDiag::Log(L"FinishSpawningActor failed for chase camera.");
            return false;
        }
        std::memcpy(&camera,finishArgs+0x78,8);
        if(!camera || !SetCameraViewTarget(pc,camera)) {
            FHJDiag::Log(L"Could not bind player view to chase camera.");
            return false;
        }
        // PlayerController::bAutoManageActiveCameraTarget would otherwise undo
        // our chase-camera view target on subsequent possession updates.
        bool previous=true;
        if(Read(pc+0x370,previous)) {
            bool disable=false;
            if(TryWrite(pc+0x370,disable)) {
                gOriginalCameraAutoManage=previous;
                gCameraAutoManageSaved=true;
            }
        }
        gChaseCamera=camera;
        Read(camera+0x0C,gCameraIndex);
        const auto comp=ReadPtr(camera+0x2B0); // Engine.CameraActor.CameraComponent
        auto fov=FindFunction(L"CameraComponent",L"SetFieldOfView");
        if(comp && fov) {
            float angle=86.0f;
            Invoke(comp,fov,&angle);
        }
        ApplyPossessionClarity(pc,comp);
        FHJDiag::Log(L"Created camera actor and switched view target for monster mode.");
        return true;
    }
    // Mothman's BP_FlyingPawnTest_C::Smoke is the Niagara component at
    // +0x3A8. SceneComponent::bHiddenInGame (+0x189 bit 3) is NOT a Net
    // property in the supplied SDK. Its bVisible flag (+0x188 bit 5)
    // IS replicated, so NEVER use SceneComponent.SetVisibility here.
    // This suppresses only the host's local Smoke rendering while they
    // pilot Mothman. Other players retain the usual effect.
    bool SetLocalSmokeHidden(uintptr_t component,bool hidden) {
        const auto fn=FindFunction(L"SceneComponent",L"SetHiddenInGame");
        uint16_t params=0;
        if(!component || !fn || !Read(fn+0xB6,params) || params!=2)
            return false;
        uint8_t args[2]{static_cast<uint8_t>(hidden?1:0),0};
        return Invoke(component,fn,args);
    }
    void RestoreLocalMothmanSmoke() {
        const auto saved=gLocalMothmanSmoke;
        gLocalMothmanSmoke=LocalMothmanSmoke{};
        if(!saved.pawn || !saved.component)return;
        int32_t pawnId=-1,componentId=-1;
        if(!Read(saved.pawn+0x0C,pawnId) || pawnId!=saved.pawnIndex ||
           !Read(saved.component+0x0C,componentId) ||
           componentId!=saved.componentIndex ||
           ReadPtr(saved.pawn+0x3A8)!=saved.component)return;
        if(!SetLocalSmokeHidden(saved.component,saved.previouslyHidden))
            FHJDiag::Log(L"MOTHMAN LOCAL SMOKE: could not restore original visibility.");
    }
    void SyncLocalMothmanSmoke() {
        const bool controllingMothman=gMonsterActive.load() &&
            ClassName(ReadPtr(gMonsterPawn+0x10))==L"BP_FlyingPawnTest_C";
        if(!controllingMothman || !gMothmanReduceLocalSmoke.load()) {
            if(gLocalMothmanSmoke.component)RestoreLocalMothmanSmoke();
            return;
        }
        const auto smoke=ReadPtr(gMonsterPawn+0x3A8);
        if(!smoke)return;
        int32_t pawnId=-1,smokeId=-1;
        if(!Read(gMonsterPawn+0x0C,pawnId) || pawnId!=gMonsterIndex ||
           !Read(smoke+0x0C,smokeId) || smokeId<0)return;
        if(gLocalMothmanSmoke.pawn==gMonsterPawn &&
           gLocalMothmanSmoke.component==smoke &&
           gLocalMothmanSmoke.componentIndex==smokeId)return;
        if(gLocalMothmanSmoke.component)RestoreLocalMothmanSmoke();
        uint8_t componentFlags=0;
        if(!Read(smoke+0x189,componentFlags))return;
        const bool wasHidden=(componentFlags&0x08)!=0;
        if(!SetLocalSmokeHidden(smoke,true)) {
            FHJDiag::Log(L"MOTHMAN LOCAL SMOKE: native nonreplicated SetHiddenInGame unavailable.");
            return;
        }
        gLocalMothmanSmoke={gMonsterPawn,smoke,pawnId,smokeId,wasHidden};
        FHJDiag::Log(L"MOTHMAN LOCAL SMOKE: hid only the local Smoke Niagara renderer; eyes, moths, wings and remote player visuals are unchanged.");
    }
    void RestoreLocalMothmanRender() {
        const auto saved=gMothRender;
        gMothRender=LocalMothmanRender{};
        int32_t live=-1;
        if(!saved.pawn || !Read(saved.pawn+0xC,live) ||
           live!=saved.pawnId)return;
        constexpr uintptr_t expectedOffsets[3]{0x3D0,0x3B8,0x3C0};
        for(size_t i=0;i<3;++i) {
            const auto& part=saved.visibleParts[i];
            int32_t id=-1;
            if(part.modified && part.component &&
               ReadPtr(saved.pawn+expectedOffsets[i])==part.component &&
               Read(part.component+0xC,id) && id==part.id)
                SetLocalSmokeHidden(part.component,part.originallyHidden);
        }
        int32_t postId=-1;
        if(saved.postModified && saved.postProcess &&
           ReadPtr(saved.pawn+0x340)==saved.postProcess &&
           Read(saved.postProcess+0xC,postId) && postId==saved.postId) {
            uint8_t flags=0;
            if(Read(saved.postProcess+0x94C,flags)) {
                const uint8_t restored=saved.postEnabled?
                    static_cast<uint8_t>(flags|1):
                    static_cast<uint8_t>(flags&~1);
                TryWrite(saved.postProcess+0x94C,restored);
            }
        }
        FHJDiag::Log(L"MOTHMAN LOCAL VISION: restored pre-possession body/wing visibility and post-process state.");
    }
    void SyncLocalMothmanRender() {
        if(!gMonsterActive.load() ||
           ClassName(ReadPtr(gMonsterPawn+0x10))!=L"BP_FlyingPawnTest_C") {
            if(gMothRender.pawn)RestoreLocalMothmanRender();
            return;
        }
        int32_t id=-1;
        if(!Read(gMonsterPawn+0xC,id) || id!=gMonsterIndex)return;
        if(gMothRender.pawn!=gMonsterPawn || gMothRender.pawnId!=id) {
            if(gMothRender.pawn)RestoreLocalMothmanRender();
            gMothRender.pawn=gMonsterPawn;
            gMothRender.pawnId=id;
        }
        // IsStealthed (+0x431) is a replicated gameplay state: it MUST NOT
        // be altered just to make the local camera easier to use.
        bool stealth=false;
        if(Read(gMonsterPawn+0x431,stealth) &&
           (!gMothRender.stealthLogged || stealth!=gMothRender.lastStealthed)) {
            gMothRender.lastStealthed=stealth;
            gMothRender.stealthLogged=true;
            FHJDiag::Log(stealth?
                L"MOTHMAN VISIBILITY: native replicated IsStealthed=TRUE; local renderer is compensated without changing stealth for other players.":
                L"MOTHMAN VISIBILITY: native replicated IsStealthed=FALSE; local render visibility rechecked.");
        }
        constexpr uintptr_t offsets[3]{0x3D0,0x3B8,0x3C0};
        for(size_t i=0;i<3;++i) {
            const auto partPtr=ReadPtr(gMonsterPawn+offsets[i]);
            if(!partPtr)continue;
            auto& part=gMothRender.visibleParts[i];
            int32_t partIndex=-1;
            if(!Read(partPtr+0xC,partIndex))continue;
            if(part.component!=partPtr || part.id!=partIndex) {
                part=LocalMothmanRender::Part{};
                part.component=partPtr;
                part.id=partIndex;
                uint8_t flags=0;
                if(Read(partPtr+0x189,flags))
                    part.originallyHidden=(flags&0x08)!=0;
            }
            uint8_t flags=0;
            if(Read(partPtr+0x189,flags) && (flags&0x08)) {
                if(SetLocalSmokeHidden(partPtr,false))
                    part.modified=true;
            }
        }
        // BP_FlyingPawnTest_C.PostProcess is a separate local
        // UPostProcessComponent, whose bEnabled (+0x94C bit 0) is NOT
        // replicated in UE5.5. Suppress only its local darkening/
        // blur effect while the hunter is piloting the creature.
        const auto post=ReadPtr(gMonsterPawn+0x340);
        int32_t postId=-1;
        if(post && Read(post+0xC,postId)) {
            if(gMothRender.postProcess!=post ||
               gMothRender.postId!=postId) {
                gMothRender.postProcess=post;
                gMothRender.postId=postId;
                uint8_t flags=0;
                if(Read(post+0x94C,flags))
                    gMothRender.postEnabled=(flags&1)!=0;
            }
            uint8_t flags=0;
            if(Read(post+0x94C,flags) && (flags&1) &&
               TryWrite(post+0x94C,static_cast<uint8_t>(flags&~1)))
                gMothRender.postModified=true;
        }
    }
    void ReleaseMonsterCamera(uintptr_t pc,uintptr_t destination) {
        RestorePossessionClarity(pc);
        RestoreLocalMothmanSmoke();
        RestoreLocalMothmanRender();
        if(pc && gCameraAutoManageSaved) {
            TryWrite(pc+0x370,gOriginalCameraAutoManage);
            gCameraAutoManageSaved=false;
        }
        if(pc && destination)SetCameraViewTarget(pc,destination);
        if(gChaseCamera) {
            int32_t live=-1;
            if(Read(gChaseCamera+0x0C,live) && live==gCameraIndex) {
                auto destroy=FindFunction(L"Actor",L"K2_DestroyActor");
                if(destroy)Invoke(gChaseCamera,destroy,nullptr);
            }
        }
        gChaseCamera=0;
        gCameraIndex=-1;
    }
    void UpdateMonsterHUD() {
        if(!gMonsterActive.load())return;
        if(gControlledRespawnStart>=0.0) {
            std::lock_guard<std::mutex> lock(gMonsterStatusMutex);
            gMonsterHUD.respawning=true;
            gMonsterHUD.respawnSeconds=static_cast<float>(
                std::max(0.0,gControlledRespawnRemaining));
            gMonsterHUD.health=0.0;
            gMonsterHUD.cameraReady=gChaseCamera!=0;
            return;
        }
        std::string name;
        auto cls=ReadPtr(gMonsterPawn+0x10);
        auto wide=ClassName(cls);
        name.assign(wide.begin(),wide.end());
        int species=0;
        auto lower=Lower(wide);
        if(lower.find(L"moth")!=std::wstring::npos || lower.find(L"flying")!=std::wstring::npos)species=1;
        else if(lower.find(L"vamp")!=std::wstring::npos)species=2;
        else if(lower.find(L"wend")!=std::wstring::npos)species=3;
        double health=-1.0,maxHealth=-1.0;
        // Mothman does not declare Health/MaxHealth at the base-cryptid
        // offsets; health remains unknown rather than displaying garbage.
        if(species!=1) {
            Read(gMonsterPawn+0x710,health);
            Read(gMonsterPawn+0x718,maxHealth);
        }
        std::lock_guard<std::mutex> lock(gMonsterStatusMutex);
        gMonsterHUD.label=name.empty()?"UNKNOWN CRYPTID":name;
        gMonsterHUD.species=species;
        gMonsterHUD.health=health;
        gMonsterHUD.maxHealth=maxHealth;
        gMonsterHUD.cameraReady=gChaseCamera!=0;
        gMonsterHUD.possessConfirmed=true;
        gMonsterHUD.respawning=false;
        gMonsterHUD.respawnSeconds=0.f;
        for(int slot=0;slot<5;++slot) {
            const auto fn=CreatureSkillFunction(species,slot);
            gMonsterHUD.available[static_cast<size_t>(slot)]=fn!=0;
            gMonsterHUD.skills[static_cast<size_t>(slot)]=CreatureSkillLabel(species,slot,fn);
        }
    }
    // UE5.5 SDK Engine.Pawn.AddMovementInput: FVector (0x18), float, bool.
    // CharacterMovement consumes this input on its own tick and updates
    // velocity + AnimBP state. Never teleport the monster or human pawn.
    bool AddSmoothMovement(uintptr_t pawn,const Vec3& input,float gain=1.f) {
        if(!pawn)return false;
        static uintptr_t pawnMove=0;
        if(!pawnMove)pawnMove=FindFunction(L"Pawn",L"AddMovementInput");
        if(pawnMove) {
            alignas(8) unsigned char args[0x20]{};
            std::memcpy(args,&input,sizeof(input));
            std::memcpy(args+0x18,&gain,sizeof(gain));
            args[0x1C]=1; // bForce: support AI-controlled cryptid
            return Invoke(pawn,pawnMove,args);
        }
        // Fallback is the native PawnMovementComponent input accumulator.
        const auto movement=ReadPtr(pawn+0x330);
        const auto add=FindFunction(L"PawnMovementComponent",L"AddInputVector");
        if(!movement||!add)return false;
        alignas(8) unsigned char args[0x20]{};
        std::memcpy(args,&input,sizeof(input));
        args[0x18]=1;
        return Invoke(movement,add,args);
    }
    bool SetHUDVisibility(uintptr_t widget,uint8_t visibility) {
        const auto func=FindFunction(L"Widget",L"SetVisibility");
        return widget && func && Invoke(widget,func,&visibility);
    }
    void RestoreHumanHUD() {
        if(!gHumanHudHidden && !gHudRestorePending)return;
        int32_t live=-1;
        if(gHumanHUD && Read(gHumanHUD+0x0C,live) && live==gHumanHUDIndex) {
            if(!SetHUDVisibility(gHumanHUD,gPriorHumanHudVisibility)) {
                FHJDiag::Log(L"Monster HUD: failed restoring original hunter widget visibility.");
                gHudRestorePending=true;
                return;
            }
        }
        gHumanHUD=0;gHumanHUDIndex=-1;
        gHumanHudHidden=false;gHudRestorePending=false;
        FHJDiag::Log(L"Monster HUD: hunter interface restored to its previous visibility.");
    }
    void HideHumanHUD() {
        if(!gHumanPawn)return;
        const auto widget=ReadPtr(gHumanPawn+0x870); // SDK PlayerCharacter_C.HUD
        if(!widget)return;
        auto currentClass=ClassName(ReadPtr(widget+0x10));
        if(currentClass!=L"PlayerHUD_C")return;
        if(!gHumanHudHidden || widget!=gHumanHUD) {
            gHumanHUD=widget;
            Read(widget+0x0C,gHumanHUDIndex);
            const auto fn=FindFunction(L"Widget",L"GetVisibility");
            uint8_t visibility=0;
            if(!fn || !Invoke(widget,fn,&visibility)) {
                FHJDiag::Log(L"Monster HUD: failed reading hunter widget visibility.");
                return;
            }
            gPriorHumanHudVisibility=visibility;
            gHumanHudHidden=true;
            FHJDiag::Log(L"Monster HUD: captured original PlayerHUD_C visibility.");
        }
        const auto fn=FindFunction(L"Widget",L"GetVisibility");
        uint8_t current=0;
        if(fn && Invoke(widget,fn,&current) && current!=1)
            SetHUDVisibility(widget,1); // ESlateVisibility::Collapsed
    }
    void RestoreHumanControls() {
        if(!gHumanInputController)return;
        int32_t live=-1;
        if(gHumanPawn && Read(gHumanPawn+0x0C,live) && live==gHumanIndex &&
           gHumanInputDisabled) {
            const auto fn=FindFunction(L"Actor",L"EnableInput");
            if(fn)Invoke(gHumanPawn,fn,&gHumanInputController);
        }
        if(gHumanIgnoreMovement) {
            const auto fn=FindFunction(L"Controller",L"SetIgnoreMoveInput");
            if(fn) {bool ignored=false;Invoke(gHumanInputController,fn,&ignored);}
        }
        if(gHumanIgnoreLook) {
            const auto fn=FindFunction(L"Controller",L"SetIgnoreLookInput");
            if(fn) {bool ignored=false;Invoke(gHumanInputController,fn,&ignored);}
        }
        gHumanInputController=0;gHumanInputDisabled=false;
        gHumanIgnoreMovement=false;gHumanIgnoreLook=false;
        FHJDiag::Log(L"Monster Mode: human input bindings and controller movement restored.");
    }
    void SuspendHumanControls(uintptr_t pc) {
        if(!pc || !gHumanPawn || gHumanInputController)return;
        gHumanInputController=pc;
        // Disable the *human pawn's* gameplay InputComponent without destroying
        // or unpossessing it; the avatar remains in the expedition.
        const auto disable=FindFunction(L"Actor",L"DisableInput");
        if(disable)gHumanInputDisabled=Invoke(gHumanPawn,disable,&pc);
        const auto ignoreMove=FindFunction(L"Controller",L"SetIgnoreMoveInput");
        if(ignoreMove) {
            bool ignored=true;
            gHumanIgnoreMovement=Invoke(pc,ignoreMove,&ignored);
        }
        const auto ignoreLook=FindFunction(L"Controller",L"SetIgnoreLookInput");
        if(ignoreLook) {
            bool ignored=true;
            gHumanIgnoreLook=Invoke(pc,ignoreLook,&ignored);
        }
        FHJDiag::Log(gHumanInputDisabled ?
            L"Monster Mode: human pawn input disabled; controller motion/look blocked.":
            L"Monster Mode: WARNING pawn DisableInput failed; weapon input may remain active.");
    }
    void StopMonsterAI() {
        const auto ai=ReadPtr(gMonsterPawn+0x2D8); // APawn.Controller
        if(!ai)return;
        gMonsterAIController=ai;
        // Stop active path following as well as the behavior tree. Stopping
        // BrainComponent alone leaves the last MoveTo request running.
        const auto stopPath=FindFunction(L"AIController",L"StopMovement");
        if(stopPath)Invoke(ai,stopPath,nullptr);
        // For Mothman, 0x330 is BP_StuckDetector, NOT CharacterMovement.
        const auto creatureClass=ClassName(ReadPtr(gMonsterPawn+0x10));
        if(creatureClass!=L"BP_FlyingPawnTest_C") {
            const auto movement=ReadPtr(gMonsterPawn+0x330);
            const auto stopVelocity=FindFunction(L"MovementComponent",L"StopMovementImmediately");
            if(movement && stopVelocity)Invoke(movement,stopVelocity,nullptr);
        }
        const auto brain=ReadPtr(ai+0x388); // SDK AIModule.AAIController.BrainComponent
        if(!brain)return;
        const auto state=FindFunction(L"BrainComponent",L"IsRunning");
        bool running=false;
        if(state)Invoke(brain,state,&running);
        gMonsterBrain=brain;
        gMonsterBrainWasRunning=running;
        const auto stop=FindFunction(L"BrainComponent",L"StopLogic");
        if(stop && running) {
            wchar_t textReason[]=L"HunterJournal: remote cryptid control";
            FStringView reason{textReason,
                static_cast<int32_t>(sizeof(textReason)/sizeof(wchar_t)),
                static_cast<int32_t>(sizeof(textReason)/sizeof(wchar_t))};
            Invoke(brain,stop,&reason);
            FHJDiag::Log(L"Monster Mode: AI brain paused; animation graph remains active.");
        }
    }

    // In UE5.5, AAIController::MoveToLocation is a reflected UFUNCTION.
    // CharacterMovement applies navigation-requested velocity and drives
    // AnimBlueprint locomotion, unlike ignored player AddMovementInput.
    bool MoveCryptidWithAI(const Vec3& input,ULONGLONG now) {
        if(!gMonsterAIController || !gMonsterPawn)return false;
        const auto fn=FindFunction(L"AIController",L"MoveToLocation");
        if(!fn)return false;
        // Check UFunction::ParmsSize/ReturnValueOffset before relying on this
        // UE5.5 layout. Never issue a speculative ProcessEvent signature.
        uint16_t size=0,returnOffset=0;
        if(!Read(fn+0xB6,size) || !Read(fn+0xB8,returnOffset) ||
           size<0x2A || size>0x38 || returnOffset!=0x29) {
            static ULONGLONG last=0;
            if(now-last>10000) {
                last=now;
                FHJDiag::Log(L"CRYPTID MOVE: MoveToLocation reflected signature differs; using swept fallback.");
            }
            return false;
        }
        Vec3 origin{};
        if(!Position(gMonsterPawn,origin))return false;
        const double alignment=input.x*gMonsterMoveDirX+
                               input.y*gMonsterMoveDirY;
        const double remaining=std::hypot(
            gMonsterMoveGoal.x-origin.x,gMonsterMoveGoal.y-origin.y);
        if(gMonsterPathActive && alignment>.972 && remaining>150.0 &&
           now-gMonsterLastMoveRequest<1250)
            return true; // use native path for up to 1.25s, then refresh
        if(gMonsterPathActive && now-gMonsterLastMoveRequest<85)
            return true; // short directional debounce keeps animation continuous
        // Project farther ahead so the AI follows a continuous path until
        // approaching its goal; repath only near the target or upon turning.
        const Vec3 target{origin.x+input.x*900.0,
                          origin.y+input.y*900.0,origin.z};
        alignas(8) uint8_t args[0x38]{};
        std::memcpy(args,&target,sizeof(target));
        const float acceptance=50.0f;
        std::memcpy(args+0x18,&acceptance,sizeof(acceptance));
        args[0x1C]=0; // StopOnOverlap: disabled, use explicit acceptance radius
        args[0x1D]=1; // bUsePathfinding
        args[0x1E]=1; // bProjectDestinationToNavigation
        args[0x1F]=1; // bCanStrafe
        args[0x28]=1; // bAllowPartialPath
        if(!Invoke(gMonsterAIController,fn,args))return false;
        // EPathFollowingRequestResult: Failed=0, AlreadyAtGoal=1,
        // RequestSuccessful=2. Do not equate ProcessEvent success with motion.
        if(args[0x29]==0)return false;
        gMonsterLastMoveRequest=now;
        gMonsterMoveDirX=input.x;gMonsterMoveDirY=input.y;
        gMonsterMoveGoal=target;
        gMonsterPathActive=true;
        return true;
    }
    void StopCryptidPath() {
        if(!gMonsterPathActive)return;
        if(gMonsterAIController) {
            const auto stop=FindFunction(L"AIController",L"StopMovement");
            if(stop)Invoke(gMonsterAIController,stop,nullptr);
        }
        gMonsterPathActive=false;
        gMonsterLastMoveRequest=0;
    }
    void CacheCryptidActions() {
        gMonsterFunctions.clear();
        auto cls=ReadPtr(gMonsterPawn+0x10);
        std::vector<uintptr_t> hierarchy;
        for(int n=0;n<25 && cls;++n) {
            hierarchy.push_back(cls);
            const auto next=ReadPtr(cls+0x40);
            if(!next || next==cls)break;
            cls=next;
        }
        int count=0;uintptr_t chunks=0;
        if(!GetObjects(count,chunks))return;
        // Visit derived class first so its own attack/ability implementation
        // wins over an inherited method with the same UFunction name.
        for(const auto owner:hierarchy) {
            for(int i=0;i<count;++i) {
                const auto obj=ObjectAt(i,chunks);
                if(!obj || ReadPtr(obj+0x20)!=owner ||
                   ClassName(ReadPtr(obj+0x10))!=L"Function")continue;
                const auto name=ObjectName(obj);
                if(!name.empty())gMonsterFunctions.emplace(name,obj);
            }
        }
        wchar_t msg[160]{};
        swprintf_s(msg,L"CRYPTID: indexed %u reflected functions from creature class hierarchy.",
            static_cast<unsigned>(gMonsterFunctions.size()));
        FHJDiag::Log(msg);
        // A method name alone does not prove a callable signature. Log the
        // actual reflected ParmSize for attacks, invisibility and stance.
        for(const auto& [name,method]:gMonsterFunctions) {
            const auto lower=Lower(name);
            if(lower.find(L"attack")==std::wstring::npos &&
               lower.find(L"melee")==std::wstring::npos &&
               lower.find(L"sound")==std::wstring::npos &&
               lower.find(L"invis")==std::wstring::npos &&
               lower.find(L"stance")==std::wstring::npos &&
               lower.find(L"scream")==std::wstring::npos)continue;
            uint16_t params=0;
            if(!Read(method+0xB6,params))continue;
            wchar_t detail[240]{};
            swprintf_s(detail,L"CRYPTID SDK METHOD: %s / ParmSize=%u",name.c_str(),params);
            FHJDiag::Log(detail);
        }
    }
    uintptr_t NoArgCryptidAction(std::initializer_list<const wchar_t*> candidates) {
        for(const wchar_t* name:candidates) {
            const auto it=gMonsterFunctions.find(name);
            if(it==gMonsterFunctions.end())continue;
            uint16_t params=0;
            if(Read(it->second+0xB6,params) && params==0)return it->second;
        }
        return 0;
    }
    // The species is detected from the live inherited Blueprint class.
    // Expose a skill only if its UFunction exists and has a compatible
    // parameter layout. No generic vampire skills are assigned to Wendigo.
    uintptr_t CreatureSkillFunction(int species,int slot) {
        switch(slot) {
            case 0:
                // Mothman is an APawn: the three-map SDK declares NO
                // standard melee UFunction. DarknessAttack is a separate
                // special attack, NOT LMB. Do not activate it on click.
                if(species==1)return 0;
                // Ground cryptids are native ACharacters; use the
                // authoritative base attack that selects native montages.
                return NoArgCryptidAction({
                    L"Server_Attack",L"Melee"});
            case 1:
                if(species==1) return NoArgCryptidAction({
                    L"ForceScreech",L"RetriggerScreech"});
                if(species==3) return NoArgCryptidAction({
                    L"AUTH_Scream",L"Howl",L"AUTH_Howl",L"Roar",
                    L"Server_Scream",L"Scream"});
                return NoArgCryptidAction({
                    L"AUTH_Scream",L"Server_Scream",L"Scream",
                    L"Roar",L"DoScream",L"PlayRoar"});
            case 2:
                if(species==1) return NoArgCryptidAction({
                    L"RetriggerScreech"});
                if(species==3) return NoArgCryptidAction({
                    L"Lunge",L"AUTH_Lunge",L"Charge",
                    L"AUTH_Charge",L"Pounce",L"LeapAttack"});
                if(species!=2)return 0;
                if(const auto fn=NoArgCryptidAction({
                    L"Invisible",L"AUTH_Invisible",L"GoInvisible",
                    L"ToggleInvisible",L"ActivateInvisibility"}))return fn;
                for(const wchar_t* name:{L"SetInvisible",L"SetInvisibility",
                                         L"AUTH_SetInvisible",L"SetIsInvisible"}) {
                    const auto it=gMonsterFunctions.find(name);
                    uint16_t size=0;
                    if(it!=gMonsterFunctions.end() && Read(it->second+0xB6,size) &&
                       (size==1 || size==2))return it->second;
                }
                return 0;
            case 3:
                if(species==1)return NoArgCryptidAction({
                    L"DarknessAttack"}); // special power on R, never LMB
                if(species==3)return NoArgCryptidAction({
                    L"Rage",L"AUTH_Rage",L"Enrage",L"AUTH_Enrage",
                    L"Frenzy",L"FeralRoar"});
                if(species!=2)return 0;
                if(const auto fn=NoArgCryptidAction({
                    L"CycleStance",L"ChangeStance",L"SwitchStance"}))return fn;
                for(const wchar_t* name:{L"SetStance",L"AUTH_SetStance"}) {
                    const auto it=gMonsterFunctions.find(name);
                    uint16_t size=0;
                    if(it!=gMonsterFunctions.end() && Read(it->second+0xB6,size) &&
                       (size==1 || size==4 || size==8))return it->second;
                }
                return 0;
            case 4:
                return species==2 ? NoArgCryptidAction({
                    L"All_ScreamSound",L"ScreamSound",L"PlayScreamSound",
                    L"PlayScream"}):0;
            default:return 0;
        }
    }
    std::string CreatureSkillLabel(int species,int slot,uintptr_t fn) {
        if(!fn)return {};
        if(slot==0)return "LMB NATIVE MELEE";
        // Show the *actual* reflected method, not a guessed gameplay power.
        const auto method=ObjectName(fn);
        const std::string name(method.begin(),method.end());
        const char* key=slot==1?"Q":slot==2?"E":slot==3?"R":"AUDIO";
        return std::string(key)+" "+name;
    }

    void RestoreGroundMovementSettings() {
        const auto backup=gGroundMovementSettings;
        gGroundMovementSettings=GroundMovementSettings{};
        gGroundDrive=GroundMovementDrive{};
        if(!backup.saved || !backup.pawn || !backup.movement)return;
        int32_t id=-1;
        if(!Read(backup.pawn+0xC,id) || id!=backup.pawnId ||
           ReadPtr(backup.pawn+0x330)!=backup.movement)return;
        uint8_t rotation=0,path=0,yaw=0;
        if(Read(backup.movement+0x2D8,rotation)) {
            // CharacterMovement: orient-to-velocity bit 4;
            // use-controller-desired-yaw bit 3.
            const uint8_t bits=static_cast<uint8_t>(0x18);
            const uint8_t restored=static_cast<uint8_t>(
                (rotation&~bits) | (backup.rotationBefore&bits));
            TryWrite(backup.movement+0x2D8,restored);
        }
        if(Read(backup.movement+0x546,path)) {
            // bRequestedMoveUseAcceleration bit 6.
            const uint8_t restored=static_cast<uint8_t>(
                (path&~0x40) | (backup.pathBefore&0x40));
            TryWrite(backup.movement+0x546,restored);
        }
        if(Read(backup.pawn+0x2B0,yaw)) {
            // APawn::bUseControllerRotationYaw bit 1.
            const uint8_t restored=static_cast<uint8_t>(
                (yaw&~0x02) | (backup.pawnYawBefore&0x02));
            TryWrite(backup.pawn+0x2B0,restored);
        }
        FHJDiag::Log(L"GROUND CRYPTID: restored original native movement orientation/acceleration flags.");
    }
    void EnableGroundMovementSettings() {
        if(!gMonsterPawn || !gMonsterMovement)return;
        uint8_t turn=0,path=0,yaw=0;
        int32_t id=-1;
        if(!Read(gMonsterPawn+0xC,id) ||
           !Read(gMonsterMovement+0x2D8,turn) ||
           !Read(gMonsterMovement+0x546,path) ||
           !Read(gMonsterPawn+0x2B0,yaw))return;
        gGroundMovementSettings={true,gMonsterPawn,gMonsterMovement,id,turn,path,yaw};
        // Native CharacterMovement now rotates in the velocity direction,
        // accelerates into nav moves, and does not snap to controller yaw.
        const uint8_t newTurn=static_cast<uint8_t>((turn|0x10)&~0x08);
        const uint8_t newPath=static_cast<uint8_t>(path|0x40);
        const uint8_t newYaw=static_cast<uint8_t>(yaw&~0x02);
        TryWrite(gMonsterMovement+0x2D8,newTurn);
        TryWrite(gMonsterMovement+0x546,newPath);
        TryWrite(gMonsterPawn+0x2B0,newYaw);
        FHJDiag::Log(L"GROUND CRYPTID: native orient-to-velocity and acceleration-based movement enabled.");
    }
    // AI pathing is useful where NPC movement input is intentionally
    // ignored, but its repeated path requests make manual WASD feel
    // delayed. Try UE CharacterMovement input first, then switch to
    // native AI navigation only if the actor genuinely remains stuck.
    void DriveGroundCryptid(const Vec3& desired,bool boost,ULONGLONG now) {
        int32_t id=-1;
        Vec3 position{};
        if(!gMonsterPawn || !Read(gMonsterPawn+0xC,id) ||
           !Position(gMonsterPawn,position))return;
        if(gGroundDrive.pawn!=gMonsterPawn || gGroundDrive.pawnId!=id) {
            gGroundDrive=GroundMovementDrive{};
            gGroundDrive.pawn=gMonsterPawn;
            gGroundDrive.pawnId=id;
        }
        if(!gGroundDrive.sampleValid) {
            gGroundDrive.sampleValid=true;
            gGroundDrive.sampleAt=now;
            gGroundDrive.samplePosition=position;
        }
        if(!gGroundDrive.useNavFallback) {
            StopCryptidPath();
            const bool invoked=AddSmoothMovement(gMonsterPawn,desired,1.f);
            if(!invoked) {
                gGroundDrive.useNavFallback=true;
                FHJDiag::Log(L"GROUND CRYPTID: native movement input unavailable; using AI navigation fallback.");
            } else if(now-gGroundDrive.sampleAt>=480) {
                const double movement=std::hypot(
                    position.x-gGroundDrive.samplePosition.x,
                    position.y-gGroundDrive.samplePosition.y);
                if(movement<5.0) {
                    gGroundDrive.useNavFallback=true;
                    FHJDiag::Log(L"GROUND CRYPTID: movement input was accepted but character stayed stationary; switching to game AI path following.");
                }
                gGroundDrive.sampleAt=now;
                gGroundDrive.samplePosition=position;
            }
        }
        if(gGroundDrive.useNavFallback)
            MoveCryptidWithAI(desired,now);
    }
    void RestoreMonsterAI() {
        RestoreGroundMovementSettings();
        gVampireVerify=VampireInvisibilityVerification{};
        int32_t actorId=-1;
        const bool actorLive=gMonsterPawn &&
            Read(gMonsterPawn+0xC,actorId) && actorId==gMonsterIndex;
        const bool movementLive=actorLive && gMonsterMovement &&
            ReadPtr(gMonsterPawn+0x330)==gMonsterMovement;
        const bool controllerLive=actorLive && gMonsterAIController &&
            ReadPtr(gMonsterPawn+0x2D8)==gMonsterAIController;
        if(controllerLive)StopCryptidPath();
        else {gMonsterPathActive=false;gMonsterLastMoveRequest=0;}
        gMonsterFunctions.clear();
        if(movementLive && gMonsterSpeedsSaved) {
            TryWrite(gMonsterMovement+0x268,gMonsterOriginalWalkSpeed);
            TryWrite(gMonsterMovement+0x274,gMonsterOriginalFlySpeed);
        }
        if(movementLive && gMonsterModeSaved) {
            const auto setMode=FindFunction(L"CharacterMovementComponent",L"SetMovementMode");
            if(setMode) {
                uint8_t args[2]{gMonsterPriorMovement,0};
                Invoke(gMonsterMovement,setMode,args);
            }
        }
        gMonsterMovement=0;
        gMonsterSpeedsSaved=false;gMonsterModeSaved=false;
        gMonsterNativeWalkSpeed=0.f;gMonsterNativeRunSpeed=0.f;
        gMonsterFlightActive=false;gGroundJumpHeld=false;
        gMothFlight=NativeMothmanFlight{};
        if(controllerLive && gMonsterBrain && gMonsterBrainWasRunning) {
            const auto restart=FindFunction(L"BrainComponent",L"RestartLogic");
            if(restart)Invoke(gMonsterBrain,restart,nullptr);
            FHJDiag::Log(L"Monster Mode: previous AI behavior restored.");
        }
        gMonsterBrain=0;gMonsterAIController=0;gMonsterBrainWasRunning=false;
    }
    void ConfigureMonsterMovement() {
        gGroundMovementSettings=GroundMovementSettings{};
        gGroundDrive=GroundMovementDrive{};
        gMothFlight=NativeMothmanFlight{};
        if(ClassName(ReadPtr(gMonsterPawn+0x10))==L"BP_FlyingPawnTest_C") {
            // The exact SDK says Mothman is APawn; its 0x330 is a
            // StuckDetector component. Never treat it as CharacterMovement.
            gMonsterMovement=0;gMonsterSpeedsSaved=false;gMonsterModeSaved=false;
            gMonsterFlightActive=true;
            FHJDiag::Log(L"MOTHMAN: native flying APawn recognized; skipping ACharacter movement offsets.");
            return;
        }
        gMonsterMovement=ReadPtr(gMonsterPawn+0x330); // ACharacter.CharacterMovement
        if(!gMonsterMovement)return;
        const int species=gMonsterHUD.species;
        // Flight is a species ability, NOT a universal possession cheat.
        // Wendigo, Vampire and unknown cryptids never enter MOVE_Flying.
        gMonsterFlightActive=(species==1);
        gGroundJumpHeld=false;
        gMonsterModeSaved=Read(gMonsterMovement+0x221,gMonsterPriorMovement);
        const auto setMode=FindFunction(L"CharacterMovementComponent",L"SetMovementMode");
        if(setMode) {
            // Native physics & AnimBP stay active. Mothman flies; ground
            // cryptids walk (the AI's previous mode is restored on exit).
            uint8_t args[2]{static_cast<uint8_t>(species==1?5:1),0};
            if(!Invoke(gMonsterMovement,setMode,args))
                FHJDiag::Log(L"Monster Mode: could not set native movement mode.");
        }
        gMonsterSpeedsSaved=Read(gMonsterMovement+0x268,gMonsterOriginalWalkSpeed) &&
            Read(gMonsterMovement+0x274,gMonsterOriginalFlySpeed);
        if(!gMonsterSpeedsSaved)return;
        // SDK BP_Cryptid_Base_C.WalkSpeed and RunSpeed are double
        // properties at 0x698 and 0x6A0. Respect each species' own
        // locomotion tuning instead of forcing arbitrary 540/610/700.
        double naturalWalk=0.,naturalRun=0.;
        if(Read(gMonsterPawn+0x698,naturalWalk) &&
           std::isfinite(naturalWalk) && naturalWalk>=50. && naturalWalk<=1800.)
            gMonsterNativeWalkSpeed=static_cast<float>(naturalWalk);
        else gMonsterNativeWalkSpeed=gMonsterOriginalWalkSpeed;
        if(Read(gMonsterPawn+0x6A0,naturalRun) &&
           std::isfinite(naturalRun) && naturalRun>=50. && naturalRun<=2500.)
            gMonsterNativeRunSpeed=static_cast<float>(naturalRun);
        else gMonsterNativeRunSpeed=gMonsterNativeWalkSpeed*1.45f;
        TryWrite(gMonsterMovement+0x268,gMonsterNativeWalkSpeed);
        EnableGroundMovementSettings();
        FHJDiag::Log(L"GROUND CRYPTID: live BP_Cryptid_Base WalkSpeed/RunSpeed now drive native CharacterMovement and species locomotion.");
    }
    void PlayControlledMeleeAudio() {
        // Default: no additional audio. The creature's Server_Attack,
        // Melee/DarknessAttack and montage/notifies own their animation
        // and native sound cues. The previous filename-based auto matcher
        // accidentally selected unrelated loaded weather/thunder effects
        // for every species, so it MUST NOT choose audio automatically.
        const int selected=gMonsterAttackSoundIndex.load();
        if(selected<0)return;
        // Optional explicit sound selection only, for diagnostics. This
        // cannot fire by accident and never changes the native attack.
        int total=0;uintptr_t chunks=0;
        if(!GetObjects(total,chunks) || selected>=total)return;
        const auto asset=ObjectAt(selected,chunks);
        if(!asset || !IsSoundAssetClass(ReadPtr(asset+0x10)))return;
        const auto pc=gHumanInputController;
        const auto play=FindFunction(L"PlayerController",L"ClientPlaySound");
        if(!pc || !play)return;
        uint16_t size=0;
        if(!Read(play+0xB6,size) || size!=0x10)return;
        alignas(8) uint8_t args[0x10]{};
        constexpr float volume=1.f,pitch=1.f;
        std::memcpy(args,&asset,8);
        std::memcpy(args+8,&volume,4);
        std::memcpy(args+12,&pitch,4);
        if(!Invoke(pc,play,args))
            FHJDiag::Log(L"CRYPTID AUDIO: user-selected diagnostic sound failed; native attack remains unchanged.");
    }
    void ExecuteMonsterAbility(int slot) {
        if(slot<0 || slot>=static_cast<int>(gAbilityTicks.size()) ||
           !gMonsterActive.load() || !ReadPtr(World()+0x158) ||
           gControlledRespawnStart>=0.0)return;
        int32_t index=-1;
        if(!Read(gMonsterPawn+0x0C,index) || index!=gMonsterIndex)return;
        const auto now=GetTickCount64();
        const auto minInterval=slot==0?430ULL:280ULL;
        if(now-gAbilityTicks[static_cast<size_t>(slot)]<minInterval)return;
        int species=0;
        {
            std::lock_guard<std::mutex> guard(gMonsterStatusMutex);
            species=gMonsterHUD.species;
        }
        if(gMonsterFunctions.empty())CacheCryptidActions();
        const auto fn=CreatureSkillFunction(species,slot);
        alignas(8) uint8_t args[0x20]{};
        bool advanceStance=false;
        // Only Vampire supports the reflected invisibility/stance setters.
        // Ground Wendigo cannot inherit either skill or Mothman's flying.
        bool vampireWasInvisible=false;
        bool verifyVampire=false;
        if(species==2 && slot==2 && fn) {
            // BP_VampireBase_C.IsInvisible at 0x7D0 is the real
            // replicated gameplay flag. Never trust an internal toggle
            // that can diverge from actual character state.
            verifyVampire=Read(gMonsterPawn+0x7D0,vampireWasInvisible);
            const auto name=ObjectName(fn);
            if(name==L"SetInvisible" || name==L"SetInvisibility" ||
               name==L"AUTH_SetInvisible" || name==L"SetIsInvisible")
                args[0]=vampireWasInvisible?0:1;
        }
        if(species==2 && slot==3 && fn) {
            const auto name=ObjectName(fn);
            if(name==L"SetStance" || name==L"AUTH_SetStance") {
                const int stance=(gVampireStance+1)%3;
                std::memcpy(args,&stance,sizeof(stance));
                advanceStance=true;
            }
        }
        if(!fn) {
            if(now-gLastCreatureMethodWarning>3200) {
                gLastCreatureMethodWarning=now;
                wchar_t msg[192]{};
                swprintf_s(msg,L"CRYPTID ABILITY slot %d: no compatible reflected method; inspect CRYPTID SDK METHOD ParmSize logs.",slot);
                FHJDiag::Log(msg);
            }
            return;
        }
        const auto name=ObjectName(fn);
        if(Invoke(gMonsterPawn,fn,args)) {
            gAbilityTicks[static_cast<size_t>(slot)]=now;
            if(slot==0)PlayControlledMeleeAudio();
            if(species==2 && slot==2 && verifyVampire) {
                const bool wanted=!vampireWasInvisible;
                // Blueprint Invisible() can run a timeline; recheck
                // state after it has had time to finish, not instantly.
                gVampireVerify={gMonsterPawn,gMonsterIndex,wanted,now+1100};
                gVampireInvisible=vampireWasInvisible;
                FHJDiag::Log(L"VAMPIRE INVISIBILITY: native Invisible invoked; awaiting replicated IsInvisible state verification.");
            }
            if(advanceStance)gVampireStance=(gVampireStance+1)%3;
            wchar_t msg[220]{};
            swprintf_s(msg,L"CRYPTID ABILITY slot %d: invoked native %s; effects must be verified on the host.",slot,name.c_str());
            FHJDiag::Log(msg);
        } else {
            wchar_t msg[160]{};
            swprintf_s(msg,L"CRYPTID ABILITY: ProcessEvent failed for %s.",name.c_str());
            FHJDiag::Log(msg);
        }
    }
    void VerifyVampireInvisibility(ULONGLONG now) {
        if(!gVampireVerify.verifyAt || now<gVampireVerify.verifyAt)return;
        const auto pending=gVampireVerify;
        gVampireVerify=VampireInvisibilityVerification{};
        if(!gMonsterActive.load() || !ReadPtr(World()+0x158) ||
           !pending.pawn || gMonsterPawn!=pending.pawn)return;
        int32_t currentId=-1;
        bool actual=false;
        if(!Read(pending.pawn+0xC,currentId) ||
           currentId!=pending.actorIndex ||
           !Read(pending.pawn+0x7D0,actual))return;
        if(actual==pending.wanted) {
            gVampireInvisible=actual;
            FHJDiag::Log(actual?
                L"VAMPIRE INVISIBILITY: native function confirmed replicated IsInvisible=true.":
                L"VAMPIRE INVISIBILITY: native function confirmed replicated IsInvisible=false.");
            return;
        }
        // The native no-arg Invisible() sometimes fires without
        // changing the replicated field. The SDK supplies matching
        // multicast appearance functions and the exact server-owned
        // replicated bool at 0x7D0. Use those as a guarded fallback.
        const auto visual=NoArgCryptidAction(pending.wanted?
            std::initializer_list<const wchar_t*>{L"All_Disapear"}:
            std::initializer_list<const wchar_t*>{L"All_Reappear"});
        if(!visual) {
            FHJDiag::Log(L"VAMPIRE INVISIBILITY: native state unchanged; no verified multicast appearance function available.");
            return;
        }
        if(!TryWrite(pending.pawn+0x7D0,pending.wanted)) {
            FHJDiag::Log(L"VAMPIRE INVISIBILITY: replicated flag write rejected; no appearance effect invoked.");
            return;
        }
        bool readback=false;
        if(!Read(pending.pawn+0x7D0,readback) ||
           readback!=pending.wanted)return;
        const bool dispatched=Invoke(pending.pawn,visual,nullptr);
        // OnRep updates local materials; other clients get the property
        // through normal replication and multicast appearance event.
        const auto onRep=NoArgCryptidAction({L"OnRep_IsInvisiblie"});
        if(onRep)Invoke(pending.pawn,onRep,nullptr);
        ForceActorNetUpdate(pending.pawn);
        gVampireInvisible=readback;
        FHJDiag::Log(dispatched?
            L"VAMPIRE INVISIBILITY: fallback applied replicated IsInvisible and native All_Disapear/All_Reappear + OnRep. Verify with second player.":
            L"VAMPIRE INVISIBILITY: replicated IsInvisible changed but native appearance multicast failed. Check runtime.");
    }
    void ResetControlledRespawnSession() {
        gControlledCryptidClass=0;
        gControlledRespawnSpawner=0;gControlledRespawnSpawnerIndex=-1;
        gControlledExistingIds.clear();
        gControlledRespawnStart=-1.0;gControlledRespawnRemaining=0.0;
        gControlledSpawnRequested=false;gNextControlledRespawnScan=0;
    }
    // World game time is supplied by an Unreal UFunction (pauses with the
    // actual world). Only use the OS clock if this UE reflection is absent.
    double ControlledWorldSeconds(uintptr_t world) {
        static uintptr_t timeFn=0,system=0;
        static ULONGLONG retry=0;
        const auto now=GetTickCount64();
        if(!timeFn && now>=retry) {
            retry=now+10000;
            timeFn=FindFunction(L"KismetSystemLibrary",L"GetGameTimeInSeconds");
            system=FindObjectByName(L"Default__KismetSystemLibrary");
        }
        if(timeFn && system) {
            uint16_t size=0,ret=0;
            if(Read(timeFn+0xB6,size) && Read(timeFn+0xB8,ret) &&
               ret==8 && (size==12 || size==16)) {
                alignas(8) uint8_t args[0x18]{};
                std::memcpy(args,&world,8);
                if(Invoke(system,timeFn,args)) {
                    double value=0;
                    if(size==16)std::memcpy(&value,args+8,8);
                    else {float v=0;std::memcpy(&v,args+8,4);value=v;}
                    if(std::isfinite(value) && value>=0)return value;
                }
            }
        }
        return static_cast<double>(now)*.001;
    }
    void CaptureControlledCryptidSession() {
        ResetControlledRespawnSession();
        gControlledCryptidClass=ReadPtr(gMonsterPawn+0x10);
        Position(gMonsterPawn,gControlledLastPosition);
        const auto world=World();
        double bestDistance=1e30;
        int total=0;
        for(const auto& level:GetWorldLevels(world,total))
            for(int i=0;i<level.count;++i) {
                const auto actor=ReadPtr(level.data+static_cast<size_t>(i)*8);
                if(!actor)continue;
                const auto cls=ReadPtr(actor+0x10);
                if(cls==gControlledCryptidClass) {
                    int32_t id=-1;
                    if(Read(actor+0xC,id)&&id>=0)
                        gControlledExistingIds.insert(id);
                }
                if(ClassName(cls)==L"BP_MobSpawner_C") {
                    Vec3 pos{};
                    if(!Position(actor,pos))continue;
                    const double dist=std::hypot(
                        std::hypot(pos.x-gControlledLastPosition.x,
                                   pos.y-gControlledLastPosition.y),
                        pos.z-gControlledLastPosition.z);
                    if(dist<bestDistance) {
                        bestDistance=dist;
                        gControlledRespawnSpawner=actor;
                        Read(actor+0xC,gControlledRespawnSpawnerIndex);
                    }
                }
            }
        FHJDiag::Log(gControlledRespawnSpawner?
            L"CRYPTID RESPAWN: captured creature class and nearest native map spawner.":
            L"CRYPTID RESPAWN: no native map spawner found; will follow an existing game respawn only.");
    }
    bool RebindControlledCryptid(uintptr_t newPawn) {
        RestoreLocalMothmanSmoke(); // restore the old actor before reassignment
        RestoreLocalMothmanRender();
        int32_t id=-1;
        if(!newPawn || ReadPtr(newPawn+0x10)!=gControlledCryptidClass ||
           !Read(newPawn+0xC,id) || id<0)return false;
        // An old AI/controller pointer must never be accessed after the old
        // cryptid has been destroyed. The new pawn supplies fresh components.
        gMonsterBrain=0;gMonsterAIController=0;gMonsterBrainWasRunning=false;
        gMonsterMovement=0;gMonsterSpeedsSaved=false;gMonsterModeSaved=false;
        gMonsterPathActive=false;gMonsterLastMoveRequest=0;
        gMonsterPawn=newPawn;gMonsterIndex=id;
        gControlledExistingIds.insert(id);
        gControlledRespawnStart=-1.0;gControlledRespawnRemaining=0.0;
        gControlledSpawnRequested=false;
        gChaseSmoothedValid=false;gLastMotionTick=0;
        UpdateMonsterHUD();
        StopMonsterAI();ConfigureMonsterMovement();CacheCryptidActions();
        UpdateMonsterHUD(); // respawned creature's exact abilities replace the previous HUD
        const auto world=World();
        const auto gi=ReadPtr(world+0x1D8);
        const auto locals=ReadPtr(gi+0x38);
        const auto local=ReadPtr(locals);
        const auto pc=ReadPtr(local+0x30);
        if(pc && gChaseCamera)SetCameraViewTarget(pc,gChaseCamera);
        HideHumanHUD();
        FHJDiag::Log(L"CRYPTID RESPAWN: rebound creature, camera, HUD, AI movement and ability functions; human pawn preserved.");
        return true;
    }
    bool MonitorControlledRespawn(uintptr_t world,ULONGLONG now) {
        if(!gMonsterActive.load())return true;
        int32_t liveId=-1;
        double hp=-1.0;
        const bool valid=gMonsterPawn &&
            Read(gMonsterPawn+0xC,liveId) && liveId==gMonsterIndex &&
            ReadPtr(gMonsterPawn+0x10)==gControlledCryptidClass;
        const bool isMothman=ClassName(gControlledCryptidClass)==L"BP_FlyingPawnTest_C";
        // Engine.Actor.IsActorBeingDestroyed is native and returns a bool;
        // it provides a safer Mothman liveness signal than reading invalid
        // base-cryptid health bytes from this unrelated APawn subtype.
        bool beingDestroyed=false;
        if(valid) {
            static uintptr_t isDestroying=0;
            if(!isDestroying)
                isDestroying=FindFunction(L"Actor",L"IsActorBeingDestroyed");
            uint16_t size=0;
            if(isDestroying && Read(isDestroying+0xB6,size) && size==1)
                Invoke(gMonsterPawn,isDestroying,&beingDestroyed);
        }
        const bool alive=valid && !beingDestroyed &&
            (isMothman || (Read(gMonsterPawn+0x710,hp) &&
                           std::isfinite(hp) && hp>0));
        if(alive) {
            if(gControlledRespawnStart>=0.0)RebindControlledCryptid(gMonsterPawn);
            else Position(gMonsterPawn,gControlledLastPosition);
            return true;
        }
        const double worldNow=ControlledWorldSeconds(world);
        if(gControlledRespawnStart<0.0) {
            gControlledRespawnStart=worldNow;
            gControlledSpawnRequested=false;
            gNextControlledRespawnScan=0;
            StopCryptidPath();
            FHJDiag::Log(L"CRYPTID RESPAWN: death/despawn detected; host countdown started. Camera and human HUD ownership preserved.");
        }
        constexpr double kRespawnDelay=15.0;
        const double elapsed=std::max(0.0,worldNow-gControlledRespawnStart);
        gControlledRespawnRemaining=std::max(0.0,kRespawnDelay-elapsed);
        if(now>=gNextControlledRespawnScan) {
            gNextControlledRespawnScan=now+350;
            int total=0;
            for(const auto& level:GetWorldLevels(world,total)) {
                for(int i=0;i<level.count;++i) {
                    const auto actor=ReadPtr(level.data+static_cast<size_t>(i)*8);
                    if(!actor || ReadPtr(actor+0x10)!=gControlledCryptidClass)
                        continue;
                    int32_t id=-1;
                    if(!Read(actor+0xC,id)||id<0)continue;
                    if(valid && actor==gMonsterPawn)continue;
                    if(gControlledExistingIds.count(id))continue;
                    double candidateHp=0.0;
                    if(!isMothman && (!Read(actor+0x710,candidateHp) ||
                       !std::isfinite(candidateHp)||candidateHp<=0.0))continue;
                    if(RebindControlledCryptid(actor))return true;
                }
            }
        }
        if(elapsed>=kRespawnDelay && !gControlledSpawnRequested) {
            gControlledSpawnRequested=true; // one request; no spawn flood
            int32_t spawnerId=-1;
            const bool spawnerValid=gControlledRespawnSpawner &&
                Read(gControlledRespawnSpawner+0xC,spawnerId) &&
                spawnerId==gControlledRespawnSpawnerIndex &&
                ClassName(ReadPtr(gControlledRespawnSpawner+0x10))==L"BP_MobSpawner_C";
            if(!spawnerValid || !ReadPtr(world+0x158)) {
                FHJDiag::Log(L"CRYPTID RESPAWN: host native spawner unavailable; waiting for game's own respawn without forcing an actor.");
            } else {
                const auto fn=FindFunction(L"BP_MobSpawner_C",L"TriggerSpawn");
                const auto spawner=gControlledRespawnSpawner;
                const auto saved=ReadPtr(spawner+0x2B8);
                bool overridden=false;
                if(saved && saved!=gControlledCryptidClass)
                    overridden=TryWrite(spawner+0x2B8,gControlledCryptidClass);
                alignas(8) uint8_t args[0xA0]{};
                const bool requested=fn && Invoke(spawner,fn,args);
                if(overridden && !TryWrite(spawner+0x2B8,saved))
                    FHJDiag::Log(L"CRYPTID RESPAWN WARNING: map spawner MobClass restore failed.");
                FHJDiag::Log(requested?
                    L"CRYPTID RESPAWN: invoked native map TriggerSpawn after 15-second world countdown; tracking replacement actor.":
                    L"CRYPTID RESPAWN: TriggerSpawn unavailable or rejected; continue monitoring the map for game respawn.");
            }
        }
        UpdateMonsterHUD();
        return false; // camera and human control stay in cryptid mode
    }
    void TickNativeMothmanFlight(uintptr_t pawn,const Vec3& desiredInput,
                                  bool boosting,double dt,ULONGLONG now) {
        // Exact SDK: BP_FlyingPawnTest_C inherits APawn and exposes
        // FlightTarget at 0x400. This path drives its own ReceiveTick /
        // physics and MothmanCreature_...AnimBlueprint rather than invoking
        // K2_SetActorLocation on every movement frame.
        int32_t id=-1;
        Vec3 current{};
        if(!pawn || !Read(pawn+0x0C,id) || !Position(pawn,current))return;
        if(gMothFlight.pawn!=pawn || gMothFlight.objectIndex!=id) {
            gMothFlight=NativeMothmanFlight{};
            gMothFlight.pawn=pawn;
            gMothFlight.objectIndex=id;
            gMothFlight.lastPosition=current;
            gMothFlight.lastMotionSeen=now;
            gMothFlight.positionValid=true;
            FHJDiag::Log(L"MOTHMAN FLIGHT: switched to native FlightTarget steering and actual-velocity animation blending.");
        }
        const double speed=boosting?1100.0:690.0;
        const double gain=1.0-std::exp(-std::clamp(5.5*dt,0.0,1.0));
        gMothFlight.smoothedVelocity.x+=(desiredInput.x*speed-
            gMothFlight.smoothedVelocity.x)*gain;
        gMothFlight.smoothedVelocity.y+=(desiredInput.y*speed-
            gMothFlight.smoothedVelocity.y)*gain;
        gMothFlight.smoothedVelocity.z+=(desiredInput.z*speed-
            gMothFlight.smoothedVelocity.z)*gain;
        const double commanded=std::hypot(std::hypot(
            gMothFlight.smoothedVelocity.x,gMothFlight.smoothedVelocity.y),
            gMothFlight.smoothedVelocity.z);
        // Look ahead from the real current location while keeping the flight
        // target continuous as input accelerates/decelerates. Idle targets the
        // current position to avoid drifting after releasing the keys.
        const double leadSeconds=1.10;
        const Vec3 target{
            current.x+gMothFlight.smoothedVelocity.x*leadSeconds,
            current.y+gMothFlight.smoothedVelocity.y*leadSeconds,
            current.z+gMothFlight.smoothedVelocity.z*leadSeconds};
        const bool targetAccepted=TryWrite(pawn+0x400,target);
        double nativeSpeed=0.0;
        if(Read(pawn+0x420,nativeSpeed) &&
           (!std::isfinite(nativeSpeed)||nativeSpeed<=1.0)) {
            const double initial=690.0;
            TryWrite(pawn+0x420,initial);
        }
        // Derive anim blend parameters from observed actor motion, rather
        // than moving the mesh with an artificial animation montage.
        const double observationDt=std::max(dt,0.005);
        const Vec3 delta{current.x-gMothFlight.lastPosition.x,
                         current.y-gMothFlight.lastPosition.y,
                         current.z-gMothFlight.lastPosition.z};
        const double progress=std::hypot(std::hypot(delta.x,delta.y),delta.z);
        const double measuredH=std::clamp(std::hypot(delta.x,delta.y)/
            observationDt,0.0,2500.0);
        const double measuredFull=std::clamp(progress/observationDt,
            0.0,2500.0);
        if(progress>2.0 || commanded<30.0)gMothFlight.lastMotionSeen=now;
        if(!targetAccepted)gMothFlight.lastMotionSeen=now;
        if(targetAccepted && commanded>120.0 &&
           now-gMothFlight.lastMotionSeen>650) {
            // Only a bounded emergency swept move is used if the game never
            // consumes FlightTarget. This is not the normal animation path.
            const Vec3 fallback{
                current.x+gMothFlight.smoothedVelocity.x*dt,
                current.y+gMothFlight.smoothedVelocity.y*dt,
                current.z+gMothFlight.smoothedVelocity.z*dt};
            SetActorLocationSafe(pawn,fallback,true,false);
            if(!gMothFlight.loggedFallback) {
                gMothFlight.loggedFallback=true;
                FHJDiag::Log(L"MOTHMAN FLIGHT: native pawn stalled; swept recovery engaged. Inspect FlightTarget/ReceiveTick in live dump.");
            }
        }
        gMothFlight.lastPosition=current;

        // Exact Mothman SDK: USkeletalMeshComponent at Pawn+0x3D0,
        // AnimScriptInstance +0x8A0, AnimBlueprint IsFlying 0x1310,
        // HSpeed 0x1314, FSpeed 0x1318. These feed the original blend graph;
        // keep wings/physical animation and native character visuals intact.
        const auto mesh=ReadPtr(pawn+0x3D0);
        const auto anim=ReadPtr(mesh+0x8A0);
        if(anim && ClassName(ReadPtr(anim+0x10))==
           L"MothmanCreature_Body_Skeleton_AnimBlueprint_C") {
            const bool isFlying=true;
            const float h=static_cast<float>(measuredH);
            const float v=static_cast<float>(measuredFull);
            TryWrite(anim+0x1310,isFlying);
            TryWrite(anim+0x1314,h);
            TryWrite(anim+0x1318,v);
        }
    }
    void FreeflightTick() {
        const ULONGLONG now=GetTickCount64();
        VerifyVampireInvisibility(now);
        const ULONGLONG elapsed=gLastMotionTick?now-gLastMotionTick:16;
        gLastMotionTick=now;
        const auto world=World();
        if(!world || world!=gPrevWorld)return;
        // Respawn must keep progressing even with the journal open.
        if(gMonsterActive.load() && !MonitorControlledRespawn(world,now))return;
        if(gMonsterActive.load() || gLocalMothmanSmoke.component)
            SyncLocalMothmanSmoke();
        if(gMonsterActive.load() || gMothRender.pawn)
            SyncLocalMothmanRender();
        if(now-gLastPossessionClarityRefresh>=600) {
            gLastPossessionClarityRefresh=now;
            if(gMonsterActive.load() || gPossessionClarity.active) {
                const auto instance=ReadPtr(world+0x1D8);
                const auto locals=ReadPtr(instance+0x38);
                const auto local=ReadPtr(locals);
                const auto pc=ReadPtr(local+0x30);
                if(pc) {
                    if(gMonsterActive.load())RefreshPossessionClarity(pc);
                    else RestorePossessionClarity(pc);
                }
            }
        }
        if(!gWindow || gJournalOpen.load() || GetForegroundWindow()!=gWindow)return;
        const bool monster=gMonsterActive.load();
        const auto pawn=monster?gMonsterPawn:LocalPawn();
        if(!pawn || (!monster && !gNoclipEnabled))return;
        int32_t index=-1;
        if(!Read(pawn+0x0C,index) || (monster && index!=gMonsterIndex)) {
            if(monster) {
                gMonsterActive.store(false);
                const auto local=ReadPtr(ReadPtr(world+0x1D8)+0x38);
                const auto pc=ReadPtr(ReadPtr(local)+0x30);
                RestoreHumanControls();
                RestoreHumanHUD();
                ReleaseMonsterCamera(pc,gHumanPawn);
                RestoreMonsterAI();
                gMonsterPawn=0;gMonsterIndex=-1;
                FHJDiag::Log(L"Monster actor invalidated; human controls and camera restored.");
            }
            return;
        }
        // AI-owned cryptids are driven by AIController::MoveToLocation so the
        // CharacterMovement / AnimBlueprint locomotion receives a real path.
        // Local noclip is separate and uses world-space movement.
        const double dt=std::clamp(static_cast<double>(elapsed)*.001,0.0,.045);
        const auto camera=GetCamera(world);
        const double yaw=(monster?gMonsterYaw:camera.rot.yaw)*kPi/180.0;
        const double pitch=(monster?gMonsterPitch:camera.rot.pitch)*kPi/180.0;
        const double cy=std::cos(yaw),sy=std::sin(yaw);
        const double cp=std::cos(pitch),sp=std::sin(pitch);
        auto down=[](int vk)->bool{return (GetAsyncKeyState(vk)&0x8000)!=0;};
        const double front=static_cast<double>(down('W'))-static_cast<double>(down('S'));
        const double strafe=static_cast<double>(down('D'))-static_cast<double>(down('A'));
        const bool boost=down(VK_SHIFT);
        if(monster) {
            // Windows messages are consumed to suppress human-pawn actions.
            // Detect key-down edges ourselves, including raw-input bindings.
            constexpr int keys[3]{'Q','E','R'};
            for(int i=0;i<3;++i) {
                const bool pressed=down(keys[i]);
                if(pressed && !gAbilityKeyDown[static_cast<size_t>(i)])
                    ExecuteMonsterAbility(i+1);
                gAbilityKeyDown[static_cast<size_t>(i)]=pressed;
            }
        }
        int species=0;
        if(monster) {
            std::lock_guard<std::mutex> guard(gMonsterStatusMutex);
            species=gMonsterHUD.species;
        }
        // Only the human's independent noclip and Mothman's verified
        // winged-control profile may ascend/descend. Wendigo must stay grounded.
        const bool flying=!monster || species==1;
        const double ascend=flying?
            static_cast<double>(down(VK_SPACE))-
            static_cast<double>(down(VK_CONTROL)):0.0;
        Vec3 input{cy*front-sy*strafe,sy*front+cy*strafe,ascend};
        if(monster && flying) {
            // Mothman follows camera pitch. Noclip intentionally stays on the
            // XY plane for WASD so looking up/down never kills forward motion.
            input.x*=cp; input.y*=cp;
            input.z+=sp*front;
        }
        const double length=std::sqrt(input.x*input.x+input.y*input.y+input.z*input.z);
        if(length>1e-3) {
            input.x/=length;input.y/=length;input.z/=length;
        }
        if(!monster) {
            if(pawn!=gNoclipPawn) {
                if(gNoclipIgnoredMove && gNoclipController) {
                    const auto allow=FindFunction(L"Controller",L"SetIgnoreMoveInput");
                    if(allow){bool no=false;Invoke(gNoclipController,allow,&no);}
                }
                gNoclipController=0;gNoclipIgnoredMove=false;
                gNoclipEnabled=false;gNoclipPawn=0;gNoclipPositionValid=false;
                gFlySpeedSaved=false;gOriginalFlyMovement=0;
                FHJDiag::Log(L"NOCLIP: player changed; restored controller input and stopped free-flight safely.");
                return;
            }
            if(!gNoclipPositionValid)
                gNoclipPositionValid=Position(pawn,gNoclipPosition);
            if(gNoclipPositionValid) {
                // No keys => exact position hold, never float or sink.
                const double speed=boost?1850.0:1150.0;
                gNoclipPosition.x+=input.x*speed*dt;
                gNoclipPosition.y+=input.y*speed*dt;
                gNoclipPosition.z+=input.z*speed*dt;
                const auto movement=ReadPtr(pawn+0x330);
                const auto stop=FindFunction(L"MovementComponent",L"StopMovementImmediately");
                if(movement && stop)Invoke(movement,stop,nullptr);
                if(!SetActorLocationSafe(pawn,gNoclipPosition)) {
                    static ULONGLONG warned=0;
                    if(now-warned>5000) {
                        warned=now;
                        FHJDiag::Log(L"NOCLIP: engine rejected world-position hold; inspect host authority.");
                    }
                }
            }
        } else {
            // Mothman is the only aerial cryptid in this control profile.
            // Ground species never receive direct no-clip or vertical movement.
            gMonsterFlightActive=(species==1);
            if(gMonsterFlightActive) {
                StopCryptidPath();
                TickNativeMothmanFlight(pawn,input,boost,dt,now);
            } else {
                // Space invokes Character.Jump rather than enabling flight.
                // This uses normal walk physics and leaves collision intact.
                const bool jump=down(VK_SPACE);
                if(jump && !gGroundJumpHeld) {
                    const auto jumpFn=FindFunction(L"Character",L"Jump");
                    uint16_t params=0;
                    if(jumpFn && Read(jumpFn+0xB6,params) && params==0)
                        Invoke(pawn,jumpFn,nullptr);
                }
                gGroundJumpHeld=jump;
                Vec3 horizontal{input.x,input.y,0.0};
                const double lenXY=std::hypot(horizontal.x,horizontal.y);
                if(lenXY>1e-3) {
                    horizontal.x/=lenXY;horizontal.y/=lenXY;
                    DriveGroundCryptid(horizontal,boost,now);
                } else {
                    StopCryptidPath();
                    // On release, try direct native input again rather
                    // than being locked in navigation fallback forever.
                    gGroundDrive.sampleValid=false;
                    gGroundDrive.useNavFallback=false;
                }
            }
        }
        const auto movement=monster && species==1 ? 0 : ReadPtr(pawn+0x330);
        if(movement) {
            if(!monster && gNoclipEnabled) {
                uint8_t mode=0;
                if(Read(movement+0x221,mode) && mode!=5) {
                    const auto setMode=FindFunction(L"CharacterMovementComponent",L"SetMovementMode");
                    if(setMode) {
                        uint8_t flight[2]{5,0};
                        Invoke(movement,setMode,flight);
                    }
                }
            }
            if(monster && species!=1) {
                // If the game or a previous build left a ground cryptid in
                // MOVE_Flying, force it back into normal ground physics.
                uint8_t mode=0;
                if(Read(movement+0x221,mode) && mode==5) {
                    const auto set=FindFunction(L"CharacterMovementComponent",L"SetMovementMode");
                    if(set) {
                        uint8_t walking[2]{1,0};
                        Invoke(movement,set,walking);
                    }
                }
            }
            if(monster && species!=1 && gMonsterSpeedsSaved) {
                const float authentic=boost?
                    gMonsterNativeRunSpeed:gMonsterNativeWalkSpeed;
                float current=0.f;
                if(authentic>0.f && Read(movement+0x268,current) &&
                   std::fabs(current-authentic)>1.f)
                    TryWrite(movement+0x268,authentic);
            } else if(!monster && gFlySpeedSaved && movement==gOriginalFlyMovement) {
                const float wanted=boost?1850.f:1150.f;
                float current=0;
                if(Read(movement+0x274,current) && std::fabs(current-wanted)>.5f)
                    TryWrite(movement+0x274,wanted);
            }
        }
        if(!monster)return;
        // Keep the human pawn possessed. No Controller.Possess/UnPossess calls:
        // the game's single-player GameMode may interpret unpossession as death.
        const auto instance=ReadPtr(world+0x1D8);
        const auto localPlayers=ReadPtr(instance+0x38);
        const auto local=ReadPtr(localPlayers);
        const auto pc=ReadPtr(local+0x30);
        if(!pc || ReadPtr(pc+0x350)!=gHumanPawn) {
            gMonsterActive.store(false);
            RestoreHumanControls();
            RestoreHumanHUD();
            ReleaseMonsterCamera(pc,ReadPtr(pc+0x350));
            RestoreMonsterAI();
            gMonsterPawn=0;gMonsterIndex=-1;
            FHJDiag::Log(L"Monster control stopped: human controls restored after pawn replacement.");
            return;
        }
        static ULONGLONG hudTick=0;
        if(now-hudTick>300) {
            hudTick=now;
            HideHumanHUD();
        }
        // Ground cryptids face camera yaw. Mothman's native
        // TorqueTowardsVelocity and wing animation need control of its
        // orientation: never hard-snap the aerial pawn to camera yaw.
        if(species!=1 && length<1e-3) {
            // While navigating, let AIController/path-following rotate the
            // creature naturally. Only face the camera while stationary;
            // per-frame hard yaw writes interfere with native turning.
            const Rot desired{0.0,gMonsterYaw,0.0};
            if(FindFunction(L"Actor",L"K2_SetActorRotation"))
                SetActorRotationSafe(pawn,desired);
        }
        if(gChaseCamera) {
            Vec3 location{};
            if(Position(pawn,location)) {
                double distance=650.0,height=245.0;
                if(species==1){distance=810; height=340;}
                else if(species==2){distance=625;height=225;}
                else if(species==3){distance=735;height=290;}
                const Vec3 pivot{location.x,location.y,location.z+height};
                const Vec3 goal{
                    pivot.x-cy*distance,
                    pivot.y-sy*distance,
                    pivot.z-std::sin(pitch)*distance*.18
                };
                Vec3 safeGoal{};
                if(!ConstrainChaseCamera(pivot,goal,safeGoal)) {
                    // Trace unavailable: avoid burying the camera in the
                    // creature. Collision is not guaranteed in this fallback.
                    const double close=420.0;
                    safeGoal={pivot.x-cy*close,pivot.y-sy*close,
                              pivot.z-std::sin(pitch)*close*.18};
                }
                if(!gChaseSmoothedValid) {
                    gChaseSmoothedPos=safeGoal;
                    gChaseSmoothedValid=true;
                } else {
                    // Shorter interpolation tail reduces perceived ghosting
                    // and the camera visibly lagging behind moving cryptids.
                    const double alpha=1.-std::exp(
                    -static_cast<double>(gCryptidCameraFollow.load())*dt);
                    gChaseSmoothedPos.x+=(safeGoal.x-gChaseSmoothedPos.x)*alpha;
                    gChaseSmoothedPos.y+=(safeGoal.y-gChaseSmoothedPos.y)*alpha;
                    gChaseSmoothedPos.z+=(safeGoal.z-gChaseSmoothedPos.z)*alpha;
                }
                // A smoothed trajectory can pass through a wall around a
                // corner even when its destination was trace-safe. Re-probe
                // the actual interpolated position before every camera move.
                Vec3 actual=gChaseSmoothedPos;
                if(ConstrainChaseCamera(pivot,actual,actual))
                    gChaseSmoothedPos=actual;
                SetActorLocationSafe(gChaseCamera,gChaseSmoothedPos);
                SetActorRotationSafe(gChaseCamera,{gMonsterPitch,gMonsterYaw,0.0});
            }
        }
        UpdateMonsterHUD();
        if(down(VK_LBUTTON) && now-gLastAttackTick>=700) {
            gLastAttackTick=now;
            ExecuteMonsterAbility(0);
        }
    }

    template<typename T> bool TryWrite(uintptr_t addr, const T& value) noexcept {
        if(addr<0x10000 || addr>0x00007FFFFFFFFFFFULL)return false;
        __try { std::memcpy(reinterpret_cast<void*>(addr),&value,sizeof(T)); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void ForceActorNetUpdate(uintptr_t pawn) {
        if(!pawn || !ReadPtr(World()+0x158))return;
        const auto net=FindFunction(L"Actor",L"ForceNetUpdate");
        if(net)Invoke(pawn,net,nullptr);
    }
    bool ValidPlayerPawn(uintptr_t pawn,int32_t& index) {
        index=-1;
        return pawn && Read(pawn+0x0C,index) && index>=0 &&
            ClassKind(ReadPtr(pawn+0x10))==1;
    }
    bool ApplyGodProtection(uintptr_t pawn,bool enabled) {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"God Mode rejected: requires authoritative host GameMode.");
            return false;
        }
        int32_t index=-1;
        if(!ValidPlayerPawn(pawn,index)) {
            FHJDiag::Log(L"God Mode rejected: selected pawn is not a live PlayerCharacter_C.");
            return false;
        }
        auto it=gGodBackups.find(pawn);
        if(it!=gGodBackups.end() && it->second.index!=index) {
            gGodBackups.erase(it);
            it=gGodBackups.end();
        }
        if(enabled) {
            if(it==gGodBackups.end()) {
                GodBackup base{};
                base.index=index;
                if(!Read(pawn+0x8B9,base.priorInvuln) ||
                   !Read(pawn+0x8A0,base.protectedHealth) ||
                   !std::isfinite(base.protectedHealth)) {
                    FHJDiag::Log(L"God Mode failed: unable to read Invuln/Health baseline.");
                    return false;
                }
                it=gGodBackups.emplace(pawn,base).first;
            }
            const bool yes=true;
            if(!TryWrite(pawn+0x8B9,yes)) {
                FHJDiag::Log(L"God Mode failed: unable to set replicated Invuln property.");
                return false;
            }
            bool confirmed=false;
            Read(pawn+0x8B9,confirmed);
            if(confirmed)ForceActorNetUpdate(pawn);
            FHJDiag::Log(confirmed ?
                L"God Mode ON: server Invuln verified true; health guard armed." :
                L"God Mode FAILED: Invuln did not read back true.");
            return confirmed;
        }
        if(it==gGodBackups.end()) {
            FHJDiag::Log(L"God Mode already OFF for selected hunter.");
            return true;
        }
        bool restore=it->second.priorInvuln;
        // If fall protection has its temporary midair guard, do not disable it.
        auto fall=gFallBackups.find(pawn);
        if(fall!=gFallBackups.end() && fall->second.airborne)restore=true;
        bool ok=TryWrite(pawn+0x8B9,restore);
        gGodBackups.erase(it);
        if(ok)ForceActorNetUpdate(pawn);
        FHJDiag::Log(ok ?
            L"God Mode OFF: prior server Invuln state restored." :
            L"God Mode OFF failed to restore Invuln; check game.");
        return ok;
    }
    void MaintainGodProtection() {
        // Runs only on the game window message thread, on the authoritative host.
        if(!ReadPtr(World()+0x158))return;
        for(auto it=gGodBackups.begin();it!=gGodBackups.end();) {
            auto pawn=it->first;
            auto& base=it->second;
            int32_t index=-1;
            if(!ValidPlayerPawn(pawn,index) || index!=base.index) {
                it=gGodBackups.erase(it);
                continue;
            }
            bool changed=false;
            bool invuln=false;
            if(Read(pawn+0x8B9,invuln) && !invuln) {
                const bool yes=true;
                changed|=TryWrite(pawn+0x8B9,yes);
            }
            // Some Blueprint health paths can bypass the Invuln check.
            // Restore replicated Health if such a path still reduces it.
            double health=0,maxHealth=0;
            if(Read(pawn+0x8A0,health) && Read(pawn+0x8A8,maxHealth) &&
               std::isfinite(health) && std::isfinite(maxHealth) &&
               base.protectedHealth>0 && maxHealth>0 && health<base.protectedHealth) {
                const double restore=std::min(base.protectedHealth,maxHealth);
                if(TryWrite(pawn+0x8A0,restore)) {
                    changed=true;
                    if(GetTickCount64()-base.lastCorrection>2500) {
                        base.lastCorrection=GetTickCount64();
                        FHJDiag::Log(L"God Mode: corrected health loss despite Invuln; server health restored.");
                    }
                }
            }
            if(changed)ForceActorNetUpdate(pawn);
            ++it;
        }
    }
    FallBackup* GetFallBackup(uintptr_t pawn) {
        int32_t index=-1;
        if(!ValidPlayerPawn(pawn,index))return nullptr;
        auto it=gFallBackups.find(pawn);
        if(it!=gFallBackups.end()) {
            if(it->second.index==index)return &it->second;
            gFallBackups.erase(it);
        }
        FallBackup backup{};
        backup.index=index;
        if(!Read(pawn+0x930,backup.safe) ||
           !Read(pawn+0x938,backup.maximum) ||
           !Read(pawn+0x940,backup.damage) ||
           !std::isfinite(backup.safe) || !std::isfinite(backup.maximum) ||
           !std::isfinite(backup.damage)) {
            FHJDiag::Log(L"No fall damage: could not read three SDK fall properties.");
            return nullptr;
        }
        FHJDiag::Log(L"No fall damage: saved original fall properties independently of movement component.");
        return &gFallBackups.emplace(pawn,backup).first->second;
    }
    void ApplyFallProtection(uintptr_t pawn,bool enabled) {
        if(!enabled) {
            auto it=gFallBackups.find(pawn);
            if(it==gFallBackups.end())return;
            const auto backup=it->second;
            bool ok=TryWrite(pawn+0x930,backup.safe);
            ok &= TryWrite(pawn+0x938,backup.maximum);
            ok &= TryWrite(pawn+0x940,backup.damage);
            bool desired=backup.priorInvuln;
            if(gGodBackups.find(pawn)!=gGodBackups.end())desired=true;
            if(backup.airborne || backup.landingGraceUntil)ok &= TryWrite(pawn+0x8B9,desired);
            if(ok)ForceActorNetUpdate(pawn);
            gFallBackups.erase(it);
            if(!ok)FHJDiag::Log(L"No fall damage: failed restoring some original values.");
            return;
        }
        auto base=GetFallBackup(pawn);
        if(!base)return;
        // Keep the safety threshold above any normally reachable falling height.
        constexpr double protectedSafe=1000000000.0;
        constexpr double protectedMax=10000000000.0;
        constexpr double protectedDamage=0.0;
        double safe=0,max=0,damage=-1;
        bool changed=false;
        if(Read(pawn+0x930,safe) && safe!=protectedSafe)
            changed|=TryWrite(pawn+0x930,protectedSafe);
        if(Read(pawn+0x938,max) && max!=protectedMax)
            changed|=TryWrite(pawn+0x938,protectedMax);
        if(Read(pawn+0x940,damage) && damage!=protectedDamage)
            changed|=TryWrite(pawn+0x940,protectedDamage);
        double afterSafe=0,afterMax=0,afterDamage=-1;
        const bool verified=Read(pawn+0x930,afterSafe) &&
            Read(pawn+0x938,afterMax) && Read(pawn+0x940,afterDamage) &&
            afterSafe==protectedSafe && afterMax==protectedMax &&
            afterDamage==protectedDamage;
        if(verified && !base->verified)
            FHJDiag::Log(L"NO FALL DAMAGE: all three live fall properties VERIFIED after write.");
        if(!verified && GetTickCount64()-base->lastVerifyWarning>5000) {
            base->lastVerifyWarning=GetTickCount64();
            FHJDiag::Log(L"NO FALL DAMAGE: fall properties did not match expected values; check SDK offsets.");
        }
        base->verified=verified;
        bool falling=false;
        Read(pawn+0x968,falling); // PlayerCharacter_C::IsFalling
        if(falling) {
            if(!base->airborne) {
                base->airborne=true;
                base->landingGraceUntil=0;
                const auto god=gGodBackups.find(pawn);
                if(god!=gGodBackups.end())base->priorInvuln=god->second.priorInvuln;
                else Read(pawn+0x8B9,base->priorInvuln);
                Read(pawn+0x8A0,base->healthBeforeFall);
            }
            bool inv=false;
            if(Read(pawn+0x8B9,inv) && !inv) {
                const bool yes=true;
                changed|=TryWrite(pawn+0x8B9,yes);
            }
        } else {
            // Remote clients may send a server health update AFTER the initial
            // landing frame. Hold a short post-landing protection window.
            const ULONGLONG now=GetTickCount64();
            if(base->airborne) {
                base->airborne=false;
                base->landingGraceUntil=now+900;
            }
            if(base->landingGraceUntil && now<=base->landingGraceUntil) {
                const bool invuln=true;
                bool currentInvuln=false;
                if(Read(pawn+0x8B9,currentInvuln) && !currentInvuln)
                    changed|=TryWrite(pawn+0x8B9,invuln);
                double currentHealth=0,maxHealth=0;
                if(Read(pawn+0x8A0,currentHealth) && Read(pawn+0x8A8,maxHealth) &&
                   std::isfinite(currentHealth) && std::isfinite(maxHealth) &&
                   base->healthBeforeFall>0 && maxHealth>0 &&
                   currentHealth<base->healthBeforeFall) {
                    const double restore=std::min(base->healthBeforeFall,maxHealth);
                    if(TryWrite(pawn+0x8A0,restore)) {
                        changed=true;
                        if(now-base->lastRestore>2000) {
                            base->lastRestore=now;
                            FHJDiag::Log(L"NO FALL DAMAGE: corrected delayed landing health loss on host.");
                        }
                    }
                }
            } else if(base->landingGraceUntil) {
                base->landingGraceUntil=0;
                base->healthBeforeFall=-1;
                const bool inv=gGodBackups.find(pawn)!=gGodBackups.end() ?
                    true:base->priorInvuln;
                changed|=TryWrite(pawn+0x8B9,inv);
            }
        }
        if(changed)ForceActorNetUpdate(pawn);
    }
    PhysicsBackup* BackupPawn(uintptr_t pawn) {
        int32_t index=-1;
        if(!pawn || !Read(pawn+0xC,index) || index<0)return nullptr;
        if(auto it=gPhysicsBackups.find(pawn);it!=gPhysicsBackups.end()) {
            if(it->second.index==index)return &it->second;
            gPhysicsBackups.erase(it);
        }
        const auto movement=ReadPtr(pawn+0x330);
        if(!movement)return nullptr;
        PhysicsBackup backup{};backup.index=index;
        // Speed and jump use independent snapshots. Fall protection does NOT
        // require any of these movement values to exist.
        if(!Read(pawn+0x7F0,backup.walk) || !Read(pawn+0x7F8,backup.sprint) ||
           !Read(movement+0x190,backup.jump) ||
           !Read(movement+0x188,backup.gravityScale) ||
           !Read(movement+0x268,backup.maxWalk))return nullptr;
        return &gPhysicsBackups.emplace(pawn,backup).first->second;
    }
    void ApplyPawnPhysics(uintptr_t pawn,bool modifyFall,bool modifySpeed,bool modifyJump) {
        // Fix: do not bail on movement reads before applying independent fall protection.
        if(modifyFall)ApplyFallProtection(pawn,gProtectFall.load());
        if(!modifySpeed && !modifyJump)return;
        const auto base=BackupPawn(pawn);
        if(!base)return;
        const auto movement=ReadPtr(pawn+0x330);
        if(!movement)return;
        bool ok=true;
        if(modifySpeed) {
            const double speed=static_cast<double>(gSpeedMultiplier.load());
            const double walk=std::clamp(base->walk*speed,15.0,5000.0);
            const double sprint=std::clamp(base->sprint*speed,15.0,7000.0);
            const float maxWalk=std::clamp(base->maxWalk*gSpeedMultiplier.load(),20.f,5000.f);
            ok &= TryWrite(pawn+0x7F0,walk);
            ok &= TryWrite(pawn+0x7F8,sprint);
            ok &= TryWrite(movement+0x268,maxWalk);
        }
        if(modifyJump) {
            const float jump=std::clamp(base->jump*gJumpMultiplier.load(),20.f,4000.f);
            ok &= TryWrite(movement+0x190,jump);
        }
        if(!ok)FHJDiag::Log(L"Physics movement write failed; movement offsets may not be valid.");
    }
    void ApplyPhysicsDimension(bool fall,bool speed,bool jump,bool lobby) {
        const bool host=ReadPtr(World()+0x158)!=0;
        const auto local=LocalPawn();
        if(local)ApplyPawnPhysics(local,fall,speed,jump);
        if(lobby && host)for(const auto& actor:gKnown) {
            if(actor.kind==1 && actor.address!=local)
                ApplyPawnPhysics(actor.address,fall,speed,jump);
        }
    }
    void ReapplyMovementForNewPlayers() {
        // Don't overwrite Speed/Jump when only another dimension has changed.
        const bool fall=gProtectFall.load() || !gFallBackups.empty();
        const bool speed=std::fabs(gSpeedMultiplier.load()-1.f)>.001f;
        const bool jump=std::fabs(gJumpMultiplier.load()-1.f)>.001f;
        const auto local=LocalPawn();
        if(local)ApplyPawnPhysics(local,fall,speed,jump);
        if(ReadPtr(World()+0x158))for(const auto& actor:gKnown) {
            if(actor.kind!=1 || actor.address==local)continue;
            ApplyPawnPhysics(actor.address,
                fall && gProtectLobby.load(),
                speed && gSpeedLobby.load(),
                jump && gJumpLobby.load());
        }
    }
    void ApplyWorldGravity(float factor,bool entireLobby) {
        const uintptr_t world=World();
        if(!world || (entireLobby && !ReadPtr(world+0x158))) {
            FHJDiag::Log(L"Lobby gravity requires host authority and an active level.");return;
        }
        const auto level=ReadPtr(world+0x30);
        const auto ws=ReadPtr(level+0x2A8); // SDK ULevel::WorldSettings
        if(!ws || ClassName(ReadPtr(ws+0x10))!=L"WorldSettings") {
            FHJDiag::Log(L"WorldSettings actor unavailable; no gravity change.");return;
        }
        if(gGravityBackup.worldSettings!=ws) {
            gGravityBackup=WorldGravityBackup{};
            gGravityBackup.worldSettings=ws;
            Read(ws+0x330,gGravityBackup.worldGravity);
            Read(ws+0x334,gGravityBackup.globalGravity);
            Read(ws+0x2B6,gGravityBackup.flags);
        }
        const bool restoring=std::fabs(factor-1.f)<0.00001f;
        const float gravity=restoring?gGravityBackup.worldGravity:
            std::clamp(gGravityBackup.worldGravity*factor,-6000.f,0.f);
        const float global=restoring?gGravityBackup.globalGravity:gravity;
        uint8_t bits=0;
        if(!Read(ws+0x2B6,bits))return;
        bits=restoring?gGravityBackup.flags:static_cast<uint8_t>(bits|0x02);
        const bool ok=TryWrite(ws+0x2B6,bits) &&
            TryWrite(ws+0x330,gravity) && TryWrite(ws+0x334,global);
        if(ok) {
            const auto rep=FindFunction(L"WorldSettings",L"OnRep_WorldGravityZ");
            if(rep)Invoke(ws,rep,nullptr);
            const auto net=FindFunction(L"Actor",L"ForceNetUpdate");
            if(net)Invoke(ws,net,nullptr);
            FHJDiag::Log(entireLobby?
                L"Host WorldGravityZ updated (SDK Net property); check other clients for synchronization.":
                L"World gravity changed locally; remote players not targeted.");
        } else FHJDiag::Log(L"WorldGravityZ update failed.");
    }
    bool VerifyAndInitializeSpawnedCryptid(uintptr_t spawned,uintptr_t wantedClass) {
        if(!spawned || ReadPtr(spawned+0x10)!=wantedClass)return false;
        const bool isMothman=ClassName(wantedClass)==L"BP_FlyingPawnTest_C";
        const auto mesh=ReadPtr(spawned+(isMothman?0x3D0:0x328));
        // Engine.USkinnedMeshComponent::SkeletalMesh and SkinnedAsset are
        // +0x520 and +0x528. Engine.USkeletalMeshComponent::AnimClass is
        // +0x898; an AnimScriptInstance is +0x8A0. Don't report success
        // for a bare APawn with no cooked model or animation.
        const bool hasMesh=mesh &&
            (ReadPtr(mesh+0x520) || ReadPtr(mesh+0x528));
        const bool hasAnimation=mesh &&
            (ReadPtr(mesh+0x898) || ReadPtr(mesh+0x8A0));
        if(!hasMesh || !hasAnimation) {
            FHJDiag::Log(L"CRYPTID SPAWN: rejected missing skeletal mesh or AnimBlueprint. Species resources are not loaded in this map.");
            return false;
        }
        auto controller=ReadPtr(spawned+0x2D8);
        if(!controller && ReadPtr(spawned+0x2C0)) {
            const auto createAI=FindFunction(L"Pawn",L"SpawnDefaultController");
            uint16_t parms=0;
            if(createAI && Read(createAI+0xB6,parms) && parms==0)
                Invoke(spawned,createAI,nullptr);
            controller=ReadPtr(spawned+0x2D8);
        }
        if(!controller) {
            FHJDiag::Log(L"CRYPTID SPAWN: rejected actor with no AIController after native SpawnDefaultController; map prerequisites may be unloaded.");
            return false;
        }
        // No claim of multiplayer replication merely because the model
        // and AI pointers exist locally; validate remotely in a private hunt.
        return true;
    }
    bool SpawnNativeCryptid(uintptr_t spawner,uintptr_t species,int ordinal,
                             int* createdObjectId=nullptr) {
        if(!spawner || !species || ClassKind(species)!=2 || !IsPawnClass(species))
            return false;
        const auto context=FindObjectByName(L"Default__GameplayStatics");
        const auto begin=FindFunction(L"GameplayStatics",L"BeginDeferredActorSpawnFromClass");
        const auto finish=FindFunction(L"GameplayStatics",L"FinishSpawningActor");
        const auto world=World();
        if(!context || !begin || !finish || !world)return false;
        Vec3 location{};
        if(!Position(spawner,location))return false;
        // Space adjacent requested creatures so they do not overlap the
        // spawner or each other; locations are UE5 world units (centimeters).
        const double theta=static_cast<double>(ordinal)*2.399963229728653;
        const double radius=300.0+static_cast<double>(ordinal/6)*140.0;
        Transform64 transform{};
        transform.location.x=location.x+std::cos(theta)*radius;
        transform.location.y=location.y+std::sin(theta)*radius;
        transform.location.z=location.z+75.0;
        alignas(8) uint8_t beginArgs[0x90]{};
        std::memcpy(beginArgs,&world,8);
        std::memcpy(beginArgs+8,&species,8);
        std::memcpy(beginArgs+0x10,&transform,sizeof(transform));
        beginArgs[0x70]=1; // ESpawnActorCollisionHandlingMethod AlwaysSpawn
        if(!Invoke(context,begin,beginArgs))return false;
        uintptr_t spawned=0;
        std::memcpy(&spawned,beginArgs+0x88,sizeof(spawned));
        if(!spawned)return false;
        alignas(8) uint8_t finishArgs[0x80]{};
        std::memcpy(finishArgs,&spawned,sizeof(spawned));
        std::memcpy(finishArgs+0x10,&transform,sizeof(transform));
        if(!Invoke(context,finish,finishArgs))return false;
        uintptr_t returned=0;
        std::memcpy(&returned,finishArgs+0x78,sizeof(returned));
        int32_t id=-1;
        if(returned && Read(returned+0x0C,id) && id>=0 &&
           ClassKind(ReadPtr(returned+0x10))==2) {
            if(!VerifyAndInitializeSpawnedCryptid(returned,species)) {
                // Avoid leaving an invisible model-less or controller-less
                // actor behind; it cannot be safely possessed.
                const auto destroy=FindFunction(L"Actor",L"K2_DestroyActor");
                if(destroy)Invoke(returned,destroy,nullptr);
                return false;
            }
            if(gKnown.size()<400 &&
               std::none_of(gKnown.begin(),gKnown.end(),
                   [returned](const ActorEntry& e){return e.address==returned;}))
                gKnown.push_back({returned,id,2});
            if(createdObjectId)*createdObjectId=id;
            ForceActorNetUpdate(returned);
            return true;
        }
        return false;
    }
    void AdvanceSpawnWave() {
        if(gSpawnWave.remaining<=0)return;
        const auto now=GetTickCount64();
        if(now<gSpawnWave.nextTick)return;
        const auto world=World();
        if(!world || !ReadPtr(world+0x158)) {
            FHJDiag::Log(L"CRYPTID SPAWN: host authority lost; cancelled.");
            gSpawnWave=SpawnWave{};return;
        }
        std::vector<uintptr_t> spawners;
        int total=0;
        for(const auto& level:GetWorldLevels(world,total))
            for(int i=0;i<level.count;++i) {
                const auto actor=ReadPtr(level.data+static_cast<size_t>(i)*8);
                if(!actor || ClassName(ReadPtr(actor+0x10))!=L"BP_MobSpawner_C")
                    continue;
                int32_t id=-1;
                if(!Read(actor+0x0C,id))continue;
                if(gSpawnWave.spawnerObjectIndex<0 ||
                   id==gSpawnWave.spawnerObjectIndex)
                    spawners.push_back(actor);
            }
        if(gSpawnWave.spawnerObjectIndex>=0 && spawners.empty()) {
            FHJDiag::Log(L"CRYPTID SPAWN: requested map spawner isn't present in this loaded hunt; cancelled.");
            gSpawnWave=SpawnWave{};return;
        }
        // Only an explicitly selected loaded class may fall back to the
        // host's position when a map has no native BP_MobSpawner_C actor.
        const auto anchor=spawners.empty() ?
            (gSpawnWave.classObjectIndex>=0?LocalPawn():0) :
            spawners[static_cast<size_t>(gSpawnWave.dispatched)%spawners.size()];
        if(!anchor) {
            FHJDiag::Log(L"CRYPTID SPAWN: no usable spawner or host pawn.");
            gSpawnWave=SpawnWave{};return;
        }
        uintptr_t species=0;
        if(gSpawnWave.classObjectIndex>=0) {
            int objectCount=0;uintptr_t chunks=0;
            if(GetObjects(objectCount,chunks) &&
               gSpawnWave.classObjectIndex<objectCount) {
                const auto klass=ObjectAt(gSpawnWave.classObjectIndex,chunks);
                int32_t id=-1;
                const auto meta=ClassName(ReadPtr(klass+0x10));
                if(klass && Read(klass+0x0C,id) &&
                   id==gSpawnWave.classObjectIndex &&
                   (meta==L"Class" || meta==L"BlueprintGeneratedClass") &&
                   ClassKind(klass)==2 && IsPawnClass(klass))
                    species=klass;
            }
            if(!species) {
                FHJDiag::Log(L"CRYPTID SPAWN: selected cryptid UClass unloaded or changed; no spawn attempted.");
                gSpawnWave=SpawnWave{};return;
            }
        } else {
            for(const auto& found:gKnown)
                if(found.kind==2 && found.index==gSpawnWave.speciesIndex) {
                    int32_t id=-1;
                    if(Read(found.address+0xC,id) && id==found.index)
                        species=ReadPtr(found.address+0x10);
                    break;
                }
            if(!species && !spawners.empty())
                species=ReadPtr(anchor+0x2B8); // map's own MobClass
        }
        if(!species || ClassKind(species)!=2) {
            FHJDiag::Log(L"CRYPTID SPAWN: no loaded cryptid class selected by map or user.");
            gSpawnWave=SpawnWave{};return;
        }
        int createdId=-1;
        const bool created=SpawnNativeCryptid(anchor,species,gSpawnWave.dispatched,
                                               &createdId);
        bool dispatched=created;
        if(!created && !spawners.empty()) {
            const auto original=ReadPtr(anchor+0x2B8);
            if(original==species) {
                // Only the map's own species can use the native TriggerSpawn
                // fallback. Its Blueprint graph can override MobClass when
                // handling cross-map requests, spawning an invisible/default
                // creature; never report that as the requested species.
                const auto trigger=FindFunction(L"BP_MobSpawner_C",L"TriggerSpawn");
                if(trigger) {
                    alignas(8) uint8_t args[0xA0]{};
                    dispatched=Invoke(anchor,trigger,args);
                }
            } else {
                FHJDiag::Log(L"CRYPTID SPAWN: selected off-map class failed model/AI verification; refused TriggerSpawn fallback to prevent default or invisible actor.");
            }
        }
        if(!dispatched) {
            FHJDiag::Log(L"CRYPTID SPAWN: native deferred spawn and map spawner dispatch failed; stopping wave.");
            gSpawnWave=SpawnWave{};return;
        }
        ++gSpawnWave.dispatched;
        if(created)++gSpawnWave.created;
        --gSpawnWave.remaining;
        gSpawnWave.nextTick=now+550;
        wchar_t msg[224]{};
        swprintf_s(msg,L"CRYPTID SPAWN: requested %d/%d, verified direct actors %d, pending %d. Spawner dispatch alone is not a verified actor.",
            gSpawnWave.dispatched,gSpawnWave.requested,
            gSpawnWave.created,gSpawnWave.remaining);
        FHJDiag::Log(msg);
        if(created && createdId>=0 && gSpawnWave.controlFirst) {
            gSpawnWave.controlFirst=false;
            PossessCreature(createdId,false);
        }
    }
    void SetNativeMapSpawnerEnabled(int spawnerIndex,bool enabled) {
        if(!ReadPtr(World()+0x158) || spawnerIndex<0)return;
        for(const auto& actor:gKnownSpawners) {
            if(actor.index!=spawnerIndex)continue;
            int32_t id=-1;
            if(!Read(actor.address+0x0C,id) || id!=spawnerIndex ||
               ClassName(ReadPtr(actor.address+0x10))!=L"BP_MobSpawner_C")
                break;
            const auto fn=FindFunction(L"BP_MobSpawner_C",L"SetSpawningEnabled");
            if(!fn)break;
            alignas(8) uint8_t args[0x40]{};
            args[0]=enabled?1:0; // SDK native first bool parameter
            if(!Invoke(actor.address,fn,args))break;
            bool active=false;
            if(Read(actor.address+0x308,active) && active==enabled)
                FHJDiag::Log(enabled?
                    L"MAP SPAWNER: SetSpawningEnabled(true) reflected state confirmed.":
                    L"MAP SPAWNER: SetSpawningEnabled(false) reflected state confirmed.");
            else
                FHJDiag::Log(L"MAP SPAWNER: native toggle dispatched but state change not confirmed.");
            return;
        }
        FHJDiag::Log(L"MAP SPAWNER: selected live map spawner or native method not found.");
    }
    void SpawnSelectedClassWave(int count,int classObjectIndex,
                                 int mapSpawnerIndex,bool autoControl) {
        if(!ReadPtr(World()+0x158) || count<1 || count>10 || classObjectIndex<0)
            return;
        int total=0;uintptr_t chunks=0;
        if(!GetObjects(total,chunks) || classObjectIndex>=total)return;
        const auto klass=ObjectAt(classObjectIndex,chunks);
        int32_t id=-1;
        const auto speciesName=ObjectName(klass);
        const bool concrete=speciesName==L"BP_FlyingPawnTest_C" ||
            speciesName==L"BP_Vampire_C" || speciesName==L"BP_Wendigo_C";
        if(!klass || !Read(klass+0xC,id) || id!=classObjectIndex ||
           !concrete || ClassKind(klass)!=2 || !IsPawnClass(klass)) {
            FHJDiag::Log(L"CRYPTID SPAWN: rejected object that isn't a currently loaded cryptid UClass.");
            return;
        }
        gSpawnWave=SpawnWave{};
        gSpawnWave.remaining=count;gSpawnWave.requested=count;
        gSpawnWave.classObjectIndex=classObjectIndex;
        gSpawnWave.spawnerObjectIndex=mapSpawnerIndex;
        gSpawnWave.controlFirst=autoControl;
        FHJDiag::Log(L"CRYPTID SPAWN: queued selected species in active hunt; will verify each direct actor.");
    }
    void SpawnCreatureWave(int count,int liveCryptidIndex) {
        const auto world=World();
        if(!world || !ReadPtr(world+0x158)) {
            FHJDiag::Log(L"Creature spawn rejected: host GameMode required.");return;
        }
        if(!FindFunction(L"BP_MobSpawner_C",L"TriggerSpawn")) {
            FHJDiag::Log(L"Creature spawn rejected: TriggerSpawn not loaded.");return;
        }
        gSpawnWave={count,count,0,liveCryptidIndex,0};
        FHJDiag::Log(L"CRYPTID SPAWN: queued paced native spawn wave; a single map spawner can receive multiple calls.");
    }
    void PossessCreature(int index,bool restore) {
        const auto world=World();
        if(!world || !ReadPtr(world+0x158)) {
            FHJDiag::Log(L"Monster Mode requires solo or host authority.");
            return;
        }
        const auto instance=ReadPtr(world+0x1D8);
        const auto players=ReadPtr(instance+0x38);
        const auto local=ReadPtr(players);
        const auto pc=ReadPtr(local+0x30);
        if(!pc)return;
        // No possession swaps: PlayerCharacter_C.ReceiveUnpossessed and
        // GameMode's singleplayer restart logic may destroy/restart the hunt.
        // Instead the controller stays on the human and its ViewTarget
        // switches to a camera chasing the independently driven cryptid.
        if(restore) {
            RestoreHumanControls();
            RestoreHumanHUD();
            ReleaseMonsterCamera(pc,gHumanPawn?gHumanPawn:ReadPtr(pc+0x350));
            RestoreMonsterAI();
            gMonsterActive.store(false);
            gPuppeteering=false;gPossessedCryptid=false;
            gMonsterPawn=0;gMonsterIndex=-1;gChaseSmoothedValid=false;
            ResetControlledRespawnSession();
            gHumanPawn=0;gHumanIndex=-1;
            FHJDiag::Log(L"Restore Human: camera/HUD/AI reset without Controller.Possess; hunt remains active.");
            return;
        }
        uintptr_t selected=0;
        for(const auto& actor:gKnown) {
            if(actor.kind==2 && actor.index==index) {
                selected=actor.address;
                break;
            }
        }
        if(!selected || !IsPawnClass(ReadPtr(selected+0x10))) {
            FHJDiag::Log(L"Monster Mode: selected cryptid not a live APawn/ACharacter.");
            return;
        }
        const auto human=ReadPtr(pc+0x350);
        int32_t humanIndex=-1;
        if(!human || !Read(human+0x0C,humanIndex)) {
            FHJDiag::Log(L"Monster Mode: human pawn unavailable; refusing unsafe control.");
            return;
        }
        if(gMonsterActive.load()) {
            if(selected==gMonsterPawn)return;
            RestoreHumanControls();
            RestoreHumanHUD();
            ReleaseMonsterCamera(pc,gHumanPawn);
            RestoreMonsterAI();
            gMonsterActive.store(false);
        }
        gHumanPawn=human;
        gHumanIndex=humanIndex;
        gMonsterPawn=selected;
        gMonsterIndex=index;
        CaptureControlledCryptidSession();
        gChaseSmoothedValid=false;
        const Camera previous=GetCamera(world);
        gMonsterYaw=std::isfinite(previous.rot.yaw)?previous.rot.yaw:0.;
        gMonsterPitch=std::clamp(std::isfinite(previous.rot.pitch)?previous.rot.pitch:0.,-80.,80.);
        gLastMotionTick=0;
        gLastAbilityTick=0;gAbilityTicks.fill(0);gAbilityKeyDown.fill(false);
        gVampireStance=0;gVampireInvisible=false;
        Vec3 pos{};
        if(!Position(selected,pos) || !CreateMonsterCamera(pc,pos)) {
            gMonsterPawn=0;gMonsterIndex=-1;
            ResetControlledRespawnSession();
            ReleaseMonsterCamera(pc,gHumanPawn);
            FHJDiag::Log(L"Monster Mode cancelled: chase-camera setup failed; human still possessed.");
            return;
        }
        gMonsterActive.store(true);
        gPuppeteering=true;gPossessedCryptid=true;
        UpdateMonsterHUD();
        StopMonsterAI();
        ConfigureMonsterMovement();
        CacheCryptidActions();
        UpdateMonsterHUD(); // refresh HUD after the selected species' function cache is built
        SuspendHumanControls(pc);
        HideHumanHUD();
        FHJDiag::Log(L"Monster puppet mode: human controller remains possessed. WASD native movement, Q/E/R ability, F11 restore.");
    }
    void ExecutePhysics(const Action& a) {
        if(a.type==ActionType::FallProtection) {
            gProtectFall.store(a.amount!=0);gProtectLobby.store(a.target==1);
            ApplyPhysicsDimension(true,false,false,a.target==1);
            FHJDiag::Log(L"Fall-damage properties updated without touching speed/jump/gravity.");
        } else if(a.type==ActionType::SpeedChange) {
            gSpeedMultiplier.store(a.value);gSpeedLobby.store(a.amount!=0);
            ApplyPhysicsDimension(false,true,false,a.amount!=0);
            FHJDiag::Log(L"Walk/sprint speed updated independently of JumpZVelocity and gravity.");
        } else if(a.type==ActionType::JumpBoost) {
            gJumpMultiplier.store(a.value);gJumpLobby.store(a.amount!=0);
            ApplyPhysicsDimension(false,false,true,a.amount!=0);
            FHJDiag::Log(L"JumpZVelocity updated independently of walk/sprint and gravity.");
        } else if(a.type==ActionType::GravityChange)
            ApplyWorldGravity(a.value,a.amount!=0);
        else if(a.type==ActionType::SpawnMob)
            SpawnCreatureWave(a.amount,a.target);
        else if(a.type==ActionType::SpawnSpecies)
            SpawnSelectedClassWave(a.amount,a.target,
                static_cast<int>(a.precise),a.value>0.5f);
        else if(a.type==ActionType::SpawnerToggle)
            SetNativeMapSpawnerEnabled(a.target,a.amount!=0);
        else if(a.type==ActionType::PossessCryptid || a.type==ActionType::RestoreHuman)
            PossessCreature(a.target,a.type==ActionType::RestoreHuman);
    }

    void EquipVanity(const Action& a) {
        if(!ReadPtr(World()+0x158)) {
            FHJDiag::Log(L"VANITY: host authority required for equipment changes.");return;
        }
        const auto pawn=LocalPawn();
        const auto ps=ReadPtr(pawn+0x2B0);
        const auto manager=ReadPtr(ps+0x368);
        if(!manager || ClassName(ReadPtr(manager+0x10))!=L"BP_CosmeticsManager_C") {
            FHJDiag::Log(L"VANITY: local cosmetics manager not found. Enter a playable hunt.");
            return;
        }
        // Whitelist to actual classes in the manager's AllCosmeticItems TArray.
        auto data=ReadPtr(manager+0xF8);
        int32_t count=0;
        if(!data || !Read(manager+0x100,count) || count<1 || count>256)return;
        uintptr_t selected=0;
        for(int i=0;i<count;++i) {
            auto klass=ReadPtr(data+static_cast<size_t>(i)*8);
            auto name=ObjectName(klass);
            if(std::string(name.begin(),name.end())==a.item) {
                selected=klass;break;
            }
        }
        if(!selected) {
            FHJDiag::Log(L"VANITY: requested class no longer exists in local available catalog.");
            return;
        }
        // Host-owned function, not a guessed actor memory write.
        const auto fn=FindFunction(L"BP_CosmeticsManager_C",L"SetEquippedVanityItem");
        if(!fn) {FHJDiag::Log(L"VANITY: SetEquippedVanityItem missing.");return;}
        if(!Invoke(manager,fn,&selected)) {
            FHJDiag::Log(L"VANITY: SetEquippedVanityItem invocation failed.");return;
        }
        const auto actual=ReadPtr(manager+0xC8);
        FHJDiag::Log(actual==selected?
            L"VANITY: equipped item class confirmed in replicated cosmetics manager.":
            L"VANITY: dispatch succeeded but selected cosmetic was not committed.");
    }
    void ExecuteAction(const Action& a) {
        if(a.type==ActionType::EquipVanity) {EquipVanity(a);return;}
        if(a.type==ActionType::MonsterAbility) {ExecuteMonsterAbility(a.amount);return;}
        if (a.type==ActionType::FallProtection ||
            a.type==ActionType::SpeedChange ||
            a.type==ActionType::JumpBoost ||
            a.type==ActionType::GravityChange ||
            a.type==ActionType::SpawnMob ||
            a.type==ActionType::SpawnSpecies ||
            a.type==ActionType::SpawnerToggle ||
            a.type==ActionType::PossessCryptid ||
            a.type==ActionType::RestoreHuman) {
            ExecutePhysics(a); return;
        }
        if(a.type==ActionType::NoclipOn || a.type==ActionType::NoclipOff) {
            ExecuteNoclip(a.type==ActionType::NoclipOn);return;
        }
        if(a.type==ActionType::TargetSound) {ExecuteClientSound(a);return;}
        if(a.type==ActionType::TargetCameraShake) {ExecuteClientCameraShake(a);return;}
        if(a.type==ActionType::TargetHealth || a.type==ActionType::TargetNoise ||
           a.type==ActionType::TargetRevive || a.type==ActionType::TargetKill ||
           a.type==ActionType::TargetKick || a.type==ActionType::TargetGodMode) {
            ExecuteTargetedAction(a);return;
        }
        if(a.type==ActionType::TargetItem) { GiveCatalogItem(a);return; }
        if(a.type==ActionType::GodMode) {
            const auto pawn=LocalPawn();
            int32_t index=-1;
            if(ValidPlayerPawn(pawn,index)) {
                const auto old=gGodBackups.find(pawn);
                const bool active=old!=gGodBackups.end() && old->second.index==index;
                ApplyGodProtection(pawn,!active);
            } else FHJDiag::Log(L"Local human pawn not available for God Mode.");
            return;
        }
        if (a.type==ActionType::VampireScream) {TriggerVampireScream();return;}
        if (a.type==ActionType::FlickerLights) {TriggerFlashlightPrank(false);return;}
        if (a.type==ActionType::CancelFlicker) {TriggerFlashlightPrank(true);return;}
        if (a.type==ActionType::CatalogItem) { GiveCatalogItem(a); return; }
        uintptr_t world=World();
        uintptr_t gm=ReadPtr(world+0x158);
        if (!world || !gm) {FHJDiag::Log(L"Host action rejected: no authoritative GameMode (client or menu).");return;}
        std::wstring fn;
        std::wstring owner=L"FolkloreGamemode";
        switch(a.type) {
            case ActionType::Rain:
            case ActionType::Snow:
                ExecuteWeather(a);
                return;
            case ActionType::ClearWeather:
                ExecuteClearWeather();
                return;
            case ActionType::BeanMode:fn=L"EnableBeanMode";break;
            case ActionType::RangerMode:fn=L"EnableRangerMode";break;
            case ActionType::SmoreMode:fn=L"EnableSmoreMode";break;
            case ActionType::GiveItem:fn=L"GiveItem";break;
            case ActionType::GodMode:fn=L"ToggleGodMode";break;
            case ActionType::AddXP:fn=L"AddXP";break;
            case ActionType::AddLevels:fn=L"AddLevels";break;
            case ActionType::Time:fn=L"SetTime";break;
            case ActionType::TimeScale:fn=L"SetTimeScale";break;
            case ActionType::AddSilver:fn=L"Cheat_AddSilver";owner=L"FL_Gamemode_C";break;
            case ActionType::InfiniteDamage:fn=L"Cheat_InfiniteDamage";owner=L"FL_Gamemode_C";break;
            case ActionType::CatalogItem:
            case ActionType::VampireScream:
            case ActionType::FlickerLights:
            case ActionType::CancelFlicker:
            case ActionType::TargetHealth:
            case ActionType::TargetItem:
            case ActionType::TargetNoise:
            case ActionType::TargetRevive:
            case ActionType::TargetKill:
            case ActionType::TargetKick:
            case ActionType::TargetGodMode:
            case ActionType::TargetSound:
            case ActionType::TargetCameraShake:
            case ActionType::NoclipOn:
            case ActionType::NoclipOff:
            case ActionType::FallProtection:
            case ActionType::SpeedChange:
            case ActionType::JumpBoost:
            case ActionType::GravityChange:
            case ActionType::SpawnMob:
            case ActionType::SpawnSpecies:
            case ActionType::SpawnerToggle:
            case ActionType::PossessCryptid:
            case ActionType::RestoreHuman:
            case ActionType::MonsterAbility:
            case ActionType::EquipVanity: return;
        }
        // FL_Gamemode_C overrides GiveItem/SetTime/SetTimeScale in this SDK.
        // Prefer the active Blueprint function and fall back to the native base UFunction.
        uintptr_t function=0;
        if(a.type==ActionType::BeanMode) {
            // Only native FolkloreGamemode declares EnableBeanMode.
            function=FindFunction(L"FolkloreGamemode",fn);
        } else if(a.type==ActionType::GiveItem || a.type==ActionType::Time ||
                  a.type==ActionType::TimeScale || a.type==ActionType::RangerMode ||
                  a.type==ActionType::SmoreMode || a.type==ActionType::AddLevels) {
            function=FindFunction(L"FL_Gamemode_C",fn);
        }
        if (!function) function=FindFunction(owner,fn);
        if (!function) { FHJDiag::Log(L"GameMode command not found in live reflection; no action executed.");return; }
        alignas(8) uint8_t args[0x40]{};
        std::wstring item;
        if (a.type==ActionType::GiveItem) {
            int chars=MultiByteToWideChar(CP_UTF8,0,a.item.c_str(),-1,nullptr,0);
            if (chars<2 || chars>200) return;
            item.resize(static_cast<size_t>(chars));
            MultiByteToWideChar(CP_UTF8,0,a.item.c_str(),-1,item.data(),chars);
            FStringView str{item.data(),chars,chars};
            std::memcpy(args,&str,sizeof(str));
            std::memcpy(args+0x10,&a.amount,sizeof(a.amount));
        } else if (a.type==ActionType::Time || a.type==ActionType::TimeScale) {
            std::memcpy(args,&a.value,sizeof(a.value));
        } else {
            std::memcpy(args,&a.amount,sizeof(a.amount));
        }
        if(Invoke(gm,function,args)) {
            if(a.type==ActionType::BeanMode || a.type==ActionType::RangerMode ||
               a.type==ActionType::SmoreMode) {
                FHJDiag::Log(L"DEVELOPER MODE: native UFunction dispatch succeeded; its internal effect is not exposed by SDK.");
            } else FHJDiag::Log(L"GameMode ProcessEvent dispatched (verify actual game effect).");
        } else FHJDiag::Log(L"GameMode ProcessEvent failed; action not confirmed.");
    }
    bool Enqueue(Action a) {
        if (!gWindow) return false;
        {
            std::lock_guard<std::mutex> lock(gActionsMutex);
            if (gActions.size()>16) return false;
            gActions.push_back(std::move(a));
        }
        if (!gActionPending.exchange(true,std::memory_order_acq_rel))
            PostMessageW(gWindow,HunterNative::kGameAction,0,0);
        return true;
    }
}
void HunterNative::AttachWindow(HWND window) { gWindow=window; }
void HunterNative::RequestTick() {
    if (!gWindow) return;
    const ULONGLONG now=GetTickCount64();
    const DWORD desiredInterval = (gNoclipEnabled || gMonsterActive.load()) ? 16 : kScanIntervalMs;
    if (now-gLastTick<desiredInterval) return;
    gLastTick=now;
    if (!gTickPending.exchange(true,std::memory_order_acq_rel))
        PostMessageW(gWindow,kGameTick,0,0);
}
void HunterNative::HandleWindowMessage(UINT message) {
    if (message==kGameTick) Tick();
    else if (message==kGameAction) {
        std::vector<Action> pending;
        {
            std::lock_guard<std::mutex> lock(gActionsMutex);
            pending.swap(gActions);
            gActionPending.store(false,std::memory_order_release);
        }
        for(const auto& action:pending) ExecuteAction(action);
    }
}
HunterNative::Status HunterNative::GetStatus() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gStatus;
}
std::vector<HunterNative::PlayerInfo> HunterNative::GetPlayers() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gRoster;
}
std::vector<HunterNative::CreatureInfo> HunterNative::GetCreatures() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gCreatures;
}
std::vector<HunterNative::SoundInfo> HunterNative::GetSounds() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSounds;
}
std::vector<HunterNative::CameraShakeInfo> HunterNative::GetCameraShakes() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gCameraShakes;
}
std::vector<HunterNative::CryptidSpawnClass> HunterNative::GetSpawnableCryptidClasses() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSpawnClassCatalog;
}
std::vector<HunterNative::CryptidSpawnPoint> HunterNative::GetActiveMapSpawners() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSpawnPointCatalog;
}
HunterNative::CryptidCameraSettings HunterNative::GetCryptidCameraSettings() {
    LoadPreferences();
    return {gCryptidYawSensitivity.load(),gCryptidPitchSensitivity.load(),
            gCryptidCameraFollow.load(),gCryptidInvertPitch.load(),
            gCryptidClearLocalBlur.load()};
}
void HunterNative::SetCryptidCameraSettings(const CryptidCameraSettings& value) {
    if(!std::isfinite(value.yawSensitivity) ||
       !std::isfinite(value.pitchSensitivity) ||
       !std::isfinite(value.followRate))return;
    gCryptidYawSensitivity.store(std::clamp(value.yawSensitivity,.015f,.60f));
    gCryptidPitchSensitivity.store(std::clamp(value.pitchSensitivity,.015f,.60f));
    gCryptidCameraFollow.store(std::clamp(value.followRate,8.f,72.f));
    gCryptidInvertPitch.store(value.invertPitch);
    gCryptidClearLocalBlur.store(value.clearBlurDuringPossession);
    auto file=SettingsPath();
    if(file.empty())return;
    wchar_t buf[32]{};
    swprintf_s(buf,L"%d",static_cast<int>(gCryptidYawSensitivity.load()*1000.f+.5f));
    WritePrivateProfileStringW(L"CryptidCamera",L"YawMilli",buf,file.c_str());
    swprintf_s(buf,L"%d",static_cast<int>(gCryptidPitchSensitivity.load()*1000.f+.5f));
    WritePrivateProfileStringW(L"CryptidCamera",L"PitchMilli",buf,file.c_str());
    swprintf_s(buf,L"%d",static_cast<int>(gCryptidCameraFollow.load()+.5f));
    WritePrivateProfileStringW(L"CryptidCamera",L"FollowRate",buf,file.c_str());
    WritePrivateProfileStringW(L"CryptidCamera",L"InvertPitch",
        value.invertPitch?L"1":L"0",file.c_str());
    WritePrivateProfileStringW(L"CryptidCamera",L"ClearBlurWhilePossessed",
        value.clearBlurDuringPossession?L"1":L"0",file.c_str());
}
bool HunterNative::MothmanLocalSmokeReduction() {
    LoadPreferences();
    return gMothmanReduceLocalSmoke.load();
}
void HunterNative::SetMothmanLocalSmokeReduction(bool enabled) {
    gMothmanReduceLocalSmoke.store(enabled);
    const auto file=SettingsPath();
    if(!file.empty())
        WritePrivateProfileStringW(L"Mothman",L"ReduceLocalSmoke",
            enabled?L"1":L"0",file.c_str());
}
std::vector<HunterNative::VanityItem> HunterNative::GetVanityItems() {
    std::lock_guard<std::mutex> lock(gMutex);
    std::vector<VanityItem> result;
    result.reserve(gVanityChoices.size());
    for(const auto& item:gVanityChoices)
        result.push_back({item.name,item.name,item.equipped});
    return result;
}


void HunterNative::DrawESP(ImVec2 display) {
    LoadPreferences();
    if (!gEsp.load())return;
    Camera camera{};
    std::vector<Marker> markers;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        camera=gCamera;
        markers=gMarkers;
    }
    // The actor scan is deliberately throttled, but camera projection must
    // never reuse that scan's 110 ms-old view. Sample the camera cache for
    // EVERY presented frame to keep ESP pinned while the mouse turns.
    const auto liveCamera=GetCamera(World());
    if(liveCamera.fov>=20.0f)camera=liveCamera;
    if(camera.fov<20 || markers.empty())return;
    ImDrawList* dl=ImGui::GetForegroundDrawList();
    constexpr const char* names[]={"INVALID","PLAYER","CRYPTID","WILDLIFE/NPC","CHEST/BOX","QUEST/OBJECTIVE","LOOT/ITEM"};
    constexpr ImU32 colors[]={
        IM_COL32(180,180,180,255),IM_COL32(125,193,128,235),
        IM_COL32(215,75,69,240),IM_COL32(224,153,81,235),
        IM_COL32(210,176,107,235),IM_COL32(137,176,229,240),
        IM_COL32(195,154,226,230)};
    for(const auto& marker:markers) {
        if(marker.kind<1 || marker.kind>6)continue;
        double meters=0;
        ImVec2 pt{};
        if(!Project(marker.pos,camera,display,pt,meters))continue;
        // Mothman uses the same red ESP treatment as every other cryptid.
        const ImU32 color=colors[marker.kind];
        const float radius=std::clamp(14.f-static_cast<float>(meters)*.018f,4.f,14.f);
        dl->AddCircle(pt,radius,color,12,1.4f);
        dl->AddLine({pt.x-5,pt.y},{pt.x+5,pt.y},color,1.2f);
        dl->AddLine({pt.x,pt.y-5},{pt.x,pt.y+5},color,1.2f);
        char label[160]{};
        const bool showHealth=marker.kind==1 || marker.kind==2;
        const char* category=marker.mothman?"MOTHMAN":names[marker.kind];
        if(marker.mothman)
            snprintf(label,sizeof(label),"MOTHMAN %.0fm | HP not exposed",
                meters);
        else if(showHealth && marker.health>=0 && marker.maxHealth>0)
            snprintf(label,sizeof(label),"%s %.0fm | HP %.0f / %.0f",
                category,meters,marker.health,marker.maxHealth);
        else if(showHealth && marker.health>=0)
            snprintf(label,sizeof(label),"%s %.0fm | HP %.0f",
                category,meters,marker.health);
        else if(showHealth)
            snprintf(label,sizeof(label),"%s %.0fm | HP ?",category,meters);
        else
            snprintf(label,sizeof(label),"%s %.0fm",category,meters);
        const float left=pt.x+radius+4;
        dl->AddText({left,pt.y-11},color,label);
        if(showHealth && marker.health>=0 && marker.maxHealth>0) {
            constexpr float width=94.f;
            const float ratio=std::clamp(
                static_cast<float>(marker.health/marker.maxHealth),0.f,1.f);
            dl->AddRectFilled({left,pt.y+7},{left+width,pt.y+13},
                IM_COL32(34,37,33,210),2.f);
            dl->AddRectFilled({left,pt.y+7},{left+width*ratio,pt.y+13},
                color,2.f);
        }
    }
}
void HunterNative::DrawESPSettings() {
    LoadPreferences();
    bool esp=gEsp.load(),cr=gCryptids.load(),pl=gPlayers.load();
    bool animals=gWildlife.load(),containers=gContainers.load(),
        quest=gQuest.load(),loot=gLoot.load();
    int range=gMaxDistanceMeters.load();
    bool changed=false;
    changed|=ImGui::Checkbox("Enable all-world ESP", &esp);
    changed|=ImGui::Checkbox("Cryptids (all inherited species)",&cr);
    changed|=ImGui::Checkbox("Players",&pl);
    changed|=ImGui::Checkbox("Wildlife / wolves / other NPCs",&animals);
    changed|=ImGui::Checkbox("Chests / crates / boxes",&containers);
    changed|=ImGui::Checkbox("Quest / mission / objectives",&quest);
    changed|=ImGui::Checkbox("General loot / pickups / items",&loot);
    changed|=ImGui::SliderInt("Maximum range (m)",&range,25,750);
    if(changed) {
        gEsp.store(esp);gCryptids.store(cr);gPlayers.store(pl);
        gWildlife.store(animals);gContainers.store(containers);
        gQuest.store(quest);gLoot.store(loot);gMaxDistanceMeters.store(range);
        SavePreferences();
    }
    ImGui::TextDisabled("SDK cryptid ancestry + class-name heuristics for other objects.");
    ImGui::TextDisabled("Bounded actor scan: max 700/tick, max 180 markers. World positions experimental.");
}
bool HunterNative::RequestGiveItem(const std::string& item,int n) {
    if(item.empty() || n<1 || n>999) return false;
    return Enqueue({ActionType::GiveItem,item,n,0});
}
bool HunterNative::RequestGodMode() {return Enqueue({ActionType::GodMode,"",0,0});}
bool HunterNative::RequestAddXP(int xp) {return xp>0 && xp<=100000?Enqueue({ActionType::AddXP,"",xp,0}):false;}
bool HunterNative::RequestAddLevels(int levels) {
    return levels>0 && levels<=100 ?
        Enqueue({ActionType::AddLevels,"",levels,0}):false;
}
bool HunterNative::RequestTime(float hour) {return std::isfinite(hour)?Enqueue({ActionType::Time,"",0,hour}):false;}
bool HunterNative::RequestTimeScale(float scale) {return std::isfinite(scale) && scale>0.05f && scale<10.f?Enqueue({ActionType::TimeScale,"",0,scale}):false;}
bool HunterNative::RequestAddSilver(int n) {return n>0&&n<=100000?Enqueue({ActionType::AddSilver,"",n,0}):false;}
bool HunterNative::RequestInfiniteDamage() {return Enqueue({ActionType::InfiniteDamage,"",0,0});}

bool HunterNative::RequestCatalogItem(const std::string& name,int quantity) {
    if (quantity<1 || quantity>999 || name.size()>96 || name.rfind("Item_",0)!=0) return false;
    return Enqueue({ActionType::CatalogItem,name,quantity,0});
}
bool HunterNative::RequestBeanMode() {return Enqueue({ActionType::BeanMode,"",0,0});}
bool HunterNative::RequestRangerMode() {return Enqueue({ActionType::RangerMode,"",0,0});}
bool HunterNative::RequestSmoreMode() {return Enqueue({ActionType::SmoreMode,"",0,0});}
bool HunterNative::RequestRain(double amount) {
    return std::isfinite(amount) && amount>=0.0 && amount<=1.0 ?
        Enqueue({ActionType::Rain,"",0,static_cast<float>(amount)}):false;
}
bool HunterNative::RequestSnow(double amount) {
    return std::isfinite(amount) && amount>=0.0 && amount<=1.0 ?
        Enqueue({ActionType::Snow,"",0,static_cast<float>(amount)}):false;
}
bool HunterNative::RequestClearWeather() {
    return Enqueue({ActionType::ClearWeather,"",0,0});
}

bool HunterNative::RequestVampireScream() {return Enqueue({ActionType::VampireScream,"",0,0});}
bool HunterNative::RequestFlickerLights(bool cancel) {
    return Enqueue({cancel?ActionType::CancelFlicker:ActionType::FlickerLights,"",0,0});
}

bool HunterNative::RequestNoclip(bool enabled) {
    return Enqueue({enabled?ActionType::NoclipOn:ActionType::NoclipOff,"",0,0,-1,0});
}
bool HunterNative::RequestTargetHealth(int index,double value) {
    if(index<0 || !std::isfinite(value) || value<0.0 || value>10000.0)return false;
    return Enqueue({ActionType::TargetHealth,"",0,0,index,value});
}
bool HunterNative::RequestTargetItem(int index,const std::string& name,int amount) {
    if(index<0 || amount<1 || amount>999 || name.rfind("Item_",0)!=0)return false;
    return Enqueue({ActionType::TargetItem,name,amount,0,index,0});
}
bool HunterNative::RequestTargetNoise(int index,double loudness,double maxRangeMeters) {
    if(index<0 || !std::isfinite(loudness) || loudness<0 || loudness>100 ||
        !std::isfinite(maxRangeMeters) || maxRangeMeters<1 || maxRangeMeters>2000) return false;
    return Enqueue({ActionType::TargetNoise,"",0,static_cast<float>(maxRangeMeters),index,loudness});
}
bool HunterNative::RequestTargetSound(int playerIndex,int soundIndex,float volume) {
    if(playerIndex< -1 || soundIndex<0 || !std::isfinite(volume) ||
       volume<0.05f || volume>2.0f)return false;
    return Enqueue({ActionType::TargetSound,"",soundIndex,volume,playerIndex,0});
}
bool HunterNative::RequestTargetCameraShake(int playerIndex,int shakeClassIndex,float scale) {
    if(playerIndex< -1 || shakeClassIndex<0 || !std::isfinite(scale) ||
       scale<0.1f || scale>2.0f)return false;
    return Enqueue({ActionType::TargetCameraShake,"",shakeClassIndex,scale,playerIndex,0});
}

bool HunterNative::RequestTargetRevive(int id) {
    return id>=0?Enqueue({ActionType::TargetRevive,"",0,0,id,0}):false;
}
bool HunterNative::RequestTargetKill(int id) {
    return id>=0?Enqueue({ActionType::TargetKill,"",0,0,id,0}):false;
}
bool HunterNative::RequestTargetKick(int id) {
    return id>=0?Enqueue({ActionType::TargetKick,"",0,0,id,0}):false;
}
bool HunterNative::RequestTargetGodMode(int id,bool enabled) {
    return id>=0?Enqueue({ActionType::TargetGodMode,"",enabled?1:0,0,id,0}):false;
}

bool HunterNative::RequestFallProtection(bool enabled) {
    return Enqueue({ActionType::FallProtection,"",enabled?1:0,0,1,0});
}
bool HunterNative::RequestSpeedMultiplier(float factor,bool lobby) {
    if(!std::isfinite(factor)||factor<.25f||factor>4.f)return false;
    return Enqueue({ActionType::SpeedChange,"",lobby?1:0,factor,-1,0});
}
bool HunterNative::RequestJumpBoost(float factor,bool lobby) {
    if(!std::isfinite(factor)||factor<.25f||factor>4.f)return false;
    return Enqueue({ActionType::JumpBoost,"",lobby?1:0,factor,-1,0});
}
bool HunterNative::RequestGravity(float scale,bool lobby) {
    if(!std::isfinite(scale)||scale<0||scale>3)return false;
    return Enqueue({ActionType::GravityChange,"",lobby?1:0,scale,-1,0});
}
bool HunterNative::RequestSpawnMobWave(int amount,int selectedLiveCryptidIndex) {
    return amount>=1&&amount<=10 ?
        Enqueue({ActionType::SpawnMob,"",amount,0,selectedLiveCryptidIndex,0}):false;
}
bool HunterNative::RequestSpawnCryptidClass(int classObjectIndex,int count,
                                            int mapSpawnerIndex,bool autoControl) {
    if(classObjectIndex<0 || count<1 || count>10 || mapSpawnerIndex< -1)
        return false;
    return Enqueue({ActionType::SpawnSpecies,"",count,autoControl?1.f:0.f,
                    classObjectIndex,static_cast<double>(mapSpawnerIndex)});
}
bool HunterNative::RequestSetMapSpawnerEnabled(int mapSpawnerIndex,bool enabled) {
    return mapSpawnerIndex>=0 ?
        Enqueue({ActionType::SpawnerToggle,"",enabled?1:0,0.f,mapSpawnerIndex,0}):false;
}
bool HunterNative::RequestPossessCryptid(int index) {
    return index>=0?Enqueue({ActionType::PossessCryptid,"",0,0,index,0}):false;
}
bool HunterNative::RequestRestoreHuman() {
    return Enqueue({ActionType::RestoreHuman,"",0,0,-1,0});
}


bool HunterNative::ESPEnabled() {
    return gEsp.load(std::memory_order_relaxed);
}
bool HunterNative::MonsterModeActive() {
    return gMonsterActive.load(std::memory_order_relaxed);
}
void HunterNative::SetJournalOpen(bool isOpen) {
    gJournalOpen.store(isOpen,std::memory_order_relaxed);
}
void HunterNative::FeedRawMouse(LPARAM lParam) {
    if(!gMonsterActive.load() || gJournalOpen.load()) return;
    UINT bytes=0;
    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),RID_INPUT,nullptr,&bytes,
        sizeof(RAWINPUTHEADER)) != 0 || bytes<sizeof(RAWINPUT) || bytes>2048)return;
    alignas(RAWINPUT) unsigned char buffer[2048]{};
    UINT cap=sizeof(buffer);
    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam),RID_INPUT,
        buffer,&cap,sizeof(RAWINPUTHEADER))==static_cast<UINT>(-1))return;
    auto raw=reinterpret_cast<const RAWINPUT*>(buffer);
    if(raw->header.dwType!=RIM_TYPEMOUSE)return;
    // Camera look stays independent of creature AI/controller movement graphs.
    gMonsterYaw+=static_cast<double>(raw->data.mouse.lLastX)*
        static_cast<double>(gCryptidYawSensitivity.load());
    const double sign=gCryptidInvertPitch.load()?1.0:-1.0;
    gMonsterPitch=std::clamp(gMonsterPitch+sign*
        static_cast<double>(raw->data.mouse.lLastY)*
        static_cast<double>(gCryptidPitchSensitivity.load()),-82.0,82.0);
    if(gMonsterYaw>180.0)gMonsterYaw-=360.0;
    if(gMonsterYaw< -180.0)gMonsterYaw+=360.0;
}
void HunterNative::DrawMonsterHUD(ImVec2 screen) {
    if(!gMonsterActive.load())return;
    MonsterHUDState snap{};
    {
        std::lock_guard<std::mutex> lock(gMonsterStatusMutex);
        snap=gMonsterHUD;
    }
    ImDrawList* draw=ImGui::GetForegroundDrawList();
    const ImU32 mainColor=snap.species==1?IM_COL32(129,169,236,255):
         snap.species==2?IM_COL32(220,80,88,255):
         snap.species==3?IM_COL32(194,223,173,255):
          IM_COL32(206,172,115,255);
    const char* title=snap.species==1?"MOTHMAN / NIGHT SKY HUNTER":
                      snap.species==2?"VAMPIRE / BLOOD STALKER":
                      snap.species==3?"WENDIGO / FOREST PREDATOR":
                                      "CRYPTID / MONSTER MODE";
    const char* trait=snap.species==1?"AERIAL HUNT // WINGED PREDATOR":
                      snap.species==2?"NOCTURNAL STALK // CLAW ATTACK":
                      snap.species==3?"FERAL HUNT // CLOSE COMBAT":
                                      "PREDATOR CONTROLS";
    const ImVec2 tl{14,14};
    const ImVec2 tr{std::min(screen.x-14.f,560.f),108.f};
    draw->AddRectFilled(tl,tr,IM_COL32(10,16,17,232),6.f);
    draw->AddRect(tl,tr,mainColor,6.f,0,2.f);
    draw->AddText(ImVec2(28,24),mainColor,title);
    draw->AddText(ImVec2(28,45),IM_COL32(214,219,209,255),trait);
    if(snap.maxHealth>0.0) {
        const float pct=std::clamp(
            static_cast<float>(snap.health/snap.maxHealth),0.f,1.f);
        draw->AddRectFilled(ImVec2(28,70),ImVec2(tr.x-20,83),
            IM_COL32(44,53,48,245),3.f);
        draw->AddRectFilled(ImVec2(28,70),
            ImVec2(28+(tr.x-48)*pct,83),mainColor,3.f);
    }
    char health[140]{};
    if(snap.respawning)
        snprintf(health,sizeof(health),"RESPAWN IN %.1fs  |  CAMERA %s",
            snap.respawnSeconds,snap.cameraReady?"ATTACHED":"NOT READY");
    else if(snap.species==1)
        snprintf(health,sizeof(health),"MOTHMAN ACTIVE / HP UNAVAILABLE  |  CAMERA %s",
            snap.cameraReady?"CHASE":"FALLBACK");
    else
        snprintf(health,sizeof(health),"HEALTH %.0f / %.0f  |  CAMERA %s",
            snap.health,snap.maxHealth,snap.cameraReady?"CHASE":"FALLBACK");
    draw->AddText(ImVec2(28,88),IM_COL32(223,209,180,255),health);
    const float x=screen.x*.5f,y=screen.y*.5f;
    draw->AddCircle({x,y},9.f,mainColor,20,1.3f);
    draw->AddLine({x-16,y},{x-7,y},mainColor,1.3f);
    draw->AddLine({x+7,y},{x+16,y},mainColor,1.3f);
    draw->AddLine({x,y-16},{x,y-7},mainColor,1.3f);
    draw->AddLine({x,y+7},{x,y+16},mainColor,1.3f);
    const float panelWidth=std::min(screen.x-30.f,790.f);
    const ImVec2 bottom{(screen.x-panelWidth)*.5f,screen.y-146.f};
    const ImVec2 endPanel{bottom.x+panelWidth,bottom.y+128.f};
    draw->AddRectFilled(bottom,endPanel,IM_COL32(10,14,13,232),6.f);
    draw->AddRect(bottom,endPanel,mainColor,6.f,0,1.f);
    const char* moveHint=snap.species==1 ?
        "MOTHMAN  |  WASD FLY  |  SPACE ASCEND  |  CTRL DESCEND  |  SHIFT BOOST" :
        snap.species==3 ?
        "WENDIGO  |  WASD WALK  |  SPACE JUMP  |  SHIFT SPRINT  |  NO FLIGHT" :
        snap.species==2 ?
        "VAMPIRE  |  WASD WALK  |  SPACE JUMP  |  SHIFT SPRINT  |  NO FLIGHT" :
        "CRYPTID  |  WASD WALK  |  SPACE JUMP  |  SHIFT SPRINT";
    draw->AddText(ImVec2(bottom.x+14,bottom.y+10),
        IM_COL32(232,215,177,255),snap.respawning?
        "RESPAWNING: CAMERA AND HUD WILL FOLLOW NEW CREATURE":moveHint);
    draw->AddText(ImVec2(bottom.x+14,bottom.y+32),mainColor,
        "ACTIVE NATIVE SKILLS (ONLY THIS CRYPTID)  |  F11 RETURN HUMAN");
    int shown=0;
    for(int slot=0;slot<4;++slot) {
        if(!snap.available[static_cast<size_t>(slot)] ||
           snap.skills[static_cast<size_t>(slot)].empty())continue;
        const auto& skill=snap.skills[static_cast<size_t>(slot)];
        const int column=shown%2,row=shown/2;
        draw->AddText(ImVec2(bottom.x+14+column*(panelWidth*.5f),
                             bottom.y+57+row*22),
                      IM_COL32(223,230,209,255),skill.c_str());
        ++shown;
    }
    if(shown==0)
        draw->AddText(ImVec2(bottom.x+14,bottom.y+63),
            IM_COL32(179,187,181,255),
            "No compatible native attack/skill methods loaded for this creature");
    if(snap.respawning)
        draw->AddText(ImVec2(bottom.x+14,bottom.y+108),
            IM_COL32(227,179,109,255),"Skill actions paused while respawning");

}

bool HunterNative::RequestMonsterAbility(int slot) {
    if(slot<0 || slot>5 || !gMonsterActive.load())return false;
    return Enqueue({ActionType::MonsterAbility,"",slot,0,-1,0});
}
void HunterNative::SetMonsterAttackSound(int soundObjectIndex) {
    gMonsterAttackSoundIndex.store(soundObjectIndex>=0?soundObjectIndex:-1);
}

bool HunterNative::RequestEquipVanity(const std::string& name) {
    if(name.empty() || name.size()>160)return false;
    return Enqueue({ActionType::EquipVanity,name,0,0,-1,0});
}

int HunterNative::MonsterSpecies() {
    if(!gMonsterActive.load())return 0;
    std::lock_guard<std::mutex> guard(gMonsterStatusMutex);
    return gMonsterHUD.species;
}

bool HunterNative::NoclipActive() { return gNoclipEnabled; }
