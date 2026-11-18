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
#include <mutex>
#include <vector>
#include <unordered_map>
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
    struct Marker { Vec3 pos{}; int kind{}; double health{-1.0}, maxHealth{-1.0}; };
    struct Camera { Vec3 pos{}; Rot rot{}; float fov{}; };
    struct ActorEntry { uintptr_t address{}; int index{}; int kind{}; };
    struct LevelView { uintptr_t data{}; int32_t count{}; int32_t start{}; };
    enum class ActionType { GiveItem, CatalogItem, GodMode, AddXP, Time, TimeScale, AddSilver, InfiniteDamage, BeanMode, RangerMode, SmoreMode, Rain, Snow, VampireScream, FlickerLights, CancelFlicker, TargetHealth, TargetItem, TargetNoise, TargetRevive, TargetKill, TargetKick, TargetGodMode, NoclipOn, NoclipOff, FallProtection, SpeedChange, JumpBoost, GravityChange, SpawnMob, PossessCryptid, RestoreHuman, MonsterAbility, EquipVanity };
    struct Action { ActionType type{}; std::string item; int amount{}; float value{}; int target{-1}; double precise{}; };
    std::mutex gMutex;
    std::mutex gActionsMutex;
    HWND gWindow=nullptr;
    ULONGLONG gLastTick=0;
    std::atomic_bool gTickPending{false};
    std::atomic_bool gActionPending{false};
    std::vector<Action> gActions;
    std::vector<ActorEntry> gKnown;
    std::vector<Marker> gMarkers;
    std::vector<HunterNative::PlayerInfo> gRoster;
    std::vector<HunterNative::CreatureInfo> gCreatures;
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
    bool gPossessedCryptid = false;
    std::atomic_bool gWildlife{true}, gContainers{true}, gQuest{true}, gLoot{true};
    
    uintptr_t gNoclipPawn = 0;
    uint8_t gPriorMovement = 1;
    bool gPriorCollision = true;
    bool gNoclipEnabled = false;
    float gOriginalFlySpeed=0.f;
    uintptr_t gOriginalFlyMovement=0;
    bool gFlySpeedSaved=false;
    uintptr_t gMonsterAIController=0, gMonsterBrain=0;
    bool gMonsterBrainWasRunning=false;
    uintptr_t gMonsterMovement=0;
    float gMonsterOriginalWalkSpeed=0.f, gMonsterOriginalFlySpeed=0.f;
    bool gMonsterSpeedsSaved=false;
    uintptr_t gHumanHUD=0;
    int32_t gHumanHUDIndex=-1;
    uint8_t gPriorHumanHudVisibility=0;
    bool gHumanHudHidden=false;
    bool gHudRestorePending=false;
    int gVampireStance=0;
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
    ULONGLONG gLastMotionTick=0, gLastScanTick=0, gLastAttackTick=0;
    std::mutex gMonsterStatusMutex;
    struct MonsterHUDState {
        std::string label{"CRYPTID"};
        int species=0;
        double health=0.0, maxHealth=0.0;
        bool cameraReady=false;
        bool possessConfirmed=false;
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
    int ClassKind(uintptr_t cls) {
        if (!cls) return 0;
        if (auto it=gKinds.find(cls);it!=gKinds.end()) return it->second;
        uintptr_t current=cls;
        int result=0;
        for (int depth=0;depth<24 && current;++depth) {
            const auto name=ClassName(current);
            if (name==L"PlayerCharacter_C") {result=1;break;}
            if (name==L"BP_Cryptid_Base_C" || name==L"CryptidBase") {result=2;break;}
            const auto parent=ReadPtr(current+0x40);
            if (!parent || parent==current)break;
            current=parent;
        }
        if(!result) {
            const auto name=Lower(ClassName(cls));
            // Class-name categories are heuristics. This supplements actual inheritance checks.
            // Exclude level-only trigger/volume components to reduce false markers.
            if(Matches(name,{L"collision",L"trigger",L"volume",L"widget",L"component",L"preview"}))
                return 0;
            if(Matches(name,{L"wolf",L"deer",L"bear",L"boar",L"animal",L"wildlife",L"npc",L"mob_",L"creature"}))
                result=3;
            else if(Matches(name,{L"chest",L"crate",L"container",L"lootbox",L"loot_box",L"box_"}))
                result=4;
            else if(Matches(name,{L"quest",L"mission",L"objective",L"contract_",L"collectible"}))
                result=5;
            else if(Matches(name,{L"droppeditem",L"pickup",L"loot_",L"itemdrop",L"placeable",L"supply"}))
                result=6;
        }
        // Don't cache negative names. FNames may be unavailable while streaming new maps.
        if(result) gKinds.emplace(cls,result);
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
            gPrevWorld=world;
            gScanIndex=0;
            gKinds.clear();gClassNames.clear();gFunctionCache.clear();
            gKnown.clear();
            gPhysicsBackups.clear();gFallBackups.clear();gGodBackups.clear();
            gHumanPawn=0;gHumanIndex=-1;gPossessedCryptid=false;
            gMonsterActive.store(false);gMonsterPawn=0;gChaseCamera=0;
            gCameraAutoManageSaved=false;
            gMonsterIndex=-1;gCameraIndex=-1;gLastMotionTick=0;
            gMonsterAIController=0;gMonsterBrain=0;gMonsterBrainWasRunning=false;
            gMonsterMovement=0;gMonsterSpeedsSaved=false;gPuppeteering=false;
            gHumanHUD=0;gHumanHudHidden=false;gHudRestorePending=false;
            gChaseSmoothedValid=false;
            gGravityBackup=WorldGravityBackup{};
            gNoclipPawn=0;gNoclipEnabled=false;
            FHJDiag::Log(L"World changed: reset ESP and player cache.");
        }
        HunterNative::Status status{};
        Camera cam{};
        std::vector<Marker> markers;
        std::vector<HunterNative::PlayerInfo> playersOut;
        std::vector<HunterNative::CreatureInfo> creaturesOut;
        status.worldReady=world!=0;
        if(world) {
            status.host=ReadPtr(world+0x158)!=0;
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
            const int maxScan=esp ? 700 : 180; // player list remains available when ESP is off
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
                const int kind=ClassKind(ReadPtr(actor+0x10));
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
                    // Reflected BP_Cryptid_Base_C::Health / MaxHealth (0x710 / 0x718).
                    if(!Read(actor.address+0x710,creature.health) ||
                       !std::isfinite(creature.health) || creature.health<0)
                        creature.health=-1.0;
                    if(!Read(actor.address+0x718,creature.maxHealth) ||
                       !std::isfinite(creature.maxHealth) || creature.maxHealth<=0)
                        creature.maxHealth=-1.0;
                    const auto name=ClassName(cls);
                    creature.displayName=std::string(name.begin(),name.end());
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
                    if(!Read(actor.address+0x710,actorHealth) ||
                       !std::isfinite(actorHealth) || actorHealth<0)actorHealth=-1.0;
                    if(!Read(actor.address+0x718,actorMaxHealth) ||
                       !std::isfinite(actorMaxHealth) || actorMaxHealth<=0)actorMaxHealth=-1.0;
                } else if(actor.kind==1) {
                    if(!Read(actor.address+0x8A0,actorHealth) ||
                       !std::isfinite(actorHealth) || actorHealth<0)actorHealth=-1.0;
                    if(!Read(actor.address+0x8A8,actorMaxHealth) ||
                       !std::isfinite(actorMaxHealth) || actorMaxHealth<=0)actorMaxHealth=-1.0;
                }
                markers.push_back({loc,actor.kind,actorHealth,actorMaxHealth});
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
            gVanityChoices=std::move(vanity);
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
        const bool changed=TryWrite(flag,enabled);
        alignas(8) uint8_t args[0x30]{};
        std::memcpy(args,&intensity,sizeof(intensity));
        if(!Invoke(actor,func,args)) {
            FHJDiag::Log(L"WEATHER: UFunction invocation failed.");
            return false;
        }
        if(changed && enabled) {
            // Spawn/initialize weather particle systems when enabling rain/snow
            // on maps whose BP_WeatherTool started with that effect disabled.
            const auto setup=FindFunction(L"BP_WeatherTool_C",L"Weather Setup Function");
            if(setup)Invoke(actor,setup,nullptr);
        }
        bool readFlag=false;
        Read(flag,readFlag);
        FHJDiag::Log(changed && readFlag==enabled?
            L"WEATHER: live weather tool flag verified and intensity function dispatched (local visibility only).":
            L"WEATHER: intensity dispatched, but Enable_Rain/Snow flag could not be verified.");
        return changed;
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
                gNoclipPawn=pawn;gNoclipEnabled=true;
                gOriginalFlyMovement=movement;
                gFlySpeedSaved=Read(movement+0x274,gOriginalFlySpeed);
                if(gFlySpeedSaved) {
                    const float speed=1150.f;
                    TryWrite(movement+0x274,speed); // SDK MaxFlySpeed
                }
            }
        } else if(gNoclipPawn!=pawn) {
            FHJDiag::Log(L"Noclip restore skipped: pawn changed. Base movement applies.");return;
        }
        alignas(8) unsigned char moveArgs[0x8]{};
        moveArgs[0]= enabled?5:gPriorMovement; // UE MOVE_Flying=5; restore prior mode
        moveArgs[1]=0;
        bool canCollide=enabled?false:gPriorCollision;
        Invoke(movement,moveFunc,moveArgs);
        Invoke(pawn,collisionFunc,&canCollide);
        if(!enabled) {
            if(gFlySpeedSaved && gOriginalFlyMovement==movement)
                TryWrite(movement+0x274,gOriginalFlySpeed);
            gFlySpeedSaved=false;gOriginalFlyMovement=0;
            gNoclipEnabled=false;gNoclipPawn=0;
        }
        FHJDiag::Log(enabled?L"Local noclip flying/collision request dispatched.":
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

    bool SetActorLocationSafe(uintptr_t actor,const Vec3& next) {
        const auto f=FindFunction(L"Actor",L"K2_SetActorLocation");
        if(!actor || !f)return false;
        alignas(8) uint8_t args[0x128]{};
        std::memcpy(args,&next,sizeof(next));
        args[0x18]=0; // bSweep = false (explicit noclip/ghost movement)
        args[0x120]=1; // bTeleport = true
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
        FHJDiag::Log(L"Created camera actor and switched view target for monster mode.");
        return true;
    }
    void ReleaseMonsterCamera(uintptr_t pc,uintptr_t destination) {
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
        HunterNative::CreatureInfo dummy;
        std::string name;
        auto cls=ReadPtr(gMonsterPawn+0x10);
        auto wide=ClassName(cls);
        name.assign(wide.begin(),wide.end());
        int species=0;
        auto lower=Lower(wide);
        if(lower.find(L"moth")!=std::wstring::npos || lower.find(L"flying")!=std::wstring::npos)species=1;
        else if(lower.find(L"vamp")!=std::wstring::npos)species=2;
        else if(lower.find(L"wend")!=std::wstring::npos)species=3;
        double health=0,maxHealth=0;
        Read(gMonsterPawn+0x710,health);
        Read(gMonsterPawn+0x718,maxHealth);
        std::lock_guard<std::mutex> lock(gMonsterStatusMutex);
        gMonsterHUD.label=name.empty()?"UNKNOWN CRYPTID":name;
        gMonsterHUD.species=species;
        gMonsterHUD.health=health;
        gMonsterHUD.maxHealth=maxHealth;
        gMonsterHUD.cameraReady=gChaseCamera!=0;
        gMonsterHUD.possessConfirmed=true;
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
    void StopMonsterAI() {
        const auto ai=ReadPtr(gMonsterPawn+0x2D8); // APawn.Controller
        if(!ai)return;
        const auto brain=ReadPtr(ai+0x388); // SDK AIModule.AAIController.BrainComponent
        if(!brain)return;
        const auto state=FindFunction(L"BrainComponent",L"IsRunning");
        bool running=false;
        if(state)Invoke(brain,state,&running);
        gMonsterAIController=ai;
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
    void RestoreMonsterAI() {
        if(gMonsterMovement && gMonsterSpeedsSaved) {
            TryWrite(gMonsterMovement+0x268,gMonsterOriginalWalkSpeed);
            TryWrite(gMonsterMovement+0x274,gMonsterOriginalFlySpeed);
        }
        gMonsterMovement=0;
        gMonsterSpeedsSaved=false;
        if(gMonsterBrain && gMonsterBrainWasRunning) {
            const auto restart=FindFunction(L"BrainComponent",L"RestartLogic");
            if(restart)Invoke(gMonsterBrain,restart,nullptr);
            FHJDiag::Log(L"Monster Mode: previous AI behavior restored.");
        }
        gMonsterBrain=0;gMonsterAIController=0;gMonsterBrainWasRunning=false;
    }
    void ConfigureMonsterMovement() {
        gMonsterMovement=ReadPtr(gMonsterPawn+0x330); // ACharacter.CharacterMovement
        if(!gMonsterMovement)return;
        gMonsterSpeedsSaved=Read(gMonsterMovement+0x268,gMonsterOriginalWalkSpeed) &&
            Read(gMonsterMovement+0x274,gMonsterOriginalFlySpeed);
        if(!gMonsterSpeedsSaved)return;
        const int species=gMonsterHUD.species;
        const float walk=species==2?610.f:species==3?540.f:700.f;
        const float fly=species==1?1200.f:850.f;
        TryWrite(gMonsterMovement+0x268,walk);
        TryWrite(gMonsterMovement+0x274,fly);
        FHJDiag::Log(L"Monster Mode: native CharacterMovement speed profile enabled.");
    }
    void ExecuteMonsterAbility(int slot) {
        if(!gMonsterActive.load() || !ReadPtr(World()+0x158))return;
        int32_t index=-1;
        if(!Read(gMonsterPawn+0x0C,index) || index!=gMonsterIndex)return;
        const auto now=GetTickCount64();
        if(now-gLastAbilityTick<650)return; // bound ability spam
        const auto cls=ReadPtr(gMonsterPawn+0x10);
        bool vampire=false,cryptid=false;
        uintptr_t parent=cls;
        for(int depth=0;parent && depth<25;++depth) {
            const auto n=ClassName(parent);
            if(n==L"BP_VampireBase_C")vampire=true;
            if(n==L"BP_Cryptid_Base_C")cryptid=true;
            const auto next=ReadPtr(parent+0x40);
            if(!next || next==parent)break;
            parent=next;
        }
        uintptr_t fn=0;
        alignas(8) uint8_t args[0x20]{};
        switch(slot) {
            case 0:
                // Native melee method preserves the creature's anim montage chain.
                fn=FindFunction(vampire?L"BP_VampireBase_C":L"BP_Cryptid_Base_C",L"Melee");
                break;
            case 1: if(vampire)fn=FindFunction(L"BP_VampireBase_C",L"AUTH_Scream");break;
            case 2: if(vampire)fn=FindFunction(L"BP_VampireBase_C",L"Invisible");break;
            case 3: if(vampire) {
                fn=FindFunction(L"BP_VampireBase_C",L"SetStance");
                gVampireStance=(gVampireStance+1)%3;
                std::memcpy(args,&gVampireStance,4);
                break;
            }
            case 4: if(vampire)fn=FindFunction(L"BP_VampireBase_C",L"All_ScreamSound");break;
            default:break;
        }
        if(!fn && slot==0 && cryptid)fn=FindFunction(L"BP_Cryptid_Base_C",L"Server_Attack");
        if(!fn) {
            FHJDiag::Log(L"Monster ability unavailable: no verified no-arg native function for this species.");
            return;
        }
        if(Invoke(gMonsterPawn,fn,args)) {
            gLastAbilityTick=now;
            FHJDiag::Log(L"Monster ability dispatched on existing cryptid actor; animation/effect subject to game authority.");
        } else FHJDiag::Log(L"Monster ability reflection dispatch failed.");
    }
    void FreeflightTick() {
        const ULONGLONG now=GetTickCount64();
        const ULONGLONG elapsed=gLastMotionTick?now-gLastMotionTick:16;
        gLastMotionTick=now;
        if(!gWindow || gJournalOpen.load() || GetForegroundWindow()!=gWindow)return;
        const auto world=World();
        if(!world || world!=gPrevWorld)return;
        const bool monster=gMonsterActive.load();
        const auto pawn=monster?gMonsterPawn:LocalPawn();
        if(!pawn || (!monster && !gNoclipEnabled))return;
        int32_t index=-1;
        if(!Read(pawn+0x0C,index) || (monster && index!=gMonsterIndex)) {
            if(monster) {
                gMonsterActive.store(false);
                RestoreHumanHUD();
                FHJDiag::Log(L"Monster actor invalidated; removed monster overlay.");
            }
            return;
        }
        // Native character input is accumulated for CharacterMovement::Tick.
        // The old K2_SetActorLocation teleport-per-frame method was causing
        // both jitter and a permanently idle AnimBlueprint locomotion state.
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
        int species=0;
        if(monster) {
            std::lock_guard<std::mutex> guard(gMonsterStatusMutex);
            species=gMonsterHUD.species;
        }
        const bool flying=!monster || species==1; // Mothman uses flight; ground cryptids run.
        const double ascend=flying?
            static_cast<double>(down(VK_SPACE))-static_cast<double>(down(VK_CONTROL)):0.0;
        Vec3 input{cy*front-sy*strafe,sy*front+cy*strafe,ascend};
        if(flying) {
            input.x*=cp; input.y*=cp;
            input.z+=sp*front;
        }
        const double length=std::sqrt(input.x*input.x+input.y*input.y+input.z*input.z);
        if(length>1e-3) {
            input.x/=length;input.y/=length;input.z/=length;
            if(!AddSmoothMovement(pawn,input,1.f)) {
                static ULONGLONG warned=0;
                if(now-warned>8000) {
                    warned=now;
                    FHJDiag::Log(L"Movement input could not be dispatched. No teleport fallback used.");
                }
            }
        }
        const auto movement=ReadPtr(pawn+0x330);
        if(movement) {
            if(monster && gMonsterSpeedsSaved) {
                const float speed=boost?1.7f:1.f;
                const float walk=(species==2?610.f:species==3?540.f:700.f)*speed;
                const float fly=(species==1?1200.f:850.f)*speed;
                float current=0;
                if(Read(movement+0x268,current) && std::fabs(current-walk)>.5f)
                    TryWrite(movement+0x268,walk);
                if(Read(movement+0x274,current) && std::fabs(current-fly)>.5f)
                    TryWrite(movement+0x274,fly);
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
            RestoreHumanHUD();
            FHJDiag::Log(L"Monster control stopped: original player pawn changed externally.");
            return;
        }
        static ULONGLONG hudTick=0;
        if(now-hudTick>300) {
            hudTick=now;
            HideHumanHUD();
        }
        // Rotate the creature itself so its mesh and melee hit direction face the view.
        const Rot desired{0.0,gMonsterYaw,0.0};
        const auto turn=FindFunction(L"Actor",L"K2_SetActorRotation");
        if(turn) SetActorRotationSafe(pawn,desired);
        if(gChaseCamera) {
            Vec3 location{};
            if(Position(pawn,location)) {
                double distance=340.0,height=175.0;
                if(species==1){distance=540; height=260;}
                else if(species==2){distance=315;height=170;}
                else if(species==3){distance=410;height=235;}
                const Vec3 goal{
                    location.x-cy*distance,
                    location.y-sy*distance,
                    location.z+height-std::sin(pitch)*distance*.18
                };
                if(!gChaseSmoothedValid) {
                    gChaseSmoothedPos=goal;
                    gChaseSmoothedValid=true;
                } else {
                    const double alpha=1.-std::exp(-12.*dt);
                    gChaseSmoothedPos.x+=(goal.x-gChaseSmoothedPos.x)*alpha;
                    gChaseSmoothedPos.y+=(goal.y-gChaseSmoothedPos.y)*alpha;
                    gChaseSmoothedPos.z+=(goal.z-gChaseSmoothedPos.z)*alpha;
                }
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
    void SpawnCreatureWave(int count,int liveCryptidIndex) {
        const auto world=World();
        if(!world || !ReadPtr(world+0x158)) {
            FHJDiag::Log(L"Creature spawn rejected: host GameMode required.");return;
        }
        const auto spawn=FindFunction(L"BP_MobSpawner_C",L"TriggerSpawn");
        if(!spawn) {FHJDiag::Log(L"BP_MobSpawner_C.TriggerSpawn not loaded.");return;}
        // Optional type override uses a real currently loaded CryptidBase-derived
        // UClass, selected from a live actor. The source MobClass is restored.
        uintptr_t species=0;
        for(const auto& found:gKnown)
            if(found.kind==2 && found.index==liveCryptidIndex)
                species=ReadPtr(found.address+0x10);
        int total=0,sent=0;
        const auto levels=GetWorldLevels(world,total);
        for(const auto& level:levels) {
            for(int i=0;i<level.count && sent<count;++i) {
                const auto actor=ReadPtr(level.data+static_cast<size_t>(i)*8);
                if(!actor || ClassName(ReadPtr(actor+0x10))!=L"BP_MobSpawner_C")continue;
                // This spawn tool chooses the species configured on the map spawner.
                uintptr_t originalClass=0;
                const bool overrideClass=species && Read(actor+0x2B8,originalClass) &&
                    TryWrite(actor+0x2B8,species); // BP_MobSpawner_C::MobClass
                alignas(8) unsigned char args[0xA0]{};
                if(Invoke(actor,spawn,args))++sent;
                if(overrideClass && !TryWrite(actor+0x2B8,originalClass))
                    FHJDiag::Log(L"Spawner MobClass could not be restored; check world state.");
            }
            if(sent>=count)break;
        }
        wchar_t msg[190]{};
        swprintf_s(msg,L"Spawn dispatches: %d (species and spawn count follow level MobClass/SpawnCount)",sent);
        FHJDiag::Log(msg);
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
            RestoreHumanHUD();
            ReleaseMonsterCamera(pc,gHumanPawn?gHumanPawn:ReadPtr(pc+0x350));
            RestoreMonsterAI();
            gMonsterActive.store(false);
            gPuppeteering=false;gPossessedCryptid=false;
            gMonsterPawn=0;gMonsterIndex=-1;gChaseSmoothedValid=false;
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
            RestoreHumanHUD();
            ReleaseMonsterCamera(pc,gHumanPawn);
            RestoreMonsterAI();
            gMonsterActive.store(false);
        }
        gHumanPawn=human;
        gHumanIndex=humanIndex;
        gMonsterPawn=selected;
        gMonsterIndex=index;
        gChaseSmoothedValid=false;
        const Camera previous=GetCamera(world);
        gMonsterYaw=std::isfinite(previous.rot.yaw)?previous.rot.yaw:0.;
        gMonsterPitch=std::clamp(std::isfinite(previous.rot.pitch)?previous.rot.pitch:0.,-80.,80.);
        gLastMotionTick=0;
        gLastAbilityTick=0;
        gVampireStance=0;
        Vec3 pos{};
        if(!Position(selected,pos) || !CreateMonsterCamera(pc,pos)) {
            gMonsterPawn=0;gMonsterIndex=-1;
            ReleaseMonsterCamera(pc,gHumanPawn);
            FHJDiag::Log(L"Monster Mode cancelled: chase-camera setup failed; human still possessed.");
            return;
        }
        gMonsterActive.store(true);
        gPuppeteering=true;gPossessedCryptid=true;
        UpdateMonsterHUD();
        StopMonsterAI();
        ConfigureMonsterMovement();
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
            a.type==ActionType::PossessCryptid ||
            a.type==ActionType::RestoreHuman) {
            ExecutePhysics(a); return;
        }
        if(a.type==ActionType::NoclipOn || a.type==ActionType::NoclipOff) {
            ExecuteNoclip(a.type==ActionType::NoclipOn);return;
        }
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
            case ActionType::BeanMode:fn=L"EnableBeanMode";break;
            case ActionType::RangerMode:fn=L"EnableRangerMode";break;
            case ActionType::SmoreMode:fn=L"EnableSmoreMode";break;
            case ActionType::GiveItem:fn=L"GiveItem";break;
            case ActionType::GodMode:fn=L"ToggleGodMode";break;
            case ActionType::AddXP:fn=L"AddXP";break;
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
            case ActionType::NoclipOn:
            case ActionType::NoclipOff:
            case ActionType::FallProtection:
            case ActionType::SpeedChange:
            case ActionType::JumpBoost:
            case ActionType::GravityChange:
            case ActionType::SpawnMob:
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
                  a.type==ActionType::SmoreMode) {
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
        const ImU32 color=colors[marker.kind];
        const float radius=std::clamp(14.f-static_cast<float>(meters)*.018f,4.f,14.f);
        dl->AddCircle(pt,radius,color,12,1.4f);
        dl->AddLine({pt.x-5,pt.y},{pt.x+5,pt.y},color,1.2f);
        dl->AddLine({pt.x,pt.y-5},{pt.x,pt.y+5},color,1.2f);
        char label[160]{};
        if(marker.health>=0 && marker.maxHealth>0)
            snprintf(label,sizeof(label),"%s %.0fm | HP %.0f / %.0f",
                names[marker.kind],meters,marker.health,marker.maxHealth);
        else if(marker.health>=0)
            snprintf(label,sizeof(label),"%s %.0fm | HP %.0f",
                names[marker.kind],meters,marker.health);
        else snprintf(label,sizeof(label),"%s %.0fm | HP ?",names[marker.kind],meters);
        const float left=pt.x+radius+4;
        dl->AddText({left,pt.y-11},color,label);
        if(marker.kind==2 && marker.health>=0 && marker.maxHealth>0) {
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
    constexpr double sensitivity=0.12;
    gMonsterYaw+=static_cast<double>(raw->data.mouse.lLastX)*sensitivity;
    gMonsterPitch=std::clamp(gMonsterPitch-
        static_cast<double>(raw->data.mouse.lLastY)*sensitivity,-82.0,82.0);
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
    const float pct=snap.maxHealth>0?std::clamp(
        static_cast<float>(snap.health/snap.maxHealth),0.f,1.f):0.f;
    draw->AddRectFilled(ImVec2(28,70),ImVec2(tr.x-20,83),IM_COL32(44,53,48,245),3.f);
    draw->AddRectFilled(ImVec2(28,70),ImVec2(28+(tr.x-48)*pct,83),mainColor,3.f);
    char health[110]{};
    snprintf(health,sizeof(health),"HEALTH %.0f / %.0f  |  CAMERA %s",
        snap.health,snap.maxHealth,snap.cameraReady?"CHASE":"FALLBACK");
    draw->AddText(ImVec2(28,88),IM_COL32(223,209,180,255),health);
    const float x=screen.x*.5f,y=screen.y*.5f;
    draw->AddCircle({x,y},9.f,mainColor,20,1.3f);
    draw->AddLine({x-16,y},{x-7,y},mainColor,1.3f);
    draw->AddLine({x+7,y},{x+16,y},mainColor,1.3f);
    draw->AddLine({x,y-16},{x,y-7},mainColor,1.3f);
    draw->AddLine({x,y+7},{x,y+16},mainColor,1.3f);
    const float panelWidth=std::min(screen.x-30.f,745.f);
    const ImVec2 bottom{(screen.x-panelWidth)*.5f,screen.y-73.f};
    draw->AddRectFilled(bottom,ImVec2(bottom.x+panelWidth,bottom.y+55),
        IM_COL32(10,14,13,223),6.f);
    draw->AddRect(bottom,ImVec2(bottom.x+panelWidth,bottom.y+55),mainColor,6.f,0,1.f);
    draw->AddText(ImVec2(bottom.x+14,bottom.y+10),IM_COL32(232,215,177,255),
        "WASD MOVE   |   MOUSE LOOK   |   SPACE UP   |   CTRL DOWN   |   SHIFT BOOST");
    draw->AddText(ImVec2(bottom.x+14,bottom.y+29),mainColor,
        "LMB ATTACK   |   Q SCREAM   |   E INVISIBILITY   |   R STANCE   |   F11 HUMAN");
}

bool HunterNative::RequestMonsterAbility(int slot) {
    if(slot<0 || slot>5 || !gMonsterActive.load())return false;
    return Enqueue({ActionType::MonsterAbility,"",slot,0,-1,0});
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
