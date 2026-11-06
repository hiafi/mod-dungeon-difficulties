/*
 * mod-dungeon-difficulties: the heroic creature spell damage hook (STAGE-E0-E3-HANDOFF §3.6, WP-H). The rule and its
 * math are in HeroicSpellMath.h; this file reads the engine (templates, base stats, spells, SpellDifficulty, potency
 * rows) and applies F in three UnitScript hooks:
 *
 * - ModifySpellDamageTaken (Unit::CalculateSpellDamageTaken, before armor, crit and absorbs): group DirectDamage.
 * - ModifyPeriodicDamageAurasTick: group PeriodicDamage. The periodic heal tick calls it too, right before
 *   ModifyHealReceived; a spell with a periodic heal aura and no periodic damage aura is that path and is skipped.
 * - ModifyHealReceived: Unit::HealBySpell passes (healer, target), the periodic heal tick passes (target, healer). The
 *   periodic hook leaves a thread-local Heroic::HealTickMarker of its (target, caster, spell, amount) call for heal
 *   spells, which tells the two apart: a match is the periodic path (group PeriodicHeal), anything else the direct one
 *   (group DirectHeal). Every damage hook call clears the marker first, so it never outlives the tick that set it.
 *
 * Mechanics resolve spells (205001-205007) carry their own flat damage and are excluded by the custom id floor.
 * E4 (empowered heroics) multiplies its own damage factor on top here.
 */

#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DungeonDifficulties.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Unit.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include <unordered_map>

namespace DungeonDifficulties
{
    namespace Effect = Heroic::Effect;
    namespace Aura = Heroic::Aura;

    // HeroicSpellMath.h keeps its own copies of the effect and aura ids (it compiles without the core).
    constexpr bool Same(uint32 mine, uint32 engine) { return mine == engine; }
    static_assert(Same(Effect::SchoolDamage, SPELL_EFFECT_SCHOOL_DAMAGE));
    static_assert(Same(Effect::Dummy, SPELL_EFFECT_DUMMY));
    static_assert(Same(Effect::PowerDrain, SPELL_EFFECT_POWER_DRAIN));
    static_assert(Same(Effect::HealthLeech, SPELL_EFFECT_HEALTH_LEECH));
    static_assert(Same(Effect::Heal, SPELL_EFFECT_HEAL));
    static_assert(Same(Effect::WeaponDamageNoSchool, SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL));
    static_assert(Same(Effect::WeaponPercentDamage, SPELL_EFFECT_WEAPON_PERCENT_DAMAGE));
    static_assert(Same(Effect::WeaponDamage, SPELL_EFFECT_WEAPON_DAMAGE));
    static_assert(Same(Effect::PowerBurn, SPELL_EFFECT_POWER_BURN));
    static_assert(Same(Effect::HealMaxHealth, SPELL_EFFECT_HEAL_MAX_HEALTH));
    static_assert(Same(Effect::HealMechanical, SPELL_EFFECT_HEAL_MECHANICAL));
    static_assert(Same(Effect::ScriptEffect, SPELL_EFFECT_SCRIPT_EFFECT));
    static_assert(Same(Effect::NormalizedWeaponDmg, SPELL_EFFECT_NORMALIZED_WEAPON_DMG));
    static_assert(Same(Effect::HealPct, SPELL_EFFECT_HEAL_PCT));
    static_assert(Same(Effect::ForceCastWithValue, SPELL_EFFECT_FORCE_CAST_WITH_VALUE));
    static_assert(Same(Effect::TriggerSpellWithValue, SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE));
    static_assert(Same(Effect::TriggerMissileSpellWithValue, SPELL_EFFECT_TRIGGER_MISSILE_SPELL_WITH_VALUE));
    static_assert(Same(Aura::PeriodicDamage, SPELL_AURA_PERIODIC_DAMAGE));
    static_assert(Same(Aura::Dummy, SPELL_AURA_DUMMY));
    static_assert(Same(Aura::PeriodicHeal, SPELL_AURA_PERIODIC_HEAL));
    static_assert(Same(Aura::DamageShield, SPELL_AURA_DAMAGE_SHIELD));
    static_assert(Same(Aura::ObsModHealth, SPELL_AURA_OBS_MOD_HEALTH));
    static_assert(Same(Aura::ProcTriggerDamage, SPELL_AURA_PROC_TRIGGER_DAMAGE));
    static_assert(Same(Aura::PeriodicLeech, SPELL_AURA_PERIODIC_LEECH));
    static_assert(Same(Aura::PeriodicManaLeech, SPELL_AURA_PERIODIC_MANA_LEECH));
    static_assert(Same(Aura::SchoolAbsorb, SPELL_AURA_SCHOOL_ABSORB));
    static_assert(Same(Aura::PeriodicDamagePercent, SPELL_AURA_PERIODIC_DAMAGE_PERCENT));
    static_assert(Same(Aura::PowerBurn, SPELL_AURA_POWER_BURN));
    static_assert(Same(Aura::PeriodicTriggerSpellWithValue, SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE));
    static_assert(Heroic::kMaxEffects == MAX_SPELL_EFFECTS, "HeroicSpellMath.h assumes three spell effects");

