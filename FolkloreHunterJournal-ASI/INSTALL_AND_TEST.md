# Folklore Hunter — The Hunter's Journal v0.5

Windows x64 ASI / UE 5.5.4 SDK snapshot build.

## Updating the working installation

Keep the original Ultimate ASI Loader; do not install a second proxy.

1. Exit Folklore Hunter completely.
2. Navigate to the directory holding your existing Hunter Journal ASI:
   FolkloreHunter\FolkloreHunter\Binaries\Win64\plugins
3. Move FolkloreHunterJournal-AllInOne.asi and any other older Hunter Journal
   .asi copies OUT of the plugins folder (back them up).
4. Copy only this new FolkloreHunterJournal.asi into that folder.
5. Start Folklore Hunter normally.

Never install two names for the same .asi; both may be loaded simultaneously.
The .dll copy in the artifact is the same binary, for inspection/reference only.

## Controls

F8: Toggle themed journal and release/reacquire the mouse cursor.
F9: Toggle the small integrated diagnostic log (ON by default).
F10: Toggle the separate native Windows console; it may stay hidden when
     the in-game log is visible.
Under OPTIONS, the two console settings are also checkbox-controlled.

The journal is placed on the right. A compact diagnostic console occupies the
upper left. Layout follows the game's window size; no blue system-style ImGui
title bars remain.

## Gameplay feature testing

CRYPIDS: Enable "real-time ESP" (off by default).
It discovers player and cryptid actors via the UE object array and
draws camera-projected markers. Discovery is bounded and cached,
positions refreshed at most every 110 ms, max 180 markers.
This is an experimental estimate based on RootComponent.RelativeLocation;
attached actors may need a ComponentToWorld-based correction.

SUPPLIES: If you are an authoritative host (including solo),
enter an EXACT internal item identifier and quantity, then Give Item.
The SDK confirms FL_Gamemode_C.GiveItem(FString,int32) exists. We still need
an actual game test to establish recognized item identifiers and effects.

HUNTING PARTY: Host Time, TimeScale, AddXP, AddSilver, InfiniteDamage.
SURVIVOR: Native ToggleGodMode. Both are authority-gated.
Client-only sessions do not have the server's GameMode instance.
"ProcessEvent dispatched" in a log is NOT proof that the gameplay effect
was accepted by the server or replicated to other players.

Player kicks/bans, vanity and additional client RPC controls are not enabled
without verified parameter contracts and runtime authority testing.

## Crash recovery

%LOCALAPPDATA%\FolkloreHunterJournal\journal.ini

[Journal]
SafeMode=0
DebugOverlay=1
Console=1

[ESP]
Enabled=0
Cryptids=1
Players=1
MaxMeters=350

If you see a fatal error, set SafeMode=1 to disable graphics hooks,
restore the older ASI if needed, and send journal.log/crash-trace.log.
First-chance exceptions may be handled by the game and not fatal.
The Windows x64 build includes .pdb/.map symbol files for stack analysis.

The SDK comes from the user-provided Dumper-7 5.5.4 dump.
Runtime compatibility and multiplayer effects cannot be verified by CI alone.
