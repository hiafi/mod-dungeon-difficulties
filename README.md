# mod-dungeon-difficulties

Dungeon difficulties for AzerothCore (WotLK 3.3.5a) on this server: vanilla dungeons get a level 60 heroic, and later
(stage E4) empowered heroic levels. Design: `.agents/plans/dungeon-difficulties/` in the core repository (PLAN,
STAGE-E0-E3-HANDOFF §3.6, the WP-H report).

Status: stage E2, the heroic creature spell scaling hook (live). The flex 5-10 scaler (Blackrock Spire build,
handoff §3.12): written and host-tested, not yet built or playtested.

No core edits: existing `UnitScript` / `AllCreatureScript` hooks only. Heroic templates, MapDifficulty rows and spawn
masks come from dbc-tools; this module scales the stock spells those creatures cast and, on flex maps, creature health
by the player count held at each pull.

## Layout

| File | What |
| --- | --- |
| `src/HeroicSpellMath.h` | Pure rule: effect roles, CalcValue mean, group factor, SpellDifficulty index, gate |
| `src/ReferenceHealth.h` | Pure keyed reference health rows (copied from mod-boss-mechanics) |
| `src/HeroicSpellScaling.cpp` | `UnitScript` hooks, engine inputs, startup data (SpellDifficulty, potency rows) |
| `src/DungeonDifficultiesConfig.cpp` | Config load (startup and `.reload config`) |
| `src/FlexMath.h` | Pure flex rules: clamp, ladder, target counts, loot rolls, tank swap, count sources, hold state |
| `src/FlexScale.h` | Flex API for boss scripts (other modules include it by name) |
| `src/FlexScale.cpp` | Flex engine side: pull, evade, death, removal and update hooks, holds, `.dd flex` views |
| `src/DungeonDifficultiesCommand.cpp` | `.dd status`, `.dd factor`, `.dd flex` |
| `tests/` | Host tests: `bash tests/run.sh` (`CXX=clang++` for clang) |

## The rule

A stock spell cast on a listed heroic dungeon should take the same share of a player's health as the creature's
normal version takes on normal. The hook acts when all of these hold:

- the caster is a creature that no player controls (pets, charmed mobs and players are skipped);
- its map is a dungeon listed in `DungeonDifficulties.Heroic.Maps` and `Map::IsHeroic()`;
- the caster runs a heroic difficulty template (current template entry differs from its normal entry), unless
  `DungeonDifficulties.Heroic.RequireHeroicTemplate = 0`: a creature the heroic generator missed keeps stock spells to
  match its stock melee;
- the spell id is below 200000 (dbc-tools custom spells and the boss mechanics resolve spells 205001-205007 carry
  their own numbers and stay flat) and is not in `DungeonDifficulties.Heroic.SpellExceptions`;
- damage: the target is player controlled (players, their pets and guardians, bots); heal: the target is a creature no
  player controls.

It multiplies the landed amount (before armor, crit and absorbs) by

    F = wanted / actual

- `Fref(L)` = reference non-tank health at the heroic row for the caster's (heroic) level / at the normal row for its
  normal level L (the normal template, `sObjectMgr->GetCreatureTemplate(GetEntry())`, averaged over its level range).
- `actual` = what the engine computed at the heroic creature: the expected `SpellEffectInfo::CalcValue` of every effect
  the hook carries (points per level up to `MaxLevel`, the `BaseDamage` ratio of `SCALES_WITH_CREATURE_LEVEL` spells,
  the potency correction of the few player spells creatures cast, the die mean).
- `wanted` = the same sum where each scaled effect counts `Fref(L)` x its mean at the normal level instead.

For a single scaled effect this is `Fref / G`, G being the engine's own growth from the normal level to the heroic one.

