# Folklore Hunter Journal — cryptid-control / online scares release candidate

Target: Folklore Hunter UE 5.5.4 SDK snapshot, Windows x64. Native ASI plus F8 ImGui journal.

## Repairs included
- **Puppet control**: leave the local human pawn possessed and alive; temporarily disable human pawn input and suppress Controller movement/look while directing the cryptid. Stop AIController path-following and pause its BrainComponent. Save/restore the monster's movement mode and speed, human input, camera, and HUD on F11 / Restore Human. WASD + raw mouse are sampled for the cryptid. No spectator/controller possession swap.
- **Cryptid spawning**: queue 1–10 requests and distribute across live map mob spawners. When a verified cryptid-derived pawn class and GameplayStatics deferred spawning are available, spawn independent actors through `BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` and verify each returned actor is a cryptid. Otherwise use the map's `TriggerSpawn` method, which may enforce cooldowns/limits. Show **confirmed direct creations** separately from remaining dispatch requests; a successful fallback dispatch is not proof an actor spawned.
- **No-clip**: no longer swallow WASD/Space/Ctrl/Shift window messages in no-clip mode. Try the native CharacterMovement flying mode, disable collision, and keep MaxFlySpeed set; restore original state on disable or partial setup failure.
- **Cryptid ESP**: read cryptid replicated Health (0x710) and MaxHealth (0x718) in the SDK target; overlay a bar and a numeric current/maximum indicator, with unknown-state guard. Selected cryptid also shows HP.
- **Weather**: clear rain/snow using native intensity setters plus final disabled flags, then verify. The single 'CLEAR ALL RAIN + SNOW' button applies both in one queued command.
- **Online game sounds**: scan loaded USoundBase assets; a host may choose an in-game sound and dispatch `PlayerController.ClientPlaySound` on the selected player controller or all scanned players.
- **Online visual scare option**: scan loaded non-base CameraShakeBase classes, and a host may invoke `PlayerController.ClientStartCameraShake` on the selected player or all scanned players. Combine with an in-game sound for a network-native scare. This is **not** a replica of the local custom ImGui image/stinger: remote custom render art is impossible without a receiving client mod or an existing game-native equivalent.

## Release gate: tests required
Compiling a Windows x64 ASI and passing PE validation does not confirm any game action at runtime. Verify on the exact game build:
1. Start solo/host hunt with an active cryptid. Open F8, select cryptid, choose CONTROL CRYPTID. F8 close. Verify WASD, mouse look, LMB ability, AI no longer takes over, human weapon cannot fire, human remains present. F11 restores human movement/shooting and cryptid AI.
2. Spawn 1 then 3–5 (host). Verify actual actor count increases, spawned creatures move, and a second client sees them. The 'Directly created' indicator only counts actors returned by native deferred spawn and identified as cryptids.
3. Enable no-clip. Test WASD, Space, Ctrl, Shift, collision bypass; disable and verify original movement and collision.
4. Verify cryptid ESP HP responds to real damage and is shown as current/max.
5. Rain, snow, then CLEAR ALL RAIN + SNOW; confirm particles and intensity stop.
6. Two-player private lobby: select player B and choose a loaded sound; confirm B alone hears it. Trigger a loaded camera shake separately; confirm B alone sees it. Then test ALL. Some levels may not load a usable shake class.
7. Exit hunt/rejoin, switch cryptids, and restore human repeatedly; verify no stranded input or camera and inspect journal.log.

## Installation and logs
Download the latest successful GitHub Actions artifact ZIP from this branch. Place **only** `FolkloreHunterJournal.asi` under the existing Ultimate ASI Loader plugin location; do not install both the .asi and DLL copy. Remove older journal ASIs before testing. F8 opens/closes, F11 restores human during puppet control, F9 toggles debug overlay. Logs: `%LOCALAPPDATA%\FolkloreHunterJournal\journal.log` and crash-trace.log.

No anti-cheat bypass is included. Test only in private sessions with other players' permission. **Not marked final release until live host + remote client validation passes.**