    namespace
    {
        Heroic::SpellDifficultyIndex sSwaps;
        std::unordered_map<uint32, Heroic::PotencyRow> sPotency;     // key spellId << 2 | effect index

        // The periodic heal tick's call, set by ModifyPeriodicDamageAurasTick for heal spells and consumed by the
        // ModifyHealReceived call that follows it on the same thread (HeroicSpellMath.h).
        thread_local Heroic::HealTickMarker tMarker;

        uint32 PotencyKey(uint32 spellId, uint8 effIndex)
        {
            return (spellId << 2) | effIndex;
        }

        Heroic::SpellData ToSpellData(SpellInfo const* info)
        {
            Heroic::SpellData s;
            s.id = info->Id;
            s.spellLevel = info->SpellLevel;
            s.baseLevel = info->BaseLevel;
            s.maxLevel = info->MaxLevel;
            s.scalesWithCreatureLevel = info->HasAttribute(SPELL_ATTR0_SCALES_WITH_CREATURE_LEVEL);
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                SpellEffectInfo const& fx = info->Effects[i];
                Heroic::EffectData& e = s.effects[i];
                e.effect = fx.Effect;
                e.aura = fx.ApplyAuraName;
                e.basePoints = fx.BasePoints;
                e.dieSides = fx.DieSides;
                e.pointsPerLevel = fx.RealPointsPerLevel;
                if (s.scalesWithCreatureLevel && fx.TriggerSpell)
                    if (SpellInfo const* trigger = sSpellMgr->GetSpellInfo(fx.TriggerSpell))
                        e.triggerScalesWithCreatureLevel =
                            trigger->HasAttribute(SPELL_ATTR0_SCALES_WITH_CREATURE_LEVEL);
                auto const it = sPotency.find(PotencyKey(info->Id, i));
                if (it != sPotency.end())
                    e.potency = it->second;
            }
            return s;
        }

        Heroic::BaseDamageFn BaseDamageOf(uint8 unitClass, uint32 expansion)
        {
            return [unitClass, expansion](uint32 level) -> double
            {
                CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(uint8(std::min<uint32>(level, 255)),
                    unitClass);
                return expansion < MAX_EXPANSIONS ? stats->BaseDamage[expansion] : 0.0;
            };
        }

        double TemplateHealth(CreatureTemplate const* tpl, uint32 level)
        {
            CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(uint8(std::min<uint32>(level, 255)),
                tpl->unit_class);
            return double(stats->GenerateHealth(tpl));
        }

        bool HasAuraType(SpellInfo const* info, uint32 aura)
        {
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (info->Effects[i].ApplyAuraName == aura)
                    return true;
            return false;
        }

        bool IsPeriodicHealSpell(SpellInfo const* info)
        {
            return HasAuraType(info, SPELL_AURA_PERIODIC_HEAL) || HasAuraType(info, SPELL_AURA_OBS_MOD_HEALTH);
        }

        bool IsPeriodicDamageSpell(SpellInfo const* info)
        {
            return HasAuraType(info, SPELL_AURA_PERIODIC_DAMAGE) || HasAuraType(info, SPELL_AURA_PERIODIC_LEECH)
                || HasAuraType(info, SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
        }

        bool HasWeaponEffect(SpellInfo const* info, bool normalizedOnly = false)
        {
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                switch (info->Effects[i].Effect)
                {
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                        return true;
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                        if (!normalizedOnly)
                            return true;
                        break;
                    default:
                        break;
                }
            }
            return false;
        }

