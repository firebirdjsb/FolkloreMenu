# Folklore Hunter SDK audit — 2026-11-19

Source: user-uploaded `5.5.4-40574608+++UE5+Release-5.5-FolkloreHunter(2).zip`, Unreal Engine 5.5.4 Dumper-7 dump.

## Scope
- Extracted 1,714 archive entries / 89,824,703 uncompressed bytes.
- Indexed all 594 `CppSDK/SDK/*_classes.hpp` headers, 4,989 class-like declarations and 14,045 method-like signature lines (static regex inventory; not unique UFunctions).
- Native declarations do **not** contain Blueprint event graphs, verify authority, or prove gameplay/replication.

## Mothman root cause and repairs
- `BP_FlyingPawnTest_C` inherits **APawn**, not `BP_Cryptid_Base_C`. The old scanner therefore excluded the Mothman gameplay pawn from ESP and the playable list.
- Its SDK identifies `BP_StuckDetector` at `0x330` (not `CharacterMovement`); `FlightTarget` at `0x400`, `MaxVelocity` at `0x418`, `FlightSpeed` at `0x420`, `IsStealthed` at `0x431`.
- Mothman **does not** declare base cryptid Health / MaxHealth at `0x710`/`0x718`. Display HP as unknown; never dereference those offsets on a Mothman pawn.
- Its actual callable candidate functions include `DarknessAttack()`, `ForceScreech()`, and `RetriggerScreech()`. `Multicast_AllScreech(USoundBase*)` requires a sound parameter and must not be treated as a no-argument skill.
- `BP_AIController_Mothman_C` manages native flight/combat AI; unmodified runtime behavior still requires testing.
- Implemented separate Mothman classification, roster, ESP, ability and flight-target paths. **No Mothman in-game test has yet been completed.**

## Multi-map cryptid spawning
- `BP_MobSpawner_C` exposes `MobClass` at `0x2B8`, `SpawnedMobs` at `0x2F8`, `SpawningEnabled` at `0x308`, `TriggerSpawn()` and `SetSpawningEnabled(bool)`.
- UI discovers currently **loaded** cryptid UClasses and active UWorld map spawner actors. Select species and spawn point; optionally auto-control first verified direct spawn. Without a map spawner, explicit-class requests can spawn near host pawn.
- `FL_Gamestate_C::HuntData` at `0x340` identifies current hunt. Story classes include `Hunt_NightOfTheWendigo_C`, `Hunt_BloodfangForest_C`, `Hunt_Mothman_C`.
- Unloaded cooked classes cannot be summoned by knowing their SDK names; cross-map loading and network visibility require runtime verification. `TriggerSpawn` dispatch is not proof that an actor spawned.

## Camera and movement
- Ground cryptids keep longer-lived native AI path requests rather than reissuing MoveToLocation every 180 ms, reducing target resets. Mothman uses its own APawn flight attributes and a swept fallback.
- User-adjustable yaw/pitch mouse sensitivity, pitch inversion and camera-follow rate; use `journal.ini` to persist settings. Camera collision trace and blur-disable request still need in-world testing.

## Additional native surfaces and future opportunities
| Area | SDK evidence | Potential next feature | Status |
|---|---|---|---|
| Host developer tools | `FolkloreGamemode.AddXP`, `AddLevels`, `ToggleGodMode`, `GiveItem`, `SetTime`, `SetTimeScale` | `AddLevels` added; other host controls already present | Runtime verification required |
| Mode toggles | `EnableBeanMode`, `EnableRangerMode`, `EnableSmoreMode` | Mode audit by comparing runtime state before/after | Effects unknown |
| Finish hunt | `FL_Gamemode_C.Cheat_CompleteHunt(FText)` | Host-only match completion | Do not invoke until FText and gameplay flow verified |
| Weather | `BP_WeatherTool_C` mist, hail, fog, clouds, snow, sheet & bolt lightning fields | Advanced climate control and lightning | Not implemented |
| Objectives | `BP_Objective_Base_C.CompletePercentage`, `BP_JournalComponent_C.Cached_Objectives` | Read-only mission progress ESP | Not implemented |
| Native highlighting | `PlayerCharacter_C.SetActorHighlighted`, `ActorHighlighter` | In-engine highlights | Restore state and verify ownership first |
| Multiplayer/session | `FL_GameInstance_C` session create/join/update, `FL_Gamestate_C` travel | Host lobby diagnostics/map selection | Session/travel disruptive; not implemented |
| Player debug | `PlayerCharacter_C.Server_DebugToggleDeath`, `AskServer_SetHealth` | Explicit host diagnostics | Ownership/call validation needed |
| Vampire | `BP_VampireBase_C.SetStance(int32)`, `IsInvisible`, `AUTH_Scream()` | Verified stance and invisibility status | In-game tests pending |
| Cryptid attacks | `BP_Cryptid_Base_C.Server_Attack`, `Server_Vocalisation(GameplayTag)` | Species-specific native attack/audio | Audio tags require exact arguments |
| Spawner controls | `BP_MobSpawner_C.SetSpawningEnabled(bool)` | Enable/disable selected live spawner | Implemented; live effect unverified |

## Release gates
1. Solo/host Mothman story map: confirm `BP_FlyingPawnTest_C` appears in ESP and selected-cryptid list, with HP unknown.
2. Check Mothman flight, DarknessAttack, ForceScreech and animation. Do not claim gameplay verified until tested.
3. Confirm ground cryptids walk and turn smoothly; no unwanted flight/noclip.
4. Verify configurable camera look, wall clipping and postprocess blur.
5. On each story map, verify each loaded class's actual spawn count and camera handoff; test with second client.
6. Confirm AddLevels and native spawner enable/disable only in a private authoritative hunt.
7. Inspect `%LOCALAPPDATA%\\FolkloreHunterJournal\\journal.log` for missing UFunctions, class loads and authority failures.