| Effect | In the hook | Treatment |
| --- | --- | --- |
| School damage, health leech, proc-trigger damage aura | `ModifySpellDamageTaken` | scaled |
| Periodic damage, periodic leech aura | `ModifyPeriodicDamageAurasTick` | scaled |
| Weapon damage (all four weapon effects) | `ModifySpellDamageTaken` | unchanged (the template's melee scales) |
| Periodic damage percent, power burn | either | unchanged |
| Heal, periodic heal (creature on creature) | `ModifyHealReceived` | x target heroic / normal max health |
| Heal percent, heal max health, obs mod health | `ModifyHealReceived` | unchanged |

A spell that mixes types in one hook (school + weapon damage, two periodic auras) gets the weighted blend, with the
unchanged parts weighted by their own expected amount (percent x target max health; for weapon effects the caster's
per-hit weapon damage as the engine rolls it, `Unit::CalculateDamage`'s min / max with AP, `DamageModifier` and swing,
not the bare base weapon). The periodic heal tick also calls the periodic damage hook; a spell with a heal aura and no
damage aura is skipped there. Leech heals (no heal effect) are never scaled again. `ModifyHealReceived` arrives as
(healer, target) from `Unit::HealBySpell` and as (target, healer) from the periodic tick: the periodic damage hook
leaves a thread-local marker (units, spell and the amount variable) for heal spells that tells them apart, and every
damage hook call clears it first.

A damage factor is capped at 20 against a degenerate spell (an expected amount near zero). Heals are not capped: a
creature heal on a creature keeps its share of the target's max health, so Cookie's Cooking (120 on a 2,904 health
Cookie) heals about 13,600 on his 330,000 health heroic template.

Not modelled: `spell_bonus_data` `ap_bonus` / `ap_dot_bonus`. `Unit::SpellDamageBonusDone` adds the caster's attack
power times the coefficient, and that share is multiplied by F too. No Deadmines spell has one; check at each rollout.

### SpellDifficulty swaps

On a heroic dungeon the engine casts a spell's `SpellDifficulty` heroic id instead (`Spell::Spell` ->
`SpellMgr::GetSpellForDifficultyFromSpell`). 20 `spelldifficulty_dbc` overlay rows (and 2 stock `SpellDifficulty.dbc`
rows, Onyxia and Blackwing Lair spells) swap spells that creatures cast in vanilla instances to TBC/WotLK heroic
versions, e.g. Lightning Bolt 12167 (a level 20 scaling bolt) to 59863 (flat 3825-5175). The hook rescales those
against the original normal spell: wanted reads the normal spell's effects, actual the swapped one's, so heroic
Lightning Bolt lands at `Fref` x the normal bolt (F about 0.1) instead of 4,500 on a 4,120 health player. Excluding the
swapped ids would have left those TBC/WotLK numbers in place.

The index replicates the engine's searcher, so it knows which normal spell swaps to an id. A heroic-slot id is always
read as a swap. Six of them are also cast as themselves by vanilla creatures (they stay themselves on heroic) and are
then rescaled against the lower rank: 15232 (from 12471), 15305 (15659), 15472 (12739), 15530 (15043), 15582 (15657),
15616 (15039). None is in Deadmines; settle them per dungeon when one of their maps gets a heroic.

Blackrock Depths (230) casts five of them as themselves: Hate'rel and Dark Keeper Uggel 15232, Golem Lord Argelmach
15305, Anub'shiah 15472, Pyromancer Loregrain 15616 and Dope'rel 15582; its reworked bosses use copies in their own
spell slice instead (trash keeps the stock ids), and its swap casters (the Shadowforge Senator 12471, Plugger 12739,
the Twilight Emissary 15043) take the heroic version.

Scarlet Monastery (189, a 5-player heroic row, not flex) is in the conf dist's list. No creature spawned on the map
casts one of the six double-use ids or their normal-side swap sources through a SmartAI cast action or
`creature_template_spell` (read-only SELECTs 2026-11-20; action lists and C++ casts not read); its reworked bosses use
spells of their own slice (212300-213499).

Ragefire Chasm (389, a 5-player heroic row, not flex) is in the conf dist's list. No creature spawned on the map (or
summoned by it: Voidwalker Minion 8996) casts one of the six double-use ids or their normal-side swap sources, and none
of its stock casts (2818, 8242, 9532, 11970, 11976, 11986, 12746, 14873, 18070, 18072, 18266, 18267, 20791, 20800,
33914, 40505) has a `spelldifficulty_dbc` row (read-only SELECTs 2026-11-24; action lists and C++ casts not read); its
reworked bosses use spells of their own slice (213540-213799). Its level 13 to 16 casters read the level 20 row of
`ReferenceHealth` here (the lookup clamps below the first row), so a stock heroic spell lands about 15% lighter than
its normal share: BM's own ReferenceHealth has the level 16 row (`16:1100:1080`), this one deliberately does not (a row
at 16 would also move the level 17 to 19 stock heroic spells of Deadmines).

Zul'Farrak (209, a 5-player heroic row, not flex) is in the conf dist's list. Three stock casts of its creatures have a
`spelldifficulty_dbc` row that swaps on heroic (read-only SELECTs 2026-11-25): Zum'rah's Shadow Bolt 12739 (to 15472),
and the Sandfury Shadowcaster's 12471 (to 15232) and 14032 (to 15654). The reworked bosses cast copies from their own
slice (213840-214239), Zum'rah's bolt among them, so only the trash casts reach the swap. Its level 43 to 46 normal
casters read the rows of `ReferenceHealth` between 40 and 52 (interpolated).

