#pragma once

#include <array>
#include <cstdint>

#define M_PI 3.1415926535897931

// Hardcoded addresses for H1Z1 (found via Cheat Engine) - stale, kept only because
// FindSignature() in Lol2.cpp still uses ADDRESS_CGAME as a delta reference. Prefer
// moduleBase + RVA_* below for anything new.
#define ADDRESS_CGAME 0x14476c890
#define ADDRESS_CGRAPHICS 0x14476dd48

// Module-relative addresses (RVA, add to the module base) from a disassembly of this
// build. Verified against our own signature-scanned/delta-corrected results: both
// match RVA_MANAGER_SLOT and RVA_GRAPHICS_SLOT exactly.
#define RVA_MANAGER_SLOT 0x476d290       // -> cGame (same target as the signature scan)
#define RVA_GRAPHICS_SLOT 0x476e748      // -> cGraphics
#define RVA_WORLD_SLOT 0x46e1190
#define RVA_CLIENT_SLOT 0x476dc08
#define RVA_ITEM_DEF_MGR 0x476e418
#define RVA_INV_MGR 0x476df38
#define RVA_GAME_STATE_SLOT 0x5a3c2a0

// 2026-10-08 offset dump (H1Z1.exe base 0x140000000), superseding the 2026-10-05 one: cgame
// slot -> +game_manager is the object the entity list/local player offsets below hang off.
// The only value that moved since the 10-05 dump is cgame (was 0x476DC08, which equals
// RVA_CLIENT_SLOT above - no longer the game slot); every other dumped value matches this file.
#define RVA_CGAME_DUMP 0x4742DB8          // dump's "cgame"
#define RVA_UNLOCK_ALL 0x1129D80          // dump's "unlock_all" (code address) - unused
#define dumpGameManager 0x382A0           // cgame -> game manager
#define dumpPlayerHead 0xD8               // game manager -> head of player list (next = entityNext 0x748)
#define dumpEquipment 0xCE8               // entity -> equipment (entityWeaponContainer is still 0x688)

// Entity and game structure offsets
#define gameEntityCount 0x50
#define gameFirstEntity 0x780
#define gameLocalPlayer 0x58
#define graphicsCamera 0x48
#define cameraMatrixObject 0x30
#define matrixViewProjection 0x1b0
#define cameraPosition 0x40   // direct world-space camera position, distinct from the matrix chain above

// Entity properties
#define entityType 0x3f8
#define entityPosition 0x420
#define entityVelocity 0x430
#define entityRotation 0x440
#define entityPlayerBase 0x300
#define entityNext 0x748
#define entityName 0x7c0   // 0x7b8 (from the offset dump) produced garbage "@" names in testing - reverted
#define entityYaw 0x530
#define entityPitch 0x534
#define entityStance 0x954
#define charHealthBase 0x3dc8  // confirmed - matches the reference cheat's OFFSET_HEALTHBASE exactly
#define charHealthValue 0xb0   // health is stored as value/100 (percent * 100)
#define entityAnimFlags 0x3a50 // matches kCharAnimFlags - unused until now; entityStance
                                // alone didn't reliably correlate with crouch (same value
                                // gave both a dead-on hit and a 159px miss), so this is
                                // being tried as a second signal alongside it
#define playerIsAlive 0xe8  // off playerBase - int, 1=alive 0=dead. Cross-validated: a
                             // classmate's independently-sourced offset table names this
                             // exact field at this exact offset. Used to skip ESP
                             // rendering on corpses instead of relying on health>0, which
                             // can stay briefly stale during a death animation.

// Player specific
#define playerBasePosition 0x3a8
#define playerPelvisPosition 0x3f0

// Equipped-weapon identity chain - CONFIRMED via a classmate's live debugger captures
// against the running game (4 separate weapons: AR15, AK47, Magnum, RiotShotgun, all
// resolving cleanly): entity -> weapon container -> active weapon object -> a direct
// pointer to a null-terminated model/asset name string (e.g. "Weapons_AK47_3P.adr"),
// NOT a length-prefixed std::string like the earlier (wrong) attempts assumed. Replaces
// two previously-guessed and confirmed-broken chains (a "localPlayerInventory" shortcut
// and a "charWeaponMgr" numeric item-def lookup, both applied off playerBase and both
// dead ends - see git history if reviving either is ever needed).
#define entityWeaponContainer 0x688   // entity + this = pointer to the weapon container
#define weaponContainerActive 0xb8    // weaponContainer + this = pointer to the active weapon object
#define weaponModelNamePtr 0x20       // activeWeapon + this = pointer to a null-terminated C string

// Vehicle properties
#define vehicleOccupant 0x360
#define vehiclePosition 0x9d0

// Item properties
#define itemPosition 0x18c0

// Entity types
#define playerEntityType 270
#define lootbagEntityType 46  // death loot bag/backpack container - handled as its own
                               // category, not lumped in with regular ground items below
#define grenadeEntityType 174  // matches TYPE_Grenade (0xAE) from a public offset dump -
                                // was already in groundItemEntityTypes below (unfiltered,
                                // since IsArmorName rejected the name), pulled out to its
                                // own category so it can be shown/colored independently
#define weaponEntityType 52    // matches TYPE_WEAPON (0x34) from a classmate's cross-
                                // referenced offset table - was already in
                                // groundItemEntityTypes below (also unfiltered, same
                                // reason as grenades: IsArmorName only recognizes
                                // armor/vest/kevlar), pulled out to its own category
static const int vehicleEntityTypes[] = { 17, 114, 118, 204, 451 };  // 451 = TYPE_LAMBORGHINI,
                                                                        // no dedicated color
                                                                        // case yet - falls
                                                                        // back to white
static const int groundItemEntityTypes[] = { 21, 44, 178 };

// Skeleton bone chain (entity -> actor -> skeleton -> pose -> joint array). Tried and
// scrambled on the external build with these same indices at stride 0x30 - a classmate's
// working version uses the identical pointer chain and indices but stride 0x10 (16
// bytes/bone, not 48). At 0x30 we were reading roughly every 3rd real bone slot instead
// of consecutive ones (coherent-looking memory, wrong bone), and running past the true
// ~114-entry array's actual end hundreds of bytes early, into freed memory.
#define skeletonActors 0x5c0
#define skeletonStarts 0x250
#define skeletonInfos 0x50
#define skeletonBoneInfo 0x28
#define skeletonBoneStride 0x10

static const size_t kHeadBoneIdx = 26;
static const size_t kNeckBoneIdx = 24;
static const size_t kChestBoneIdx = 23;
static const size_t kSpineBoneIdx = 22;
static const size_t kPelvisBoneIdx = 12;
static const size_t kLShoulderIdx = 52;
static const size_t kLElbowBoneIdx = 54;
static const size_t kLHandIdx = 55;
static const size_t kRShoulderIdx = 76;
static const size_t kRElbowBoneIdx = 78;
static const size_t kRHandIdx = 79;
static const size_t kLHipIdx = 13;
static const size_t kLKneeBoneIdx = 14;
static const size_t kLFootBoneIdx = 16;
static const size_t kRHipIdx = 110;
static const size_t kRKneeBoneIdx = 111;
static const size_t kRFootBoneIdx = 113;
