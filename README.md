# mod-dungeon-difficulties

Dungeon difficulties for AzerothCore (WotLK 3.3.5a) on this server: vanilla dungeons get a level 60 heroic, and later
(stage E4) empowered heroic levels. Design: `.agents/plans/dungeon-difficulties/` in the core repository (PLAN,
STAGE-E0-E3-HANDOFF §3.6, the WP-H report).

Status: stage E2, the heroic creature spell scaling hook. Not yet built or playtested.

No core edits: existing `UnitScript` hooks only. Heroic templates, MapDifficulty rows and spawn masks come from
dbc-tools; this module only scales the stock spells those creatures cast.

## Layout

| File | What |
| --- | --- |
| `src/HeroicSpellMath.h` | Pure rule: effect roles, CalcValue mean, group factor, SpellDifficulty index, gate |
| `src/ReferenceHealth.h` | Pure keyed reference health rows (copied from mod-boss-mechanics) |
| `src/HeroicSpellScaling.cpp` | `UnitScript` hooks, engine inputs, startup data (SpellDifficulty, potency rows) |
| `src/DungeonDifficultiesConfig.cpp` | Config load (startup and `.reload config`) |
| `src/DungeonDifficultiesCommand.cpp` | `.dd status`, `.dd factor` |
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

## Config

| Key | Default | What |
| --- | --- | --- |
| `DungeonDifficulties.Enable` | `1` | the hook |
| `DungeonDifficulties.Heroic.Maps` | `36` (conf dist: `36 33 289`) | maps whose heroic the rule acts on |
| `DungeonDifficulties.ReferenceHealth` | `20:1300:1290 60:3670:3710 60h:4120:4110` | = BM's `ReferenceHealth.Rows` |
| `DungeonDifficulties.Heroic.SpellExceptions` | empty | stock spell ids left unscaled (cast or source id) |
| `DungeonDifficulties.Heroic.RequireHeroicTemplate` | `1` | scale only casters with a heroic template |
| `DungeonDifficulties.Debug` | `0` | log every scaled hit |

## Commands (GM)

- `.dd status`: config, heroic maps, SpellDifficulty and potency row counts.
- `.dd factor <spell>`: select a creature; prints the gate verdict where it stands, the SpellDifficulty source, its
  normal levels, `Fref`, and F per hook group (damage against you, heals on the creature). Off a listed heroic map it
  still computes F as a preview.

## Tests

`bash tests/run.sh` and `CXX=clang++ bash tests/run.sh` build `tests/heroic_spell_test.cpp` against the pure headers:
every Deadmines spell category of the handoff (Shoot, Fireball, Flamestrike, Rupture, Molten Metal, Acid Splash, Frost
Nova stock and live, Fire Blast, Rhahk'Zor Slam, Poisoned Harpoon, Axe Toss, Cleave), mixed effects, heals, the
SpellDifficulty index and the Lightning Bolt and Arcing Smash swaps, the gate and config parsing.

## E4 seam

Empowered heroics multiply their own damage factor on top of F in the same hooks (`HeroicSpellScaling.cpp`, `Apply`).