## Config

| Key | Default | What |
| --- | --- | --- |
| `DungeonDifficulties.Enable` | `1` | the hook |
| `DungeonDifficulties.Heroic.Maps` | `36` (conf dist: `36 33 289 329 229 230 189 389 209`) | maps the heroic rule acts on |
| `DungeonDifficulties.ReferenceHealth` | `20:1300:1290 60:3670:3710 60h:4120:4110` | BM's `ReferenceHealth.Rows` without row 16 |
| `DungeonDifficulties.Heroic.SpellExceptions` | empty | stock spell ids left unscaled (cast or source id) |
| `DungeonDifficulties.Heroic.RequireHeroicTemplate` | `1` | scale only casters with a heroic template |
| `DungeonDifficulties.Debug` | `0` | log every scaled hit |
| `DungeonDifficulties.Flex.Enable` | `1` | the flex scaler |
| `DungeonDifficulties.Flex.Maps` | empty (conf dist: `229`) | flex maps, every difficulty |
| `DungeonDifficulties.Flex.MinPlayers` / `MaxPlayers` | `5` / `10` | the clamp on the counted players |
| `DungeonDifficulties.Flex.HealthStep` | `0.25` | health per player above 5 |
| `DungeonDifficulties.Flex.Debug` | `0` | log every hold and release (boss pulls are always logged) |

## Flex scaler

A flex dungeon (Blackrock Spire, map 229; spec rulings 6-14 and 20, shared doc section 3 "Flex dungeons") takes 5 to 10
players on every difficulty. AutoBalance must be off for the map (`AutoBalance.Disable.PerInstance`, comma separated,
in the live `AutoBalance.conf`; the module warns at startup when a flex map is missing there). AutoBalance stays for
everything else until raids move to this scaler.

| Rule | Value |
| --- | --- |
| Count | players on the map at the pull, game masters excluded, bots included, pets never; clamped to 5-10 |
| Health | base x (1 + 0.25 x (N - 5)): 1.00, 1.25, 1.50, 1.75, 2.00, 2.25 |
| Damage | never scales (DD's heroic spell factor and BM's reference health pins are untouched) |
| Targets | the normal count at 5, the doubled count at 6 or more |
| Loot | max(1, N / 5): 1, or 2 at 10 |
| Taunt swap | only at 10 |

**Who is held.** Every hostile creature on a flex map, at its own pull: trash, stock bosses, reworked bosses and their
adds. Hostile = its faction is hostile to players, or it engaged a player or something a player controls (a neutral
beast pulled by a player). Pets, guardians, charmed units, triggers, critters and friendly NPCs fighting mobs are not.

**Which count.** First match wins:

1. a summon whose summoner is held takes the summoner's count (an engaged summoner not yet held is held first);
2. while a held dungeon boss (`IsDungeonBoss`, or a world boss) is engaged, every pull takes that encounter's count
   (a second boss of the encounter, adds placed in advance, trash that joins);
3. the GM override (`.dd flex set <n>`) of the instance;
4. a fresh count of the players.

A player joining mid-fight changes nothing until the next pull.

