# Cryptid Hunt v0.2 — dual-ASI graphics bridge, native lobby, daily Wendigo cameras

## What the user asked

Two independently selectable mods, both usable at once; a visible "Cryptid Hunt loaded" indication on the real game main menu; a custom game-mode choice that avoids unintended save changes; the existing Folklore Hunter create/join lobby and its normal map selector; each map's original monster becomes the playable cryptid. Wendigo is unable to attack in daylight, can place only a limited number of cameras per game day, and becomes invisible while placing them.

## Build artifacts and hook ownership

- `FolkloreHunterJournal.asi` — its own x64 binary, general admin tools, ESP, host creature spawning; the possession UI was removed.
- `FolkloreCryptidHunt.asi` — its own x64 binary, loaded main-menu status, custom mode selector, possession/rules/role draft and Wendigo watchpoints.
- **One shared DXGI/Win32 hook owner per process**: whichever ASI is initialized first acquires the named hook-owner mutex and installs DXGI Present/ResizeBuffers plus the game-window message interception. The second never patches those functions; it registers independent module-local **render**, **native tick/action**, **raw mouse**, and **F11 return** callbacks using the exported C ABI `FHJ_ModuleBridge*` functions. Registration is published to the WndProc only after it finishes.
- Both ASIs may now be installed simultaneously, or either one may be installed alone. When both are present, **F8 opens both panels**; they share the same host cursor/window input. Runtime coexistence still requires a live game test on the user's DX11/DX12 path; a passing CI build is insufficient to guarantee interoperability.
- This is a different *integration mechanism* for each ASI — a primary DXGI hook versus secondary hook-owner callbacks — **not** two independent patches of the same graphics function. This avoids attempting to hook the same address twice.
- Do not install an extra renamed DLL copy of either ASI.

## Custom game-mode choice and main-menu indicator

- The main-menu-compatible DXGI ImGui overlay visibly says **CRYPTID HUNT | MOD LOADED**. **F8** opens the custom-mode panel even before a hunt starts. This is a native overlay, **not** a modified cooked `MainMenu` UMG Blueprint or a newly registered `AGameMode` asset.
- The player explicitly selects **NORMAL GAME** or **CUSTOM GAME MODE: CRYPTID HUNT**. The choice is a process-local atomic flag, **OFF by default** after each game launch.
- Until selected, Cryptid Hunt possession, special abilities, daytime attack restrictions, and Wendigo surveillance actions are inactive.
- The plugin **does not call SaveGame/SaveLoad APIs, modify lobby SelectedContract, redirect maps, or overwrite save slots**. This protects against *plugin-induced* save modification, but **does not suppress normal Folklore Hunter autosaving, unlocks, achievements or platform/cloud saves**. Full progress isolation needs a separate tested save/profile implementation; it is not claimed in v0.2.

## Same stock lobby; map determines cryptid

Source: the user's exact Dumper-7 UE5.5.4 SDK and the three map-loaded object dumps.

- `GameState_Lobby_C` derives from `FL_Gamestate_C`, and has replicated `SelectedContract` at **0x388**. `FF_Contract` begins with the selected `HuntDataBase` UClass pointer.
- `FL_Gamestate_C.HuntData` is at **0x340** for active hunts. `FL_Gamestate_C.IsNight` at **0x33C** is replicated; prefer that for host/client day/night status. `FL_Gamemode_C.Director` at 0x348 and `Director_C.IsNight` at 0xB0 remain host-only fallbacks.
- **Night of the Wendigo** -> `BP_Wendigo_C` (Wendigo, ground).
- **Bloodfang Forest** -> `BP_Vampire_C` (Vampire, ground).
- **Operation Blackwing / Hunt_Mothman** -> `BP_FlyingPawnTest_C` (Mothman, flight).
- Live creature selection in the mode UI is filtered to the selected map's species. Native host possession rejects off-map cryptids. Other unknown contracts are marked **UNSUPPORTED** rather than guessing.
- The host still starts/joins the game using the **original lobby** and its built-in map selector. This prototype does not create a real Unreal game-mode Blueprint override or replace the existing in-game contract navigation.

## Day and night rules

- Wendigo: no attack or other offensive Q/E/R abilities in daytime; daytime vision moderately impaired. Night returns full vision/attacks.
- Vampire: no LMB melee or Q offensive scream in daytime; day vision substantially impaired. Non-offensive invisibility/stance controls remain. Night returns full vision/attacks.
- All attacks are gated within the mode's native ability dispatcher; other players retain normal game input. The day/night visibility veil affects only the controlling client's render overlay.
- No gameplay restriction is asserted when the native day/night state is unavailable.

## Wendigo surveillance and placement invisibility

- **C** places one Wendigo-private `CameraActor` watchpoint; **V** cycles the watchpoint list and the chase view. Existing survivor `Camera_C` and `CameraFeed_C` references are never used.
- At most **two successful new placements per game day** and **four currently active watchpoints**. Placement allowance resets on replicated **night -> day** transition. It resets on a newly loaded world as well.
- During each successful placement, native `Actor.SetActorHiddenInGame(true)` hides the Wendigo for approximately **1.6 seconds**. Its prior replicated `AActor.bHidden` state (+0x58 bit 7) is saved, restored after the short window, and network state requested to update. The restore path validates UObject identity and current UWorld to avoid changing unrelated actors after travel. F11/possession exit also restores immediately.
- Cameras are **view-only native CameraActor nodes** in v0.2, not visible placed physical props. Check actual replication and remote survival visibility in a private two-client test.

## Remaining development gates

1. Verify actual game main-menu loaded indicator and opt-in with the owner's active DX11/DX12 render path.
2. Test both ASIs loaded simultaneously: exactly one DXGI patch owner, second callbacks rendered, both F8 panels, F11 restore when the mode is a companion.
3. Verify map classification in stock lobby as each contract is selected, and after loading each story map.
4. Validate Wendigo cameras in front of rocks/obstacles, stealth restoration as seen by an unmodded survivor, two-per-day reset at actual dawn and no actions when mode OFF.
5. Implement actual multiplayer role handoff: host-selected player needs a game-authoritative replicated role assignment, client input/camera possession bridge, round scoring, death/respawn and end-of-round. The current random draw is only host UI state.
6. Implement isolated profile/save handling separately if normal game autosaves must be prevented. v0.2 only avoids calling SaveGame from the ASI itself.

No in-game or multiplayer tests were performed by this build pipeline.
