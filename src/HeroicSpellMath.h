/*
 * mod-dungeon-difficulties: the heroic creature spell scaling rule (dungeon difficulties STAGE-E0-E3-HANDOFF §3.6,
 * decision §7 #3, WP-H). Pure: no AzerothCore includes, so tests/heroic_spell_test.cpp compiles it with the host
 * compiler. HeroicSpellScaling.cpp fills the inputs from the engine.
 *
 * The rule. A creature that no player controls casts a stock spell (id < 200000, not excepted) on a listed heroic
 * dungeon. The hit should do what the creature's NORMAL version does, times the reference health ratio
 *     Fref(L) = reference non-tank health (heroic row, caster's heroic level)
 *             / reference non-tank health (normal row, caster's normal level L),
 * so the spell takes the same share of a player's health on heroic as on normal. The engine has already grown the
 * value from level L to the heroic level (SpellEffectInfo::CalcValue: points per level up to MaxLevel, or the
 * creature BaseDamage ratio for SPELL_ATTR0_SCALES_WITH_CREATURE_LEVEL, plus the potency correction for the few
 * player spells creatures cast), so the hook multiplies the landed amount by
 *     F = wanted / actual
 *     wanted = sum over the source spell's effects in the hook's group of the effect's expected value, where a
 *              Scaled effect counts Fref(L) x its mean value at the normal level L (averaged over the normal
 *              template's level range) and every other role counts its mean value at the heroic caster
 *     actual = the same sum over the cast spell's effects, every role at the heroic caster.
 * For one Scaled effect this is F = Fref / G with G = mean(heroic) / mean(normal), the engine's growth. Weapon effects
 * (their weapon part scales with the heroic template already), percent-of-health effects and power burns count the
 * same on both sides, so a pure weapon or percent spell gets F = 1 and a mixed spell gets the weighted blend.
 *
 * SpellDifficulty swaps: on a heroic map the engine casts the spell's SpellDifficulty heroic id instead (a TBC/WotLK
 * heroic version for 20 vanilla creature spells). The hook then compares against the ORIGINAL normal spell (the
 * source): wanted uses the source spell's effects, actual the swapped spell's. That undoes the swap and applies the
 * same rule, even when the two spells' effect types differ (Arcing Smash 8374 weapon damage -> 38761 weapon percent).
 *
 * Groups (which effects one hook call carries):
 *   DirectDamage   ModifySpellDamageTaken: the Spell's summed direct damage (school damage, health leech, weapon
 *                  damage, power burn) or a proc-trigger-damage aura's hit.
 *   PeriodicDamage ModifyPeriodicDamageAurasTick: periodic damage, periodic leech, periodic damage percent. One tick
 *                  comes from one aura effect but the hook does not say which, so a spell with two periodic damage
 *                  effects gets their blend (exact in total when they tick together).
 *   DirectHeal     ModifyHealReceived from Unit::HealBySpell: heal, heal percent, heal max health.
 *   PeriodicHeal   ModifyHealReceived from the periodic heal tick: periodic heal, obs mod health (percent).
 * Creature heals on creatures use the target's heroic / normal max health ratio in place of Fref, uncapped: a heal
 * keeps its share of the target's max health (Cookie's Cooking stays ~4.1% of Cookie per cast on heroic, ~114x).
 *
 * The weapon part of a weapon effect is the caster's computed per-hit weapon damage (Scenario::weaponAverage, the
 * min / max the engine rolls in Unit::CalculateDamage: weapon plus AP, DamageModifier and swing included), so a mixed
 * school + weapon spell blends against what actually lands.
 *
 * Not modelled: spell_bonus_data ap_bonus / ap_dot_bonus. Unit::SpellDamageBonusDone adds the caster's attack power
 * times that coefficient after the hook's effect base is computed, so that share is multiplied by F too (and creature
 * AP grows with level). Deadmines has none (Fire Blast 2138 has a direct_bonus only, and creatures have no spell
 * power); re-check at each rollout.
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_HEROIC_SPELL_MATH_H
#define MOD_DUNGEON_DIFFICULTIES_HEROIC_SPELL_MATH_H

#include "ReferenceHealth.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace DungeonDifficulties
{
    namespace Heroic
    {
        // Custom spells (dbc-tools ids, mod-boss-mechanics resolve spells 205001-205007) are flat by design.
        constexpr std::uint32_t kCustomSpellIdFloor = 200000;
        constexpr std::size_t kMaxEffects = 3;
        // A safety rail against a degenerate damage spell (an actual amount near zero): a damage F never leaves
        // [0, kMaxFactor]. Heals are not capped: their wanted ratio is the healed creature's heroic / normal max
        // health, which is above 100 for a heroic boss, and a heal keeps its share of that health.
        constexpr double kMaxFactor = 20.0;

        // SpellEffects / AuraType values the rule reads (checked against the engine in HeroicSpellScaling.cpp).
        namespace Effect
        {
            enum : std::uint32_t
            {
                SchoolDamage = 2,
                Dummy = 3,
                PowerDrain = 8,
                HealthLeech = 9,
                Heal = 10,
                WeaponDamageNoSchool = 17,
                WeaponPercentDamage = 31,
                WeaponDamage = 58,
                PowerBurn = 62,
                HealMaxHealth = 67,
                HealMechanical = 75,
                ScriptEffect = 77,
                NormalizedWeaponDmg = 121,
                HealPct = 136,
                ForceCastWithValue = 141,
                TriggerSpellWithValue = 142,
                TriggerMissileSpellWithValue = 148
            };
        }

        namespace Aura
        {
            enum : std::uint32_t
            {
                PeriodicDamage = 3,
                Dummy = 4,
                PeriodicHeal = 8,
                DamageShield = 15,
                ObsModHealth = 20,
                ProcTriggerDamage = 43,
                PeriodicLeech = 53,
                PeriodicManaLeech = 64,
                SchoolAbsorb = 69,
                PeriodicDamagePercent = 89,
                PowerBurn = 162,
                PeriodicTriggerSpellWithValue = 227
            };
        }

        // A spell_potency_correction row (SpellPotency::Apply): CalcValue adds correctionPerLevel per level below
        // breakpointLevel, then rolls a multiplier in [1, 1 + variancePct/100]. Any caster, creatures included.
        struct PotencyRow
        {
            bool present = false;
            float correctionPerLevel = 0.f;
            std::uint32_t breakpointLevel = 0;
            float variancePct = 0.f;
        };

        struct EffectData
        {
            std::uint32_t effect = 0;
            std::uint32_t aura = 0;                      // ApplyAuraName; 0 unless an aura effect
            std::int32_t basePoints = 0;
            std::int32_t dieSides = 0;
            float pointsPerLevel = 0.f;                  // RealPointsPerLevel
            bool triggerScalesWithCreatureLevel = false; // TriggerSpell exists and has SCALES_WITH_CREATURE_LEVEL
            PotencyRow potency;
        };

        struct SpellData
        {
            std::uint32_t id = 0;
            std::uint32_t spellLevel = 0;
            std::uint32_t baseLevel = 0;
            std::uint32_t maxLevel = 0;
            bool scalesWithCreatureLevel = false;        // SPELL_ATTR0_SCALES_WITH_CREATURE_LEVEL
            std::array<EffectData, kMaxEffects> effects{};
        };

        // creature_classlevelstats BaseDamage of the caster's class and template expansion at a level.
        using BaseDamageFn = std::function<double(std::uint32_t level)>;

        // SpellEffectInfo::CalcValue's canEffectScale for SCALES_WITH_CREATURE_LEVEL spells.
        inline bool CanEffectScale(EffectData const& e)
        {
            bool can = false;
            switch (e.effect)
            {
                case Effect::SchoolDamage:
                case Effect::Dummy:
                case Effect::PowerDrain:
                case Effect::HealthLeech:
                case Effect::Heal:
                case Effect::WeaponDamage:
                case Effect::PowerBurn:
                case Effect::ScriptEffect:
                case Effect::NormalizedWeaponDmg:
                case Effect::ForceCastWithValue:
                case Effect::TriggerSpellWithValue:
                case Effect::TriggerMissileSpellWithValue:
                    can = true;
                    break;
                default:
                    break;
            }
            switch (e.aura)
            {
                case Aura::PeriodicDamage:
                case Aura::Dummy:
                case Aura::PeriodicHeal:
                case Aura::DamageShield:
                case Aura::ProcTriggerDamage:
                case Aura::PeriodicLeech:
                case Aura::PeriodicManaLeech:
                case Aura::SchoolAbsorb:
                case Aura::PeriodicTriggerSpellWithValue:
                    can = true;
                    break;
                default:
                    break;
            }
            if (e.triggerScalesWithCreatureLevel)
                can = false;     // the caller sets this only when the spell itself scales too, as CalcValue checks
            return can;
        }

        // The expected value SpellEffectInfo::CalcValue returns for a non-player-controlled creature caster of this
        // level (no combo points, no spell mods; the die roll and the potency variance at their means).
        inline double MeanEffectValue(SpellData const& s, std::size_t i, std::uint32_t casterLevel,
            BaseDamageFn const& baseDamage)
        {
            EffectData const& e = s.effects[i];
            std::int32_t basePoints = e.basePoints;
            if (e.pointsPerLevel != 0.f)
            {
                std::int32_t level = static_cast<std::int32_t>(casterLevel);
                if (level > static_cast<std::int32_t>(s.maxLevel) && s.maxLevel > 0)
                    level = static_cast<std::int32_t>(s.maxLevel);
                else if (level < static_cast<std::int32_t>(s.baseLevel))
                    level = static_cast<std::int32_t>(s.baseLevel);
                level -= static_cast<std::int32_t>(std::max(s.baseLevel, s.spellLevel));
                basePoints += static_cast<std::int32_t>(static_cast<float>(level) * e.pointsPerLevel);
            }
            double value = static_cast<double>(basePoints) + (e.dieSides == 0 ? 0.0 : (1.0 + e.dieSides) / 2.0);

            if (e.potency.present)
            {
                if (casterLevel < e.potency.breakpointLevel)
                    value += static_cast<double>(e.potency.correctionPerLevel)
                        * static_cast<double>(e.potency.breakpointLevel - casterLevel);
                if (e.potency.variancePct > 0.f)
                    value *= 1.0 + static_cast<double>(e.potency.variancePct) / 200.0;
            }

            if (s.scalesWithCreatureLevel && s.spellLevel && s.spellLevel != casterLevel && e.pointsPerLevel == 0.f
                && CanEffectScale(e) && baseDamage)
            {
                double const atCaster = baseDamage(casterLevel);
                double const atSpell = baseDamage(s.spellLevel);
                if (atSpell > 0.0)     // the engine divides by zero here; a missing stats row is a data bug
                    value *= atCaster / atSpell;
            }
            return value;
        }

        enum class Group : std::uint8_t
        {
            DirectDamage,
            PeriodicDamage,
            DirectHeal,
            PeriodicHeal
        };

        enum class Role : std::uint8_t
        {
            None,               // not carried by this group
            Scaled,             // the rule applies
            WeaponFixed,        // weapon damage bonus (the weapon part scales with the template)
            WeaponPercent,      // weapon damage percent
            PercentOfMaxHealth, // percent of the target's max health
            MaxHealth,          // the target's whole max health (heal max health)
            Unscaled            // power burn: mana based
        };

        inline Role RoleOf(EffectData const& e, Group g)
        {
            switch (g)
            {
                case Group::DirectDamage:
                    if (e.aura)
                        return e.aura == Aura::ProcTriggerDamage ? Role::Scaled
                            : e.aura == Aura::PowerBurn ? Role::Unscaled : Role::None;
                    switch (e.effect)
                    {
                        case Effect::SchoolDamage:
                        case Effect::HealthLeech:
                            return Role::Scaled;
                        case Effect::WeaponDamage:
                        case Effect::WeaponDamageNoSchool:
                        case Effect::NormalizedWeaponDmg:
                            return Role::WeaponFixed;
                        case Effect::WeaponPercentDamage:
                            return Role::WeaponPercent;
                        case Effect::PowerBurn:
                            return Role::Unscaled;
                        default:
                            return Role::None;
                    }
                case Group::PeriodicDamage:
                    switch (e.aura)
                    {
                        case Aura::PeriodicDamage:
                        case Aura::PeriodicLeech:
                            return Role::Scaled;
                        case Aura::PeriodicDamagePercent:
                            return Role::PercentOfMaxHealth;
                        default:
                            return Role::None;
                    }
                case Group::DirectHeal:
                    if (e.aura)
                        return Role::None;
                    switch (e.effect)
                    {
                        case Effect::Heal:
                        case Effect::HealMechanical:
                            return Role::Scaled;
                        case Effect::HealPct:
                            return Role::PercentOfMaxHealth;
                        case Effect::HealMaxHealth:
                            return Role::MaxHealth;
                        default:
                            return Role::None;
                    }
                case Group::PeriodicHeal:
                    switch (e.aura)
                    {
                        case Aura::PeriodicHeal:
                            return Role::Scaled;
                        case Aura::ObsModHealth:
                            return Role::PercentOfMaxHealth;
                        default:
                            return Role::None;
                    }
            }
            return Role::None;
        }

        inline bool IsHeal(Group g)
        {
            return g == Group::DirectHeal || g == Group::PeriodicHeal;
        }

        inline bool HasRole(SpellData const& s, Group g, Role r)
        {
            for (EffectData const& e : s.effects)
                if (RoleOf(e, g) == r)
                    return true;
            return false;
        }

        // Whether any effect of the spell belongs to the group.
        inline bool InGroup(SpellData const& s, Group g)
        {
            for (EffectData const& e : s.effects)
                if (RoleOf(e, g) != Role::None)
                    return true;
            return false;
        }

        // Everything the rule needs besides the spells.
        struct Scenario
        {
            std::uint32_t heroicLevel = 0;               // the caster's level now
            BaseDamageFn heroicBaseDamage;               // the caster's class, current (heroic) template expansion
            std::uint32_t normalMinLevel = 0;            // the normal template's level range
            std::uint32_t normalMaxLevel = 0;
            BaseDamageFn normalBaseDamage;               // the caster's class, normal template expansion
            // The multiplier a Scaled effect should end at, by normal level: Fref(L) for damage, the target's heroic /
            // normal max health ratio for creature heals on creatures.
            std::function<double(std::uint32_t normalLevel)> wantedRatio;
            // The caster's mean per-hit weapon damage for the spell's attack type, as the engine rolls it
            // (MeanWeaponDamage of Unit::CalculateDamage's min / max), not the bare base weapon.
            double weaponAverage = 0.0;
            double targetMaxHealth = 0.0;
        };

        struct GroupResult
        {
            double wanted = 0.0;
            double actual = 0.0;
            double factor = 1.0;
            bool scaled = false;                         // a Scaled effect took part (else F only undoes a swap)
        };

        namespace Detail
        {
            // The expected group amount of `s`. With `scaledAtNormal` the Scaled effects count wantedRatio(L) x their
            // mean at each normal level L (averaged over the range); every other role counts at the heroic caster.
            inline double GroupAmount(SpellData const& s, Group g, Scenario const& sc, bool scaledAtNormal,
                bool& anyScaled)
            {
                double scaled = 0.0;
                double weaponFixed = 0.0;
                double weaponPct = 1.0;
                bool weapon = false;
                double other = 0.0;
                for (std::size_t i = 0; i < kMaxEffects; ++i)
                {
                    Role const role = RoleOf(s.effects[i], g);
                    if (role == Role::None)
                        continue;
                    double const atHeroic = MeanEffectValue(s, i, sc.heroicLevel, sc.heroicBaseDamage);
                    switch (role)
                    {
                        case Role::Scaled:
                        {
                            anyScaled = true;
                            if (!scaledAtNormal)
                            {
                                scaled += std::max(0.0, atHeroic);
                                break;
                            }
                            std::uint32_t const lo = std::max<std::uint32_t>(1, sc.normalMinLevel);
                            std::uint32_t const hi = std::max(lo, sc.normalMaxLevel);
                            double sum = 0.0;
                            for (std::uint32_t level = lo; level <= hi; ++level)
                            {
                                double const ratio = sc.wantedRatio ? sc.wantedRatio(level) : 1.0;
                                sum += ratio * std::max(0.0, MeanEffectValue(s, i, level, sc.normalBaseDamage));
                            }
                            scaled += sum / static_cast<double>(hi - lo + 1);
                            break;
                        }
                        case Role::WeaponFixed:
                            weapon = true;
                            weaponFixed += atHeroic;
                            break;
                        case Role::WeaponPercent:
                            weapon = true;
                            weaponPct *= std::max(0.0, atHeroic) / 100.0;
                            break;
                        case Role::PercentOfMaxHealth:
                            other += std::max(0.0, atHeroic) / 100.0 * sc.targetMaxHealth;
                            break;
                        case Role::MaxHealth:
                            other += sc.targetMaxHealth;
                            break;
                        case Role::Unscaled:
                            other += std::max(0.0, atHeroic);
                            break;
                        default:
                            break;
                    }
                }
                double const weaponPart = weapon ? std::max(0.0, sc.weaponAverage + weaponFixed) * weaponPct : 0.0;
                return scaled + weaponPart + other;
            }
        }

        // F for one hook call. `cast` is the spell the hook sees; `source` the spell the creature cast on normal (the
        // same spell unless SpellDifficulty swapped it).
        inline GroupResult GroupFactor(SpellData const& cast, SpellData const& source, Group g, Scenario const& sc)
        {
            GroupResult r;
            bool anyScaled = false;
            r.wanted = Detail::GroupAmount(source, g, sc, true, anyScaled);
            r.actual = Detail::GroupAmount(cast, g, sc, false, anyScaled);
            r.scaled = anyScaled;
            if (cast.id == source.id && !anyScaled)
                return r;      // nothing to scale: F = 1 exactly
            if (r.actual <= 0.0)
                return r;
            double const factor = std::max(0.0, r.wanted / r.actual);
            r.factor = IsHeal(g) ? factor : std::min(factor, kMaxFactor);
            return r;
        }

        // The mean of the min / max weapon damage the engine rolls for a weapon spell (Spell::EffectWeaponDmg ->
        // Unit::CalculateDamage reads UNIT_FIELD_MINDAMAGE / MAXDAMAGE or their ranged / off-hand pair).
        inline double MeanWeaponDamage(double minDamage, double maxDamage)
        {
            return (std::max(0.0, minDamage) + std::max(0.0, maxDamage)) / 2.0;
        }

        // Tells the periodic heal tick's ModifyHealReceived call from Unit::HealBySpell's. The tick
        // (AuraEffect::HandlePeriodicHealAurasTick) calls ModifyPeriodicDamageAurasTick and right after it
        // ModifyHealReceived with the same (target, caster), spell and amount variable; HealBySpell passes (healer,
        // target) and an amount of its own. A periodic tick of a heal spell marks its call (amount address
        // included), every other damage hook call clears the mark, and ModifyHealReceived consumes it. A spell with
        // both a periodic heal and a periodic damage aura marks on its damage tick too, with no heal call after it:
        // the next damage hook clears that mark, and a later direct heal cannot match it (another amount variable,
        // the units swapped).
        // Pointers are compared, never dereferenced.
        class HealTickMarker
        {
        public:
            void Clear() { _mark = {}; }

            void Mark(void const* target, void const* caster, void const* spell, void const* amount)
            {
                _mark = { target, caster, spell, amount };
            }

            // Whether this ModifyHealReceived call (first, second, spell, amount) is the marked periodic tick; always
            // clears the mark.
            bool Consume(void const* first, void const* second, void const* spell, void const* amount)
            {
                Entry const m = _mark;
                Clear();
                return m.spell && m.spell == spell && m.target == first && m.caster == second && m.amount == amount;
            }

        private:
            struct Entry
            {
                void const* target = nullptr;
                void const* caster = nullptr;
                void const* spell = nullptr;
                void const* amount = nullptr;
            };
            Entry _mark;
        };

        // Fref at one normal level: heroic row at the caster's heroic level over the normal row at that level.
        inline double ReferenceRatio(RefHealth::Table const& t, std::uint32_t heroicLevel, std::uint32_t normalLevel)
        {
            ContentKey hk;
            hk.level = static_cast<std::uint8_t>(std::min<std::uint32_t>(heroicLevel, 255));
            hk.heroic = true;
            ContentKey nk;
            nk.level = static_cast<std::uint8_t>(std::min<std::uint32_t>(normalLevel, 255));
            double const heroic = RefHealth::Lookup(t, hk).nonTank;
            double const normal = RefHealth::Lookup(t, nk).nonTank;
            return normal > 0.0 ? heroic / normal : 1.0;
        }

        // Applies F to a hook amount (rounded, never negative).
        inline std::uint32_t ScaleAmount(std::uint32_t amount, double factor)
        {
            double const v = static_cast<double>(amount) * factor + 0.5;
            if (v <= 0.0)
                return 0;
            if (v >= static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
                return std::numeric_limits<std::uint32_t>::max();
            return static_cast<std::uint32_t>(v);
        }

        inline std::int32_t ScaleAmount(std::int32_t amount, double factor)
        {
            if (amount <= 0)
                return amount;
            std::uint32_t const v = ScaleAmount(static_cast<std::uint32_t>(amount), factor);
            return static_cast<std::int32_t>(std::min<std::uint32_t>(v, std::numeric_limits<std::int32_t>::max()));
        }

        // ------------------------------------------------------------------------------------------------------------
        // SpellDifficulty: which normal spell a heroic cast came from
        // ------------------------------------------------------------------------------------------------------------

        // A SpellDifficulty row (DBC merged with spelldifficulty_dbc), slots 0-3 = difficulty 0-3; a slot whose spell
        // is missing from the spell store must be passed as 0, as DBCStores does.
        struct SpellDifficultyRow
        {
            std::uint32_t id = 0;
            std::array<std::uint32_t, 4> spells{};
        };

        // Replicates the engine's searcher (DBCStores.cpp: rows with slots 0 and 1 set, every spell of the row maps
        // to it, later rows win) and SpellMgr::GetSpellIdForDifficulty, plus the reverse lookup.
        //
        // A heroic-slot id is always read as the swap of its normal spell. A creature that casts that id as itself
        // (it stays itself on heroic) is rescaled against the normal spell too: the DBC cannot tell the two apart.
        // Six vanilla creature spells are both (15232, 15305, 15472, 15530, 15582, 15616; see the README).
        class SpellDifficultyIndex
        {
        public:
            struct Source
            {
                std::uint32_t spellId = 0;   // the normal spell; the cast id itself when not swapped
                bool swapped = false;
                // Several normal spells swap to the cast id: the hook rescales against the lowest one.
                bool ambiguous = false;
            };

            void Build(std::vector<SpellDifficultyRow> rows)
            {
                _rows.clear();
                _searcher.clear();
                for (auto& reverse : _reverse)
                    reverse.clear();
                std::sort(rows.begin(), rows.end(), [](SpellDifficultyRow const& a, SpellDifficultyRow const& b)
                {
                    return a.id < b.id;
                });
                for (SpellDifficultyRow const& row : rows)
                {
                    if (!row.spells[0] || !row.spells[1])
                        continue;
                    _rows[row.id] = row;
                    for (std::uint32_t spell : row.spells)
                        if (spell)
                            _searcher[spell] = row.id;
                }
                for (auto const& [spell, rowId] : _searcher)
                {
                    for (std::uint32_t mode = 1; mode < 4; ++mode)
                    {
                        std::uint32_t const to = SwapFor(spell, mode);
                        if (to != spell)
                            _reverse[mode][to].push_back(spell);
                    }
                }
            }

            // The spell the engine casts for `spellId` at difficulty `mode` on a dungeon or battleground map.
            std::uint32_t SwapFor(std::uint32_t spellId, std::uint32_t mode) const
            {
                if (mode >= 4)
                    return spellId;
                auto const it = _searcher.find(spellId);
                if (it == _searcher.end())
                    return spellId;
                auto const row = _rows.find(it->second);
                if (row == _rows.end())
                    return spellId;
                if (!row->second.spells[mode] && mode > 1)
                    mode -= 2;
                return row->second.spells[mode] ? row->second.spells[mode] : spellId;
            }

            Source SourceOf(std::uint32_t castId, std::uint32_t mode) const
            {
                Source s;
                s.spellId = castId;
                if (mode == 0 || mode >= 4)
                    return s;
                auto const it = _reverse[mode].find(castId);
                if (it == _reverse[mode].end() || it->second.empty())
                    return s;
                s.spellId = *std::min_element(it->second.begin(), it->second.end());
                s.swapped = true;
                s.ambiguous = it->second.size() > 1;
                return s;
            }

            std::size_t Size() const { return _rows.size(); }

        private:
            std::map<std::uint32_t, SpellDifficultyRow> _rows;
            std::map<std::uint32_t, std::uint32_t> _searcher;
            std::array<std::map<std::uint32_t, std::vector<std::uint32_t>>, 4> _reverse;
        };

        // ------------------------------------------------------------------------------------------------------------
        // The gate
        // ------------------------------------------------------------------------------------------------------------

        struct Settings
        {
            bool enabled = false;
            std::set<std::uint32_t> heroicMaps;
            std::set<std::uint32_t> exceptions;      // stock spell ids left unscaled (cast or source id)
            // Scale only casters running a heroic difficulty template (current entry != normal entry): a creature
            // the heroic generator missed keeps stock spells to match its stock melee.
            bool requireHeroicTemplate = true;
        };

        enum class Kind : std::uint8_t
        {
            Damage,     // the target must be player controlled (players, their pets and guardians, bots)
            Heal        // the target must be a creature no player controls
        };

        struct GateInputs
        {
            Kind kind = Kind::Damage;
            bool hasCaster = false;
            bool casterIsCreature = false;
            bool casterControlledByPlayer = false;
            bool dungeon = false;                     // Map::IsDungeon()
            bool heroic = false;                      // Map::IsHeroic()
            std::uint32_t mapId = 0;
            std::uint32_t castSpellId = 0;
            std::uint32_t sourceSpellId = 0;          // after undoing a SpellDifficulty swap
            bool casterHasHeroicTemplate = false;     // current template entry differs from the normal entry
            bool targetIsCreature = false;
            bool targetControlledByPlayer = false;
        };

        enum class Verdict : std::uint8_t
        {
            Act,
            Disabled,
            NoCaster,
            NotCreature,
            PlayerControlled,
            NotDungeon,
            NotListedMap,
            NotHeroic,
            NoHeroicTemplate,
            CustomSpell,
            Exception,
            TargetNotPlayer,
            TargetNotCreature
        };

        inline char const* VerdictName(Verdict v)
        {
            switch (v)
            {
                case Verdict::Act: return "scaled";
                case Verdict::Disabled: return "module disabled";
                case Verdict::NoCaster: return "no caster";
                case Verdict::NotCreature: return "caster is not a creature";
                case Verdict::PlayerControlled: return "caster is player controlled";
                case Verdict::NotDungeon: return "not a dungeon";
                case Verdict::NotListedMap: return "map not in DungeonDifficulties.Heroic.Maps";
                case Verdict::NotHeroic: return "not heroic";
                case Verdict::NoHeroicTemplate: return "caster has no heroic template";
                case Verdict::CustomSpell: return "custom spell (id >= 200000)";
                case Verdict::Exception: return "in DungeonDifficulties.Heroic.SpellExceptions";
                case Verdict::TargetNotPlayer: return "target is not player controlled";
                case Verdict::TargetNotCreature: return "heal target is not a creature";
            }
            return "?";
        }

        inline Verdict Check(Settings const& s, GateInputs const& in)
        {
            if (!s.enabled)
                return Verdict::Disabled;
            if (!in.hasCaster)
                return Verdict::NoCaster;
            if (!in.casterIsCreature)
                return Verdict::NotCreature;
            if (in.casterControlledByPlayer)
                return Verdict::PlayerControlled;
            if (!in.dungeon)
                return Verdict::NotDungeon;
            if (!s.heroicMaps.count(in.mapId))
                return Verdict::NotListedMap;
            if (!in.heroic)
                return Verdict::NotHeroic;
            if (s.requireHeroicTemplate && !in.casterHasHeroicTemplate)
                return Verdict::NoHeroicTemplate;
            if (in.castSpellId >= kCustomSpellIdFloor || in.sourceSpellId >= kCustomSpellIdFloor)
                return Verdict::CustomSpell;
            if (s.exceptions.count(in.castSpellId) || s.exceptions.count(in.sourceSpellId))
                return Verdict::Exception;
            if (in.kind == Kind::Damage && !in.targetControlledByPlayer)
                return Verdict::TargetNotPlayer;
            if (in.kind == Kind::Heal && (!in.targetIsCreature || in.targetControlledByPlayer))
                return Verdict::TargetNotCreature;
            return Verdict::Act;
        }

        // ------------------------------------------------------------------------------------------------------------
        // Config parsing
        // ------------------------------------------------------------------------------------------------------------

        // Parses a list of ids ("36 33, 289"); empty is valid. On failure `out` is empty and `err` names the entry.
        inline bool ParseIdList(std::string_view text, std::set<std::uint32_t>& out, std::string& err)
        {
            out.clear();
            err.clear();
            for (std::string_view tok : RefHealth::Detail::Tokens(text))
            {
                std::uint64_t v = 0;
                if (!RefHealth::Detail::ParseUnsigned(tok, std::numeric_limits<std::uint32_t>::max(), v))
                {
                    err = "bad entry '" + std::string(tok) + "' (want a list of unsigned ids)";
                    out.clear();
                    return false;
                }
                out.insert(static_cast<std::uint32_t>(v));
            }
            return true;
        }

        inline std::string FormatIdList(std::set<std::uint32_t> const& ids)
        {
            std::string s;
            for (std::uint32_t id : ids)
                s += (s.empty() ? "" : " ") + std::to_string(id);
            return s;
        }
    }
}

#endif