**Hold, reset, no stacking.** A hold stores the count and the creature's base (5-player) max health in the creature's
`CustomData` and sets create health and `UNIT_MOD_HEALTH` `BASE_VALUE` to base x ladder (max health follows through
`UpdateMaxHealth`, so health auras stay one multiplication), keeping the health percent. The base is the create health
(template, heroic template, `UpdateEntry`, or a script's `SetCreateHealth`), or the max health when a script wrote that
field alone (the kit's `SummonWave` health). Evade (`OnUnitEnterEvadeMode`, after `Reset`) restores the base and drops
the hold; death (`OnUnitDeath`, after `JustDied`) and removal from the world drop it too. A hold still there at the
next pull (a script evade path that skips the core's) is released first, and a creature still carrying the value the
scaler wrote resolves to its stored base, so health is never base x ladder x ladder. An `UpdateEntry` while held
and engaged rescales the new template health by the held count; a respawn or an idle `UpdateEntry` drops the hold.

**Staying scaled for the fight.** mod-autobalance resets max health (`SetMaxHealth(template)`, percent kept) on every
dungeon map, its disabled ones included, when a player levels inside or on `.reload config`. The flex
`OnAllCreatureUpdate` hook (flex maps, held creatures only) sees max health no longer matching the create health the
scaler wrote and puts the scaled value back in the same update, percent kept. The same hook releases a hold left on
an idle creature (alive, not engaged, not evading, not charmed).

**Hook order** (verified in `Creature::AtEngage`): the AI's `JustEngagedWith` runs first, then
`UnitScript::OnUnitEnterCombat`. Both go through `PullCount`, which holds at the first caller, so a boss script that
reads the count in `JustEngagedWith` and the hook that follows hold the same number and scale health once.

**Loot timing** (verified in `Unit::Kill`): the corpse loot is filled before `JustDied`, with the creature's
`LootMode`. A flex loot bit must be set at the pull (after `PullCount`), not in `JustDied`.

### API for boss scripts (`src/FlexScale.h`)

```cpp
#include "FlexScale.h"
namespace DungeonDifficulties::Flex
{
    bool IsFlexMap(Map const* map);
    uint8 HeldCount(Creature const* creature);              // 0 = not held
    uint8 PullCount(Creature* creature);                    // holds at the first caller once engaged
    uint32 BaseHealth(Creature const* creature);            // the stored 5-player base, 0 = never held or set
    void SetScaledMaxHealth(Creature* creature, uint32 base); // script-set health, base = the 5-player value
    uint8 ClampCount(uint32 players);
    float HealthLadder(uint8 count);
    uint8 TargetCount(uint8 count, uint8 normal, uint8 big);
    uint8 LootRolls(uint8 count);
    bool TankSwap(uint8 count);
}
```

- Call `PullCount(me)` in `JustEngagedWith` (or later) and keep the result for the fight; `HeldCount(me)` reads it
  again (still valid in `JustDied`: the hook drops the hold after it).
- Set health only through `SetScaledMaxHealth` on a flex map (hard modes, late arrivals, summoned bosses). Held, it
  applies base x ladder at once; not held, it sets the base and the next pull scales it. It keeps the health percent.
- Off a flex map everything degrades to the 5-player fight: `PullCount` returns 5, `HeldCount` 0, the helpers read 0
  as 5, `SetScaledMaxHealth` sets the base unscaled.
- mod-dungeon-bosses wraps this as `DungeonKit::Flex` (KT-B1); its scripts call the kit, not this header.

## Commands (GM)

- `.dd status`: config, heroic maps, SpellDifficulty and potency row counts.
- `.dd factor <spell>`: select a creature; prints the gate verdict where it stands, the SpellDifficulty source, its
  normal levels, `Fref`, and F per hook group (damage against you, heals on the creature). Off a listed heroic map it
  still computes F as a preview.
- `.dd flex`: the flex config; on a flex map the players, the count a fresh pull holds and its rules, the GM override,
  the live encounter and every held creature (count, max and base health). With a creature selected, its hold (count,
  source, time held, base x ladder, the target, loot and tank swap rules at its count) or the count a pull would hold
  now. Read only.
- `.dd flex set <n>`: test override for this instance (0 clears): pulls with nothing to inherit hold `n`, clamped.
  It lives with the instance, so an instance reset clears it; fights already held keep their count.

## Tests

`bash tests/run.sh` and `CXX=clang++ bash tests/run.sh` build the host tests against the pure headers:

- `tests/heroic_spell_test.cpp`: every Deadmines spell category of the handoff (Shoot, Fireball, Flamestrike,
  Rupture, Molten Metal, Acid Splash, Frost Nova stock and live, Fire Blast, Rhahk'Zor Slam, Poisoned Harpoon, Axe
  Toss, Cleave), mixed effects, heals, the SpellDifficulty index and the Lightning Bolt and Arcing Smash swaps, the
  gate and config parsing.
- `tests/flex_scale_test.cpp`: the ladder 1.00-2.25, the clamp below 5 and above 10, target counts at 5, 6 and 10,
  loot rolls, the tank swap only at 10, settings validation, the gate, count precedence (summoner, encounter,
  override, players), the encounter's owners, pull / evade / pull without stacking, a missed evade, both hook orders
  with a player joining between, script-set health (held, not held, max-only, changed mid-fight), health auras,
  `UpdateEntry` while held, death, adds and trash taking the encounter's count, the AutoBalance key parse, the
  re-assert after an AutoBalance max health reset, and the idle-hold / respawn release.

## E4 seam

Empowered heroics multiply their own damage factor on top of F in the same hooks (`HeroicSpellScaling.cpp`, `Apply`).
