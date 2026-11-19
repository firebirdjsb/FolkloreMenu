# Folklore Hunter — Dumper-7 per-map captures

These captures help differentiate an unloaded cryptid class from its missing cooked skeletal mesh, animation Blueprint, AI controller, or map-specific initialization.

Capture each story hunt separately: Night of the Wendigo, Bloodfang Forest, and Operation: Blackwing. Run in a solo or private host session using the same game build and Dumper-7 version.

1. Enter the hunt and allow the normal creature and environment to finish loading.
2. Dump all available SDK data inside the hunt (not in the menu). Keep GObjects-Dump.txt, GObjects-Dump-WithProperties.txt, Dumpspace/*.json, CppSDK, and .usmap/.idmap if generated.
3. Attempt to spawn a different cryptid. If it has no model or AI, collect a second complete dump in the same hunt, and include %LOCALAPPDATA%/FolkloreHunterJournal/journal.log.
4. Label the before and after folders with map, species, game version, host/client, and observed result. ZIP each map separately.
5. Remove private player names, lobby codes, private server addresses, and account identifiers before sharing.

Compare BP_FlyingPawnTest_C (Mothman), BP_AIController_Mothman_C, its Niagara Smoke component, skeletal mesh and wing AnimBlueprint, and other cryptids with their respective AIControllerClass, mesh and AnimClass/AnimScriptInstance. Inspect BP_MobSpawner_C MobClass and spawning rules.

SDK metadata does NOT include Blueprint graph implementations or actual cooked meshes/animations; missing classes on one map may require a supported cooked asset-load path. Never claim an incomplete model-less actor is a valid spawn.

Current Journal changes: Mothman red ESP; native FlightTarget smoothing and wing animation Blueprint speed channels; on the controlling host only, the nonreplicated SceneComponent HiddenInGame flag hides Mothman's Smoke while Eyes, Moths, wings, sounds and remote players remain unchanged. Restore original local state on returning to human. These runtime effects require in-game validation.