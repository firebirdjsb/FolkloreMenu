# Three-map Dumper-7 comparison — 19 November 2026

Three user-uploaded complete runtime SDK dumps from **Wendigo**, **Bloodfang**, and **Operation Blackwing**, all on Unreal Engine 5.5.4, compared by object/class identity and loaded asset declarations. This is a **static, map-loaded-object audit**, not a gameplay verification.

## Loaded concrete creatures

| Actual class | Wendigo map | Bloodfang | Operation Blackwing |
| --- | --- | --- | --- |
| `BP_Wendigo_C` | Class + 1 live pawn | Absent | Absent |
| `BP_Vampire_C` | Absent | Class + 3 live pawns | Absent |
| `BP_VampireBase_C` (base, **not** concrete creature) | Absent | Class only | Class only |
| `BP_FlyingPawnTest_C` (Mothman) | Class, no live pawn | Class, no live pawn | Class + 1 live pawn |
| `BP_MobSpawner_C` | Class, no live spawner | Class, no live spawner | Class + 4 BioLab live spawners |

Native live AI controllers: Wendigo: `AIController_Wendigo_C` (one); Bloodfang: `Vampire_AIController_C` (three); Blackwing: `BP_AIController_Mothman_C` (one). Mothman mesh and flight animation assets are loaded in all three snapshots, but the pawn/controller are live only in Blackwing.

**Key conclusion:** loaded `BP_VampireBase_C` and `BP_Cryptid_Base_C` are not playable concrete spawn species. A `UClass` alone cannot guarantee a mesh, animation graph, cooked asset dependencies or an AI controller. Never report success for a blank pawn or silently fall back to a default map mob when a different species was requested.

## Ground locomotion evidence

From `BP_Cryptid_Base_classes.hpp`: double `WalkSpeed` +0x698, `RunSpeed` +0x6A0; replicated enum `Speed` +0x690, function `OnRep_Speed()`, `Server_SetMoveSpeed(EMovementMode)`. The enum values are generically named; no mode-to-value mapping is asserted.

From `Engine_classes.hpp`: `ACharacter::CharacterMovement` +0x330; `UCharacterMovementComponent::MaxWalkSpeed` +0x268; `MaxFlySpeed` +0x274. UE native `Pawn.AddMovementInput` has params of size 0x20, and `AIController.MoveToLocation` has params of size 0x30 with return enum +0x29.

Wendigo's AnimBP has `ForwardSpeed` +0x970 and `RunSpeedThreshold` +0x978. It has native melee montage events/AI state changes. Vampire has `VAMPIRE_Skeleton_AnimBlueprint_C`, `BP_VampireBase_C::Stance` +0x800 and `IsInvisible` +0x7D0; actual creatures on Bloodfang are `BP_Vampire_C`.

**Implemented:** Ground movement no longer teleports when the AI path fails; it tries native `Pawn.AddMovementInput`, respects real `WalkSpeed/RunSpeed`, maintains a native path for up to 1.25 s between refreshes, and lets the AI steer its rotation during motion. The movement path and its multiplayer animations still require in-game testing.

## Native Mothman flight

`BP_FlyingPawnTest_C` is a separate `APawn`, has native `FlightTarget` +0x400, `FlightSpeed` +0x420, `Smoke` Niagara +0x3A8 and original AnimBlueprint `IsFlying` +0x1310 / `HSpeed` +0x1314 / `FSpeed` +0x1318. Do not write CharacterMovement offsets to Mothman. Its ESP marker is standard cryptid **red**. The local possession-only smoke occlusion uses nonreplicated `SceneComponent.SetHiddenInGame` and restores prior visibility afterward; do not modify replicated `bVisible`.

## Follow-up actions

1. **Wendigo:** test normal AI walking/running, direction changes, animation and attack montages, and restore on F11.
2. **Bloodfang:** test each of three Vampire actors, native stance/invisibility, walking vs sprinting speed, and animations.
3. **Blackwing:** test native Mothman flying acceleration and wing animation with camera following, plus local-only mist occlusion.
4. **Foreign spawning:** only expose concrete loaded classes, label maps without a live instance as experimental, validate mesh/AnimClass/AIController, and reject incomplete actors. To actually load Wendigo on Blackwing or Bloodfang, we still need a validated cooked asset path/Blueprint initialization routine; these SDK declarations alone do not provide one.
5. Observe remotely from a second client before claiming particle suppression, host movement or spawned NPCs are network-safe.

The full 3-map comparison with individual GObjects instance IDs is available separately as `FolkloreHunter-ThreeMap-SDK-Comparison-2026-11-19.md`.