        // Spell::Spell's m_attackType for the spell.
        WeaponAttackType AttackTypeOf(SpellInfo const* info)
        {
            switch (info->DmgClass)
            {
                case SPELL_DAMAGE_CLASS_MELEE:
                    return info->HasAttribute(SPELL_ATTR3_REQUIRES_OFF_HAND_WEAPON) ? OFF_ATTACK : BASE_ATTACK;
                case SPELL_DAMAGE_CLASS_RANGED:
                    return info->IsRangedWeaponSpell() ? RANGED_ATTACK : BASE_ATTACK;
                default:
                    return info->HasAttribute(SPELL_ATTR2_AUTO_REPEAT) ? RANGED_ATTACK : BASE_ATTACK;
            }
        }

        // The mean weapon damage the engine rolls for a weapon spell of `info` cast by `caster`: Spell::EffectWeaponDmg
        // -> Unit::CalculateDamage(attack, normalized, physical), which reads the unit's min / max damage fields for a
        // plain physical hit and recomputes them (CalculateMinMaxDamage) otherwise. Weapon plus AP, DamageModifier and
        // swing included: the per-hit damage of the current (heroic) template, not its bare base weapon.
        double WeaponDamageOf(Unit* caster, SpellInfo const* info)
        {
            WeaponAttackType const attack = AttackTypeOf(info);
            bool const normalized = HasWeaponEffect(info, true);
            bool const physical = info->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL;
            float minDamage = 0.f;
            float maxDamage = 0.f;
            if (normalized || !physical)
            {
                for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
                {
                    float minTmp = 0.f;
                    float maxTmp = 0.f;
                    caster->CalculateMinMaxDamage(attack, normalized, physical, minTmp, maxTmp, i);
                    minDamage += minTmp;
                    maxDamage += maxTmp;
                }
            }
            else
            {
                switch (attack)
                {
                    case RANGED_ATTACK:
                        minDamage = caster->GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
                        maxDamage = caster->GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);
                        break;
                    case OFF_ATTACK:
                        minDamage = caster->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE);
                        maxDamage = caster->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE);
                        break;
                    default:
                        minDamage = caster->GetFloatValue(UNIT_FIELD_MINDAMAGE);
                        maxDamage = caster->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
                        break;
                }
            }
            return Heroic::MeanWeaponDamage(minDamage, maxDamage);
        }
    }

    char const* GroupName(Heroic::Group group)
    {
        switch (group)
        {
            case Heroic::Group::DirectDamage: return "direct damage";
            case Heroic::Group::PeriodicDamage: return "periodic damage";
            case Heroic::Group::DirectHeal: return "direct heal";
            case Heroic::Group::PeriodicHeal: return "periodic heal";
        }
        return "?";
    }

    void LoadSpellData()
    {
        sPotency.clear();
        if (QueryResult result = WorldDatabase.Query("SELECT spell_id, effect_index, correction_per_level, "
            "breakpoint_level, variance_pct FROM spell_potency_correction"))
        {
            do
            {
                Field* f = result->Fetch();
                uint8 const effIndex = f[1].Get<uint8>();
                if (effIndex >= MAX_SPELL_EFFECTS)
                    continue;
                Heroic::PotencyRow row;
                row.present = true;
                row.correctionPerLevel = f[2].Get<float>();
                row.breakpointLevel = f[3].Get<uint8>();
                row.variancePct = f[4].Get<float>();
                sPotency[PotencyKey(f[0].Get<uint32>(), effIndex)] = row;
            } while (result->NextRow());
        }

        // The rows DBCStores feeds the engine's searcher: a slot whose spell is missing from the store counts as 0.
        std::vector<Heroic::SpellDifficultyRow> rows;
        for (SpellDifficultyEntry const* entry : sSpellDifficultyStore)
        {
            Heroic::SpellDifficultyRow row;
            row.id = entry->ID;
            for (uint8 x = 0; x < MAX_DIFFICULTY; ++x)
                if (entry->SpellID[x] > 0 && sSpellMgr->GetSpellInfo(uint32(entry->SpellID[x])))
                    row.spells[x] = uint32(entry->SpellID[x]);
            rows.push_back(row);
        }
        sSwaps.Build(std::move(rows));

        LOG_INFO(kLog, "mod-dungeon-difficulties: {} SpellDifficulty rows indexed, {} potency rows read",
            sSwaps.Size(), sPotency.size());
    }

    std::size_t SpellDifficultyRowCount()
    {
        return sSwaps.Size();
    }

    std::size_t PotencyRowCount()
    {
        return sPotency.size();
    }

    Evaluation Evaluate(Unit* caster, Unit* target, SpellInfo const* spellInfo, Heroic::Group group, bool force)
    {
        Evaluation ev;
        ModuleConfig const& cfg = GetConfig();
        bool const heal = group == Heroic::Group::DirectHeal || group == Heroic::Group::PeriodicHeal;

        Heroic::GateInputs in;
        in.kind = heal ? Heroic::Kind::Heal : Heroic::Kind::Damage;
        in.castSpellId = spellInfo ? spellInfo->Id : 0;
        in.sourceSpellId = in.castSpellId;
        in.hasCaster = caster && spellInfo && caster->IsInWorld();
        Map* map = in.hasCaster ? caster->GetMap() : nullptr;
        if (map)
        {
            in.casterIsCreature = caster->IsCreature();
            in.casterControlledByPlayer = caster->IsControlledByPlayer();
            if (Creature const* c = caster->ToCreature())
                if (CreatureTemplate const* tpl = c->GetCreatureTemplate())
                    in.casterHasHeroicTemplate = tpl->Entry != c->GetEntry();
            in.dungeon = map->IsDungeon();
            in.heroic = map->IsHeroic();
            in.mapId = map->GetId();
            // The reverse SpellDifficulty lookup only where the rule can act (or for the command's preview).
            if (force || (in.casterIsCreature && in.dungeon && in.heroic && cfg.heroic.heroicMaps.count(in.mapId)))
            {
                ev.source = sSwaps.SourceOf(in.castSpellId, in.dungeon ? map->GetSpawnMode() : 0);
                in.sourceSpellId = ev.source.spellId;
            }
        }
        else
            in.hasCaster = false;
        if (target)
        {
            in.targetIsCreature = target->IsCreature();
            in.targetControlledByPlayer = target->IsControlledByPlayer();
        }
        ev.verdict = Heroic::Check(cfg.heroic, in);
        if (ev.verdict != Heroic::Verdict::Act && !(force && in.hasCaster && in.casterIsCreature))
            return ev;

        Creature* creature = caster->ToCreature();
        CreatureTemplate const* current = creature->GetCreatureTemplate();
        CreatureTemplate const* normal = sObjectMgr->GetCreatureTemplate(creature->GetEntry());
        if (!current || !normal)
            return ev;
        SpellInfo const* sourceInfo = sSpellMgr->GetSpellInfo(ev.source.spellId);
        if (!sourceInfo)
            sourceInfo = spellInfo;

        uint8 const unitClass = caster->getClass();
        Heroic::Scenario sc;
        sc.heroicLevel = caster->GetLevel();
        sc.heroicBaseDamage = BaseDamageOf(unitClass, current->expansion);
        sc.normalMinLevel = std::min(normal->minlevel, normal->maxlevel);
        sc.normalMaxLevel = std::max(normal->minlevel, normal->maxlevel);
        sc.normalBaseDamage = BaseDamageOf(unitClass, normal->expansion);
        sc.targetMaxHealth = target ? double(target->GetMaxHealth()) : 0.0;
        if (HasWeaponEffect(spellInfo) || HasWeaponEffect(sourceInfo))
            sc.weaponAverage = WeaponDamageOf(caster, spellInfo);

        // Effects only: a spell_bonus_data ap_bonus / ap_dot_bonus share (Unit::SpellDamageBonusDone adds the caster's
        // attack power x the coefficient) is not modelled and gets F as well (HeroicSpellMath.h, "Not modelled").
        if (heal)
        {
            // Creature heals on creatures: the target's heroic / normal max health (from the templates, so auras and
            // the current level of a creature without a heroic template give ratio 1). Uncapped (GroupFactor).
            Creature const* healed = target ? target->ToCreature() : nullptr;
            CreatureTemplate const* healedCurrent = healed ? healed->GetCreatureTemplate() : nullptr;
            CreatureTemplate const* healedNormal =
                healed ? sObjectMgr->GetCreatureTemplate(healed->GetEntry()) : nullptr;
            if (healedCurrent && healedNormal)
            {
                double const heroicHealth = TemplateHealth(healedCurrent, healed->GetLevel());
                uint32 const lo = std::min(healedNormal->minlevel, healedNormal->maxlevel);
                uint32 const hi = std::max(healedNormal->minlevel, healedNormal->maxlevel);
                double normalHealth = 0.0;
                for (uint32 level = lo; level <= hi; ++level)
                    normalHealth += TemplateHealth(healedNormal, level);
                normalHealth /= double(hi - lo + 1);
                double const ratio = normalHealth > 0.0 ? heroicHealth / normalHealth : 1.0;
                sc.wantedRatio = [ratio](uint32) { return ratio; };
            }
            else
                sc.wantedRatio = [](uint32) { return 1.0; };
        }
        else
        {
            RefHealth::Table const* table = &cfg.referenceHealth;
            uint32 const heroicLevel = sc.heroicLevel;
            sc.wantedRatio = [table, heroicLevel](uint32 normalLevel)
            {
                return Heroic::ReferenceRatio(*table, heroicLevel, normalLevel);
            };
        }

        ev.heroicLevel = sc.heroicLevel;
        ev.normalMinLevel = sc.normalMinLevel;
        ev.normalMaxLevel = sc.normalMaxLevel;
        ev.refRatioLow = sc.wantedRatio(std::max<uint32>(1, sc.normalMinLevel));
        ev.refRatioHigh = sc.wantedRatio(std::max<uint32>(1, sc.normalMaxLevel));
        ev.result = Heroic::GroupFactor(ToSpellData(spellInfo), ToSpellData(sourceInfo), group, sc);
        return ev;
    }

    namespace
    {
        template <typename T>
        void Apply(Unit* caster, Unit* target, T& amount, SpellInfo const* spellInfo, Heroic::Group group)
        {
            if (!(amount > 0) || !GetConfig().heroic.enabled)
                return;
            Evaluation const ev = Evaluate(caster, target, spellInfo, group, false);
            if (ev.verdict != Heroic::Verdict::Act || ev.result.factor == 1.0)
                return;
            T const before = amount;
            amount = Heroic::ScaleAmount(amount, ev.result.factor);
            if (GetConfig().debug)
                LOG_INFO(kLog, "heroic {}: {} spell {}{} on {}: {} -> {} (F {:.3f}, wanted {:.1f}, actual {:.1f}, "
                    "level {} from {}-{})", GroupName(group), caster->GetName(), spellInfo->Id,
                    ev.source.swapped ? Acore::StringFormat(" (swapped from {}{})", ev.source.spellId,
                        ev.source.ambiguous ? ", ambiguous" : "") : "", target->GetName(), before, amount,
                    ev.result.factor, ev.result.wanted, ev.result.actual, ev.heroicLevel, ev.normalMinLevel,
                    ev.normalMaxLevel);
        }
    }
}

