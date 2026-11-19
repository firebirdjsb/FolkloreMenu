# Folklore Cryptid Hunt — separate native ASI, prototype v0.1

**Game:** Folklore Hunter 5.5.4 (Windows x64, original Dumper-7 SDK).
**Module:** `FolkloreCryptidHunt.asi` (separate ASI target compiled with `FCH_STANDALONE=1`).
**Previous module:** `FolkloreHunterJournal.asi` — retains admin/ESP and host spawning but no longer exposes possession/creature-ability controls in its CRYPTIDS page.

## Installation (important)

1. Remove **all previous versions** of `FolkloreHunterJournal.asi` and `FolkloreCryptidHunt.asi` from the game plugins directory.
2. For Cryptid Hunt gameplay, install only `FolkloreCryptidHunt.asi`. For the admin Journal, install only `FolkloreHunterJournal.asi`.
3. Do **not** install both simultaneously yet. Both targets currently reuse the native reflection/graphics hook bridge and would otherwise compete for a DXGI/Win32 hook. An exclusive named Windows mutex stops the second module from installing hooks, but you should use one mod per game run until a common dispatcher is developed.
4. F8 opens the active mod's UI, F9 diagnostics. F11 returns to the human character when playing as a cryptid.
5. Cryptid Hunt settings: `%LOCALAPPDATA%\FolkloreCryptidHunt\cryptid-hunt.ini`. Diagnostic logs currently use the common JournalDiagnostics directory.

## Species rules

| Cryptid | Day | Night |
|---|---|---|
| Wendigo | Mild local vision glare; may still move and attack; private camera watchpoints | Full vision, cameras available |
| Vampire | Heavy local sunlight glare; **LMB melee and Q scream blocked** | Full vision; native melee/scream |
| Mothman | Native FlightTarget flying/AnimBlueprint controls; no verified basic melee; R = DarknessAttack | Same species controls |

The sunlight phase is from **`FL_Gamemode_C::Director` at +0x348** and **`Director_C::IsNight` at +0xB0**. Both originate from the uploaded 5.5.4 SDK and live map observations. The Director's `IsNight` field isn't declared replicated. Therefore **only the authoritative host** can enforce these rules with this implementation; a remote client without that component reports **UNKNOWN** and gets no fabricated daytime penalties. A network-safe PvP game mode needs host-to-client day-state distribution and client-side input enforcement.

Daylight glare is an ImGui screen overlay, scoped only to the controlling local player. It doesn't modify the game's lighting for survivors or other players.

## Wendigo watch cameras

- **C:** deploy one private, native UE `CameraActor` watchpoint (up to four).
- **V:** cycle deployed watch cameras, ending at Wendigo's chase camera.
- UI button: Return chase view.
- Watchpoints are created only while possessing the live `BP_Wendigo_C` on the host. Camera IDs are tracked, verified and destroyed when possession ends or are cleared on world travel.
- These watchpoints deliberately do **not** use `Camera_C`, `PlayerCharacter_C.CurrentCamera` or `CameraFeed_C`, so they do not hijack survivor equipment and cannot view survivor-placed camera feeds.
- **Limit:** v0.1 cameras are invisible world-fixed view nodes, not fully animated visible physical surveillance props. Camera placement, field of view and watch camera handling require live testing.

## Prototype host round lobby

- Connected live players are discovered via the SDK-derived player roster.
- Host marks eligible participants in its local UI; a `std::mt19937_64` draw picks one of those marked players uniformly.
- The local host can manually possess an existing, validated live monster for gameplay testing.
- **Not implemented:** networked opt-in/ready messages, sending the selected player's new role over replicated game state, transferring a remote player's input/camera to a cryptid, authoritative round start/end announcements, survivor victory logic, round balancing, visible cryptid camera props. The host-side draw is a **prototype**, not a working multiplayer random-role game.

## Refactoring and future ownership

Stage 1 in this branch creates an **independent ASI target and independent game-mode UI**, removes the moved controls from the admin menu, scopes the new gameplay rules to `FCH_STANDALONE`, and stages CI artifacts separately. The underlying UE reflection bridge and native engine file `HunterNative.cpp` are still **shared at compile time** by both variants. They are not yet physically extracted into a stand-alone source library and hooking layer.

Next code work must isolate `CryptidControl`, `CryptidRuleEngine`, and `WendigoWatchCamera` into separate source units and expose a shared hook/render dispatch for simultaneous independent installation, then implement host-authoritative networked role messages backed by a tested client companion.

## Verification

The Windows Actions build validates the ABI/x64 PE output and the two ASI targets. It is not proof of in-game species animation, camera placement, multiplayer replication or round handoff. Test in authorized private sessions.
