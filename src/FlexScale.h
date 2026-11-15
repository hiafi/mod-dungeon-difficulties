/*
 * mod-dungeon-difficulties: the flex 5-10 scaler, public API for boss scripts (Blackrock Spire build handoff §3.12;
 * spec rulings 6-14 and 20). Every module's src dir is a public include of the modules target, so a module includes
 * this header by its bare name. mod-dungeon-bosses wraps it in its kit (DungeonKit::Flex); boss scripts call the kit.
 *
 * The rules (SYS section 3, "Flex dungeons"):
 * - The player count is read at each pull and held for the fight: players on the map, game masters excluded, bots
 *   included (they are players), pets and guardians never; clamped to Flex.MinPlayers..Flex.MaxPlayers (5..10).
 * - Health x (1 + 0.25 x (N - 5)): 1.00, 1.25, 1.50, 1.75, 2.00, 2.25 for 5..10. Boss-kit adds and trash the same.
 * - Damage never scales (nothing here touches damage).
 * - Mechanic target counts: the normal count at 5, the doubled ("big") count at 6 or more.
 * - Loot: max(1, N / 5) items (2 at 10). Taunt swaps only at 10.
 *
 * The engine side (FlexScale.cpp) holds, at its own pull, every hostile creature on a map listed in
 * DungeonDifficulties.Flex.Maps: trash, stock bosses and kit adds. A hold stores the count and the creature's base
 * (5-player) max health and scales max health to base x ladder, keeping the health percent. Evade restores the base
 * and drops the hold (the next pull counts again); death drops the hold. Scaling never stacks across pulls.
 *
 * Which count a pull holds, first match wins:
 *   1. a summon whose summoner is held (or engaged: the summoner is held first) takes the summoner's count;
 *   2. while a held dungeon boss of the map is engaged ("the encounter"), every pull takes the encounter's count;
 *   3. the GM test override (`.dd flex set <n>`), if set on this instance;
 *   4. a fresh count of the players on the map.
 *
 * Hook order (verified in core, Creature::AtEngage): the AI's JustEngagedWith runs BEFORE the
 * UnitScript::OnUnitEnterCombat hook. Both paths go through PullCount, which holds at the first caller, so a boss
 * script that calls PullCount (or SetScaledMaxHealth) in JustEngagedWith and the hook that follows read one number.
 *
 * All functions are safe on any map and with nullptr. Off a flex map (or with Flex.Enable = 0) nothing is held:
 * PullCount returns Flex.MinPlayers (5), SetScaledMaxHealth sets the base health unscaled, HeldCount returns 0.
 * The count helpers clamp their input, so 0 ("not held") reads as 5.
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_FLEX_SCALE_H
#define MOD_DUNGEON_DIFFICULTIES_FLEX_SCALE_H

#include "Define.h"

class Creature;
class Map;

namespace DungeonDifficulties::Flex
{
    // The map is listed in DungeonDifficulties.Flex.Maps and Flex.Enable is on.
    bool IsFlexMap(Map const* map);

    // The count held by this creature for its current fight, or 0 when it is not held.
    uint8 HeldCount(Creature const* creature);

    // The held count; holds it now (count, base health, health scaled) when the creature is engaged and not yet
    // held. Call it from JustEngagedWith or later. On a creature that is not engaged it returns the count a pull
    // would hold now without holding anything (a preview). Off a flex map: Flex.MinPlayers.
    uint8 PullCount(Creature* creature);

    // The creature's base (5-player) max health as the scaler stores it, 0 if it was never held or set here.
    uint32 BaseHealth(Creature const* creature);

    // Script-set health (hard modes, late arrivals, summoned bosses): `base` is the 5-player value. Held: max health
    // becomes base x ladder(held count) now; not held: max health becomes base and the next pull scales it. Off a
    // flex map: base, unscaled. Keeps the health percent (call SetFullHealth after it for a fresh start). Use this
    // instead of SetCreateHealth / SetMaxHealth on a flex map, or the next pull scales from the wrong base.
    void SetScaledMaxHealth(Creature* creature, uint32 base);

    // The pure rules with the configured clamp and step (FlexMath.h has the host-tested versions).
    uint8 ClampCount(uint32 players);                       // to Flex.MinPlayers..Flex.MaxPlayers
    float HealthLadder(uint8 count);                        // 1.00 at 5 .. 2.25 at 10
    uint8 TargetCount(uint8 count, uint8 normal, uint8 big); // `normal` at 5 or fewer, `big` at 6 or more
    uint8 LootRolls(uint8 count);                           // max(1, count / 5): 1, or 2 at 10
    bool TankSwap(uint8 count);                             // count >= 10 (after the clamp)
}

#endif