using namespace DungeonDifficulties;

class DungeonDifficultiesUnitScript : public UnitScript
{
public:
    DungeonDifficultiesUnitScript() : UnitScript("DungeonDifficultiesUnitScript", true,
        { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
            UNITHOOK_MODIFY_HEAL_RECEIVED }) { }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        tMarker.Clear();
        if (!target || !spellInfo)
            return;
        Apply(attacker, target, damage, spellInfo, Heroic::Group::DirectDamage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo)
        override
    {
        tMarker.Clear();
        if (!target || !spellInfo)
            return;
        if (IsPeriodicHealSpell(spellInfo))
        {
            // Possibly the periodic heal tick (SpellAuraEffects.cpp HandlePeriodicHealAurasTick), whose
            // ModifyHealReceived call follows with the same (target, caster, spell) and the same amount variable.
            tMarker.Mark(target, attacker, spellInfo, &damage);
            if (!IsPeriodicDamageSpell(spellInfo))
                return;
        }
        Apply(attacker, target, damage, spellInfo, Heroic::Group::PeriodicDamage);
    }

    void ModifyHealReceived(Unit* first, Unit* second, uint32& heal, SpellInfo const* spellInfo) override
    {
        bool const periodic = tMarker.Consume(first, second, spellInfo, &heal);
        if (!first || !second || !spellInfo)
            return;
        Unit* healer = periodic ? second : first;
        Unit* target = periodic ? first : second;
        Apply(healer, target, heal, spellInfo, periodic ? Heroic::Group::PeriodicHeal : Heroic::Group::DirectHeal);
    }
};

class DungeonDifficultiesWorldScript : public WorldScript
{
public:
    DungeonDifficultiesWorldScript() : WorldScript("DungeonDifficultiesWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig(reload);
        if (reload)
            LoadSpellData();     // potency rows may have changed with the config; cheap
    }

    void OnStartup() override
    {
        LoadSpellData();         // needs the DBC stores and SpellMgr, which load after the config
    }
};

void AddSC_dungeon_difficulties_heroic_scaling()
{
    new DungeonDifficultiesUnitScript();
    new DungeonDifficultiesWorldScript();
}
