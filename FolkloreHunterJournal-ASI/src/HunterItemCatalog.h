#pragma once
// Verified item BlueprintGeneratedClasses from user's Folklore Hunter UE5.5.4 SDK.
// Each is a concrete Item_* subclass; Item_VanityCosmeticBase is a base class and omitted.
// The SDK identifies classes, not guaranteed GiveItem lookup strings or item entitlement.
#include <array>
namespace HunterItems {
    struct Item {
        const char* label;
        const char* className;
        const char* category;
    };
    inline constexpr std::array<Item,19> Catalog{{
        {"Bandage", "Item_Bandage_C", "Survival"},
        {"Battery", "Item_Battery_C", "Equipment"},
        {"Bullets", "Item_Bullets_C", "Ammunition"},
        {"Camera", "Item_Camera_C", "Equipment"},
        {"Crossbow Bolt", "Item_CrossbowBolt_C", "Ammunition"},
        {"Crossbow", "Item_Crossbow_C", "Weapons"},
        {"Crucifix", "Item_Crucifix_C", "Survival"},
        {"Deer Skull Cosmetic", "Item_DeerSkullCosmetic_C", "Cosmetics"},
        {"Fire Bullets", "Item_FireBullets_C", "Ammunition"},
        {"Garlic", "Item_Garlic_C", "Survival"},
        {"Hand Radio", "Item_HandRadio_C", "Equipment"},
        {"Jerrycan", "Item_Jerrycan_C", "Equipment"},
        {"Magnum Bullets", "Item_MagnumBullets_C", "Ammunition"},
        {"Magnum", "Item_Magnum_C", "Weapons"},
        {"Meat", "Item_Meat_C", "Survival"},
        {"Rifle", "Item_Rifle_C", "Weapons"},
        {"Shotgun Shells", "Item_ShotgunShells_C", "Ammunition"},
        {"Shotgun", "Item_Shotgun_C", "Weapons"},
        {"Trap", "Item_Trap_C", "Equipment"}
    }};
}