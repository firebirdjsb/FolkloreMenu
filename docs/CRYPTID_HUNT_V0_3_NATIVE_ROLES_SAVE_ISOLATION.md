# Cryptid Hunt v0.3 development — native remote role handoff and isolated-save runner

Status: **EXPERIMENTAL / NOT GAMEPLAY-VERIFIED**. Built on the v0.2 separate-ASIs bridge. CI compiles Windows x64 and exercises the PowerShell save-profile helper; only an actual two-PC lobby can establish gameplay correctness.

## Networked role handoff (host to remote owner)

The existing random draw is still **host-local**. It now has a separate explicit action:
1. Both players install FolkloreCryptidHunt.asi and explicitly select CUSTOM GAME MODE in F8. This opt-in is in process memory, OFF on next launch.
2. Host joins a normal stock hunt through its normal lobby and map selector, checks the remote player's eligibility, then draws a random player.
3. Host selects a **live matching map cryptid**. The new handoff control is available only when the winner is remote; the host must confirm it has the remote player's permission and that their ASI is active. No network ready/consent message exists yet.
4. Host asks native UE5.5 Controller.Possess(APawn*) to attach that remote player's **authoritative PlayerController** to the selected cryptid. The implementation verifies the UFunction has an 8-byte APawn argument and checks the new controller/pawn pointers immediately on the server. Unreal's existing pawn/controller replication is responsible for informing the owning client.
5. The owning client detects that its *own* PlayerController has actually been assigned a map-native cryptid. It starts guarded WASD AddMovementInput for ground ACharacter cryptids, native Jump on Space and sends no-parameter Server_Attack on mouse press **only if reflected UFunction flags prove an owned Server RPC**.
6. Host can select **RESTORE REMOTE HUMAN**. The source world, UObject identities, original human pawn, PlayerController, and previous AIController are tracked. Controller.Possess is used again to return the player to the original human; if still available and unowned, the original AIController is asked to repossess the cryptid. Native net updates are requested. A dead/invalid cryptid triggers an attempted host restore. The function refuses to overwrite a game-created replacement pawn.

### Important limits

- **No new custom game RPC, no ready/consent protocol, no verified round scoring or replicated round state.** The draw alone does not grant the role. Server-side native possession is a different explicit action.
- The native role swap can trigger Folklore Hunter-specific Blueprint ReceiveUnpossessed/respawn/death logic. The old human may disappear or be destroyed. Recovery is only possible while original controller/human remain live. Test in a private session, not on a valued save.
- A remote user's F11 **pauses their own input**. It cannot issue a host-authoritative restoration RPC. The host must restore the human.
- Mothman is BP_FlyingPawnTest_C : APawn, not BP_Cryptid_Base_C/ACharacter. No CharacterMovement input, no invented 0x710 HP, no speculative remote FlightTarget writes. **Remote Mothman flight is blocked pending a verified network movement RPC.**
- Vampire/Wendigo ground input and Server_Attack are implemented as reflection-gated calls, but their live movement replication, animations, attack/damage and camera target are **not verified**. The remote Q/E/R skills are not transported; they need owned RPC evidence and matching remote/host behavior.
- Local host puppet mode continues to keep the human PlayerController on the human pawn; the networked remote handoff instead uses actual controller possession. Do not conflate the two control paths.
- Current actions are sent from the existing ASI message-thread bridge. Verify this execution thread is safe for the live UE world before declaring production multiplayer support.

## SaveGames profile isolation — optional launcher

File: tools/Start-CryptidHunt-Isolated.ps1

The ASI itself makes no SaveGame calls and makes **no** save isolation guarantee. To keep local default saves separate during custom mode, quit the game and disable Steam cloud synchronization for this title before use. Identify the **real** directory (normally the Unreal project's Saved\SaveGames folder, but do not guess if your install uses a different one).

From PowerShell (replace both example paths with the actual paths on the PC):

    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Start-CryptidHunt-Isolated.ps1 -GameExe 'C:\Path\To\FolkloreHunter-Win64-Shipping.exe' -SaveGameDirectory "$env:LOCALAPPDATA\FolkloreHunter\Saved\SaveGames" -DryRun

After checking the printed paths, run again without -DryRun. The script:
- refuses to operate if the selected executable or paths are unexpected, a game process is already running, a prior backup exists, or SaveGames is already a link;
- writes an interruption-recovery marker, moves existing SaveGames to SaveGames.CryptidHuntOriginal, creates a junction into CryptidHuntProfiles\CryptidHunt, then launches the shipping executable;
- restores the original SaveGames on normal exit; retains the separate mode profile across sessions;
- on an interrupted run, allows explicit recovery **after the game has exited**:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Start-CryptidHunt-Isolated.ps1 -SaveGameDirectory "$env:LOCALAPPDATA\FolkloreHunter\Saved\SaveGames" -Recover

The runner will not overwrite a conflicting directory or silently remove a junction pointing somewhere else. If it reports a conflict, preserve both folders and inspect them before manual recovery. It does **not** isolate Steam/cloud synchronization, platform achievements, multiplayer progression, or files outside the chosen SaveGames directory; it does not patch Folklore Hunter's cooked UMG/main menu.

CI runs tools/Test-CryptidHunt-Isolation.ps1: PowerShell parse, dry-run read-only behavior, a fake native process run using a renamed Windows cmd.exe, rollback of the real save folder and preservation of the mode profile. These smoke tests are not a substitute for an actual game save inspection.

## Private two-client test matrix

| Check | Expected evidence | State |
|---|---|---|
| Both ASIs coexist | One DXGI owner, both F8 panels and correct F11 behavior | Pending live test |
| Server chooses a remote winner | Host UI selects verified live PlayerCharacter_C | UI draft only |
| Host native role swap | Host log shows Controller.Possess and live PC.Pawn check; remote client's own pawn becomes matching cryptid | Pending live test |
| Remote ground cryptid WASD/Space | Native movement and animation seen by host and other client | Pending live test |
| Remote attack | Reflected Server_Attack has 0 bytes and FUNC_Net/FUNC_NetServer; other player witnesses actual damage/animation | Pending live test |
| Camera does not clip | Client camera collision against rocks/walls; restore returns to human | Pending live test |
| Remote Mothman flight, Q/E/R | Owned RPCs and game-specific movement/control verified | **Not yet implemented** |
| Respawn and host restore | Remote returns to exact human or correctly handles native replacement; cryptid AI resumes | Pending live test |
| Save isolation | Local baseline SaveGames file hash unchanged after the isolated session; mode save changes only under CryptidHuntProfiles | Windows smoke passed; in-game pending |
| Other progression | Verify Steam cloud, achievements and external profile saves separately | Not isolated |

## Logs and release gate

Host/client: %LOCALAPPDATA%\FolkloreCryptidHunt\journal.log (check actual generated logging path if different). Look for CRYPTID NETWORK ROLE and CRYPTID CLIENT messages. Keep status as **development candidate** until both Windows x64 ASIs compile AND these runtime checks are completed. Native possession is not a guarantee that the Folklore Hunter-specific game Blueprint supports a custom PvP role.
