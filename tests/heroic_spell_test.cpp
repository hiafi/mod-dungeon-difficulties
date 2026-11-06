// Host tests for src/HeroicSpellMath.h and src/ReferenceHealth.h (no server build). Spell data is the live server's
// (Spell.dbc merged with spell_dbc, 2026-11-05); base damage is creature_classlevelstats damage_base (expansion 0).

#include "HeroicSpellMath.h"
#include "check.h"
#include <map>

using namespace DungeonDifficulties;
using namespace DungeonDifficulties::Heroic;

namespace
{
    constexpr char const* kRows = "20:1300:1290 60:3670:3710 60h:4120:4110";
    constexpr double kFref = 4120.0 / 1300.0;

    std::map<std::uint32_t, double> const kWarrior = { { 10, 4.1084 }, { 15, 7.1981 }, { 17, 8.2225 },
        { 18, 8.5452 }, { 19, 8.9384 }, { 20, 9.6823 }, { 21, 9.9677 }, { 22, 10.4218 }, { 26, 12.0453 },
        { 58, 32.5967 }, { 59, 33.2007 }, { 60, 33.9625 }, { 61, 34.5714 } };
    std::map<std::uint32_t, double> const kMage = { { 10, 3.4451 }, { 15, 6.3298 }, { 17, 7.0485 }, { 18, 7.4083 },
        { 19, 7.9333 }, { 20, 8.3382 }, { 21, 8.5517 }, { 22, 8.9102 }, { 26, 10.3246 }, { 58, 27.6972 },
        { 59, 28.1686 }, { 60, 28.7983 }, { 61, 29.3225 } };

    BaseDamageFn Table(std::map<std::uint32_t, double> const& t)
    {
        return [&t](std::uint32_t level)
        {
            auto const it = t.find(level);
            CHECK(it != t.end());
            return it == t.end() ? 0.0 : it->second;
        };
    }

    RefHealth::Table const& Rows()
    {
        static RefHealth::Table t = []
        {
            RefHealth::Table table;
            std::string err;
            RefHealth::ParseTable(kRows, table, err);
            return table;
        }();
        return t;
    }

    EffectData Fx(std::uint32_t effect, std::uint32_t aura, std::int32_t bp, std::int32_t die, float ppl = 0.f)
    {
        EffectData e;
        e.effect = effect;
        e.aura = aura;
        e.basePoints = bp;
        e.dieSides = die;
        e.pointsPerLevel = ppl;
        return e;
    }

    SpellData Spell(std::uint32_t id, std::uint32_t spellLevel, std::uint32_t baseLevel, std::uint32_t maxLevel,
        bool scales, std::initializer_list<EffectData> effects)
    {
        SpellData s;
        s.id = id;
        s.spellLevel = spellLevel;
        s.baseLevel = baseLevel;
        s.maxLevel = maxLevel;
        s.scalesWithCreatureLevel = scales;
        std::size_t i = 0;
        for (EffectData const& e : effects)
            s.effects[i++] = e;
        return s;
    }

    constexpr std::uint32_t kApply = 6;
    constexpr std::uint32_t kPersistentArea = 27;

    // Heroic Deadmines: level 60 caster, normal template range lo-hi.
    Scenario Deadmines(std::uint32_t lo, std::uint32_t hi, std::map<std::uint32_t, double> const& bd)
    {
        Scenario sc;
        sc.heroicLevel = 60;
        sc.heroicBaseDamage = Table(bd);
        sc.normalMinLevel = lo;
        sc.normalMaxLevel = hi;
        sc.normalBaseDamage = Table(bd);
        sc.wantedRatio = [](std::uint32_t level) { return ReferenceRatio(Rows(), 60, level); };
        sc.weaponAverage = 400.0;
        sc.targetMaxHealth = 4120.0;
        return sc;
    }

    double FactorOf(SpellData const& s, Group g, Scenario const& sc)
    {
        return GroupFactor(s, s, g, sc).factor;
    }

    // ----------------------------------------------------------------------------------------------------------------

    void TestReferenceRatio()
    {
        CHECK_NEAR(ReferenceRatio(Rows(), 60, 20), kFref, 1e-9);
        CHECK_NEAR(ReferenceRatio(Rows(), 60, 17), kFref, 1e-9);       // clamps to the 20 row
        CHECK_NEAR(ReferenceRatio(Rows(), 60, 60), 4120.0 / 3670.0, 1e-9);
        CHECK_NEAR(ReferenceRatio(Rows(), 60, 40), 4120.0 / 2485.0, 1e-9);   // interpolated normal row
        RefHealth::Table normalOnly;
        std::string err;
        CHECK(RefHealth::ParseTable("20:1300:1290 60:3670:3710", normalOnly, err));
        CHECK_NEAR(ReferenceRatio(normalOnly, 60, 20), 3670.0 / 1300.0, 1e-9);   // no heroic row: normal by level
    }

    void TestMeanEffectValue()
    {
        // Die mean (1 + sides) / 2; 1 side adds 1, 0 sides nothing.
        SpellData flat = Spell(1, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 63, 23), Fx(Effect::SchoolDamage, 0,
            10, 1), Fx(Effect::SchoolDamage, 0, 10, 0) });
        CHECK_NEAR(MeanEffectValue(flat, 0, 20, nullptr), 75.0, 1e-9);
        CHECK_NEAR(MeanEffectValue(flat, 1, 60, nullptr), 11.0, 1e-9);
        CHECK_NEAR(MeanEffectValue(flat, 2, 60, nullptr), 10.0, 1e-9);

        // Points per level: clamped to [BaseLevel, MaxLevel], from max(BaseLevel, SpellLevel), truncated.
        SpellData fireBlast = Spell(2138, 22, 22, 27, false, { Fx(Effect::SchoolDamage, 0, 102, 25, 1.4f) });
        CHECK_NEAR(MeanEffectValue(fireBlast, 0, 19, nullptr), 115.0, 1e-9);   // below BaseLevel: no growth
        CHECK_NEAR(MeanEffectValue(fireBlast, 0, 60, nullptr), 122.0, 1e-9);   // capped at 27: int(5 x 1.4) = 7

        // SCALES_WITH_CREATURE_LEVEL: BaseDamage(caster) / BaseDamage(spell level), only without points per level.
        SpellData shoot = Spell(6660, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 35, 13) });
        CHECK_NEAR(MeanEffectValue(shoot, 0, 60, Table(kWarrior)), 42.0 * 33.9625 / 9.6823, 1e-6);
        CHECK_NEAR(MeanEffectValue(shoot, 0, 20, Table(kWarrior)), 42.0, 1e-9);     // spell level: no ratio
        SpellData scalesPpl = Spell(9, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 35, 13, 1.f) });
        CHECK_NEAR(MeanEffectValue(scalesPpl, 0, 60, Table(kWarrior)), 82.0, 1e-9);  // ppl wins, no ratio
        // An effect type outside canEffectScale does not scale (stun aura), nor one whose trigger scales.
        SpellData stun = Spell(10, 20, 20, 0, true, { Fx(kApply, 12, 5, 0) });
        CHECK_NEAR(MeanEffectValue(stun, 0, 60, Table(kWarrior)), 5.0, 1e-9);
        EffectData trig = Fx(Effect::SchoolDamage, 0, 35, 13);
        trig.triggerScalesWithCreatureLevel = true;
        SpellData triggered = Spell(11, 20, 20, 0, true, { trig });
        CHECK_NEAR(MeanEffectValue(triggered, 0, 60, Table(kWarrior)), 42.0, 1e-9);

        // Potency: + correction per level below the breakpoint, x mean variance (1 + pct / 200).
        EffectData nova = Fx(Effect::SchoolDamage, 0, -42, 1, 7.81714f);
        nova.potency = { true, 5.68371f, 25, 10.f };
        SpellData frostNova = Spell(122, 10, 10, 0, false, { nova });
        CHECK_NEAR(MeanEffectValue(frostNova, 0, 19, nullptr), (29.0 + 5.68371 * 6) * 1.05, 1e-4);
        CHECK_NEAR(MeanEffectValue(frostNova, 0, 60, nullptr), 349.0 * 1.05, 1e-4);
    }

    // The handoff §2.4 Deadmines categories, live spell data, heroic level 60 against each caster's normal range.
    void TestDeadminesCategories()
    {
        // SCALES_WITH_CREATURE_LEVEL, no points per level.
        SpellData shoot = Spell(6660, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 35, 13) });
        GroupResult r = GroupFactor(shoot, shoot, Group::DirectDamage, Deadmines(18, 19, kWarrior));
        CHECK_NEAR(r.factor, 0.8157, 1e-3);           // Goblin Engineer 622 (18-19)
        CHECK_NEAR(r.actual, 147.32, 0.01);
        CHECK_NEAR(r.wanted, 120.18, 0.01);
        CHECK(r.scaled);
        CHECK(!InGroup(shoot, Group::PeriodicDamage));

        SpellData fireball = Spell(9053, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 63, 23) });
        CHECK_NEAR(FactorOf(fireball, Group::DirectDamage, Deadmines(18, 19, kMage)), 0.8442, 1e-3);   // Wizard 4418

        // Flamestrike: direct + periodic (persistent area aura), both scale with level: the same F in each hook.
        SpellData flamestrike = Spell(11829, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 75, 13),
            Fx(kPersistentArea, Aura::PeriodicDamage, 15, 1) });
        CHECK_NEAR(FactorOf(flamestrike, Group::DirectDamage, Deadmines(17, 18, kMage)), 0.7955, 1e-3);
        CHECK_NEAR(FactorOf(flamestrike, Group::PeriodicDamage, Deadmines(17, 18, kMage)), 0.7955, 1e-3);

        SpellData rupture = Spell(14903, 20, 20, 0, true, { Fx(kApply, Aura::PeriodicDamage, 14, 1) });
        CHECK_NEAR(FactorOf(rupture, Group::PeriodicDamage, Deadmines(19, 19, kWarrior)), 0.8341, 1e-3);
        CHECK(!InGroup(rupture, Group::DirectDamage));

        // Points per level, uncapped: the engine grows 40 levels, F trims or tops it up to Fref x normal.
        SpellData moltenMetal = Spell(5213, 20, 20, 0, false, { Fx(kApply, Aura::PeriodicDamage, 29, 1, 1.5f),
            Fx(kApply, 33, -51, 1), Fx(kApply, 138, -55, 1) });
        r = GroupFactor(moltenMetal, moltenMetal, Group::PeriodicDamage, Deadmines(20, 20, kWarrior));
        CHECK_NEAR(r.actual, 90.0, 1e-9);
        CHECK_NEAR(r.wanted, 30.0 * kFref, 1e-6);
        CHECK_NEAR(r.factor, 1.0564, 1e-3);
        SpellData acidSplash = Spell(6306, 20, 20, 0, false, { Fx(kApply, Aura::PeriodicDamage, 49, 1, 1.6f) });
        CHECK_NEAR(FactorOf(acidSplash, Group::PeriodicDamage, Deadmines(20, 20, kWarrior)), 1.3900, 1e-3);

        // Capped: stock Frost Nova (MaxLevel 15) grows nothing past 15, so F = Fref.
        SpellData novaStock = Spell(122, 10, 10, 15, false, { Fx(Effect::SchoolDamage, 0, 18, 3, 0.5f),
            Fx(kApply, 26, -1, 1) });
        CHECK_NEAR(FactorOf(novaStock, Group::DirectDamage, Deadmines(19, 19, kMage)), kFref, 1e-9);
        // Live Frost Nova (the reworked player spell: uncapped 7.8 per level + potency row) grows 5.5x: F 0.573.
        EffectData nova = Fx(Effect::SchoolDamage, 0, -42, 1, 7.81714f);
        nova.potency = { true, 5.68371f, 25, 10.f };
        SpellData novaLive = Spell(122, 10, 10, 0, false, { nova, Fx(kApply, 26, -1, 1) });
        CHECK_NEAR(FactorOf(novaLive, Group::DirectDamage, Deadmines(19, 19, kMage)), 0.5730, 1e-3);
        SpellData fireBlast = Spell(2138, 22, 22, 27, false, { Fx(Effect::SchoolDamage, 0, 102, 25, 1.4f) });
        CHECK_NEAR(FactorOf(fireBlast, Group::DirectDamage, Deadmines(19, 19, kMage)), 2.9874, 1e-3);

        // Flat: F = Fref.
        SpellData slam = Spell(6304, 20, 0, 0, false, { Fx(kApply, 12, 0, 0), Fx(Effect::SchoolDamage, 0, 63, 23) });
        CHECK_NEAR(FactorOf(slam, Group::DirectDamage, Deadmines(19, 19, kWarrior)), kFref, 1e-9);
        // Poisoned Harpoon: a flat DoT (F = Fref in the periodic hook) plus a weapon hit (F = 1 in the direct hook).
        SpellData harpoon = Spell(5208, 21, 21, 0, false, { Fx(kApply, Aura::PeriodicDamage, 44, 1),
            Fx(Effect::WeaponDamage, 0, 29, 1) });
        CHECK_NEAR(FactorOf(harpoon, Group::PeriodicDamage, Deadmines(20, 20, kWarrior)), kFref, 1e-9);
        r = GroupFactor(harpoon, harpoon, Group::DirectDamage, Deadmines(20, 20, kWarrior));
        CHECK(r.factor == 1.0);
        CHECK(!r.scaled);

        // Weapon damage: skipped (the template's melee already scales).
        SpellData axeToss = Spell(6466, 18, 18, 18, false, { Fx(Effect::WeaponDamage, 0, 61, 17), Fx(kApply, 12, 0,
            0) });
        CHECK(FactorOf(axeToss, Group::DirectDamage, Deadmines(18, 19, kWarrior)) == 1.0);
        SpellData cleave = Spell(40505, 10, 10, 0, false, { Fx(Effect::WeaponPercentDamage, 0, 109, 1) });
        CHECK(FactorOf(cleave, Group::DirectDamage, Deadmines(18, 19, kWarrior)) == 1.0);
        SpellData normalized = Spell(12, 20, 20, 0, true, { Fx(Effect::NormalizedWeaponDmg, 0, 50, 1) });
        CHECK(FactorOf(normalized, Group::DirectDamage, Deadmines(18, 19, kWarrior)) == 1.0);

        // Percent of max health: skipped.
        SpellData percent = Spell(13, 20, 20, 0, false, { Fx(kApply, Aura::PeriodicDamagePercent, 4, 1) });
        CHECK(FactorOf(percent, Group::PeriodicDamage, Deadmines(18, 19, kWarrior)) == 1.0);
    }

    void TestMixedEffects()
    {
        // School damage + weapon damage in one Spell: the direct hook gets the sum, so F is the weighted blend.
        Scenario sc = Deadmines(20, 20, kWarrior);
        sc.weaponAverage = 300.0;
        SpellData mixed = Spell(14, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 99, 1),
            Fx(Effect::WeaponDamage, 0, 49, 1) });
        GroupResult r = GroupFactor(mixed, mixed, Group::DirectDamage, sc);
        CHECK_NEAR(r.actual, 100.0 + 350.0, 1e-9);
        CHECK_NEAR(r.wanted, 100.0 * kFref + 350.0, 1e-6);
        CHECK_NEAR(r.factor, (100.0 * kFref + 350.0) / 450.0, 1e-9);

        // Weapon percent multiplies the weapon part (with its fixed bonus).
        SpellData pct = Spell(15, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 99, 1),
            Fx(Effect::WeaponDamage, 0, 49, 1), Fx(Effect::WeaponPercentDamage, 0, 149, 1) });
        r = GroupFactor(pct, pct, Group::DirectDamage, sc);
        CHECK_NEAR(r.actual, 100.0 + 350.0 * 1.5, 1e-9);

        // A periodic damage and a periodic percent effect: blended over both (the percent part stays).
        SpellData dotAndPct = Spell(16, 20, 20, 0, false, { Fx(kApply, Aura::PeriodicDamage, 99, 1),
            Fx(kApply, Aura::PeriodicDamagePercent, 4, 1) });
        r = GroupFactor(dotAndPct, dotAndPct, Group::PeriodicDamage, sc);
        CHECK_NEAR(r.actual, 100.0 + 0.05 * 4120.0, 1e-9);
        CHECK_NEAR(r.wanted, 100.0 * kFref + 0.05 * 4120.0, 1e-6);

        // Health leech is direct damage; power burn counts unscaled.
        SpellData leech = Spell(17, 20, 20, 0, false, { Fx(Effect::HealthLeech, 0, 99, 1) });
        CHECK_NEAR(FactorOf(leech, Group::DirectDamage, sc), kFref, 1e-9);
        SpellData burn = Spell(18, 20, 20, 0, false, { Fx(Effect::PowerBurn, 0, 99, 1) });
        CHECK(FactorOf(burn, Group::DirectDamage, sc) == 1.0);

        // A proc-trigger-damage aura's hit is direct and scales.
        SpellData thorns = Spell(19, 20, 20, 0, false, { Fx(kApply, Aura::ProcTriggerDamage, 9, 1) });
        CHECK_NEAR(FactorOf(thorns, Group::DirectDamage, sc), kFref, 1e-9);

        // Nothing in the group: F = 1, not 0.
        CHECK(FactorOf(thorns, Group::PeriodicDamage, sc) == 1.0);
        SpellData nothing = Spell(20, 20, 20, 0, false, { Fx(kApply, 12, 0, 0) });
        CHECK(FactorOf(nothing, Group::DirectDamage, sc) == 1.0);
    }

    void TestWeaponPartIsThePerHitDamage()
    {
        // A heroic boss (warrior, level 60, DamageModifier 20.68, 2.0 s swing, BaseVariance 1) casts a flat school
        // damage 50 + weapon damage +10 spell from its level 20 normal self. The weapon part is what the engine rolls
        // (Creature::CalculateMinMaxDamage): (weapon + AP / 14) x DamageModifier x swing, weapon 1x to 1.5x base.
        double const base = kWarrior.at(60);
        double const apPart = 252.0 / 14.0;
        double const minHit = (base + apPart) * 20.68 * 2.0;
        double const maxHit = (1.5 * base + apPart) * 20.68 * 2.0;
        double const weapon = MeanWeaponDamage(minHit, maxHit);
        CHECK_NEAR(weapon, 2500.3, 0.1);
        CHECK(MeanWeaponDamage(-5.0, 10.0) == 5.0);    // the engine clamps a negative roll to 0

        Scenario sc = Deadmines(20, 20, kWarrior);
        sc.weaponAverage = weapon;
        SpellData strike = Spell(28, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 49, 1),
            Fx(Effect::WeaponDamage, 0, 9, 1) });
        GroupResult r = GroupFactor(strike, strike, Group::DirectDamage, sc);
        CHECK_NEAR(r.actual, 50.0 + weapon + 10.0, 1e-9);
        // The landed hit (about 2,560) becomes the normal school part x Fref plus the heroic weapon part: ~2,669.
        double const landed = r.actual * r.factor;
        CHECK_NEAR(landed, 50.0 * kFref + weapon + 10.0, 1e-6);
        CHECK_NEAR(landed, 2668.8, 0.1);

        // With the bare base weapon (1.25 x 33.96 = 42) as the weapon part F would be ~2.06 and the hit ~5,280.
        sc.weaponAverage = 1.25 * base;
        CHECK(GroupFactor(strike, strike, Group::DirectDamage, sc).factor > 2.0);
    }

    void TestHeals()
    {
        // Cookie's Cooking 5174 (flat heal 110 + d19) on a creature whose heroic template has 12.74x the health.
        Scenario sc = Deadmines(20, 20, kWarrior);
        double const ratio = 37000.0 / 2904.0;
        sc.wantedRatio = [ratio](std::uint32_t) { return ratio; };
        sc.targetMaxHealth = 37000.0;
        SpellData cooking = Spell(5174, 26, 26, 0, false, { Fx(Effect::Heal, 0, 110, 19) });
        CHECK_NEAR(FactorOf(cooking, Group::DirectHeal, sc), ratio, 1e-9);
        CHECK(!InGroup(cooking, Group::PeriodicHeal));
        CHECK(!InGroup(cooking, Group::DirectDamage));

        // A HoT scales in the periodic heal group; a percent heal does not.
        SpellData hot = Spell(21, 20, 20, 0, false, { Fx(kApply, Aura::PeriodicHeal, 49, 1) });
        CHECK_NEAR(FactorOf(hot, Group::PeriodicHeal, sc), ratio, 1e-9);
        CHECK(!InGroup(hot, Group::PeriodicDamage));
        SpellData pctHeal = Spell(22, 20, 20, 0, false, { Fx(Effect::HealPct, 0, 9, 1) });
        CHECK(FactorOf(pctHeal, Group::DirectHeal, sc) == 1.0);
        SpellData obs = Spell(23, 20, 20, 0, false, { Fx(kApply, Aura::ObsModHealth, 4, 1) });
        CHECK(FactorOf(obs, Group::PeriodicHeal, sc) == 1.0);
        // Regrowth shape (direct heal + HoT): each hook reads its own effect.
        SpellData regrowth = Spell(24, 20, 20, 0, false, { Fx(Effect::Heal, 0, 99, 1), Fx(kApply, Aura::PeriodicHeal,
            9, 1) });
        CHECK_NEAR(GroupFactor(regrowth, regrowth, Group::DirectHeal, sc).actual, 100.0, 1e-9);
        CHECK_NEAR(GroupFactor(regrowth, regrowth, Group::PeriodicHeal, sc).actual, 10.0, 1e-9);
        // A leech's heal back (no heal effect) is never scaled twice.
        SpellData leech = Spell(25, 20, 20, 0, false, { Fx(Effect::HealthLeech, 0, 99, 1) });
        CHECK(FactorOf(leech, Group::DirectHeal, sc) == 1.0);

        // Cookie on heroic: 2,904 health on normal (basehp0 484 x 6), 330,000 on heroic, ratio 113.6. A heal keeps its
        // share of the health uncapped (the damage rail is 20x): 120 -> ~13,636 per cast, ~4.1% as on normal.
        double const cookieRatio = 330000.0 / 2904.0;
        sc.wantedRatio = [cookieRatio](std::uint32_t) { return cookieRatio; };
        sc.targetMaxHealth = 330000.0;
        GroupResult r = GroupFactor(cooking, cooking, Group::DirectHeal, sc);
        CHECK_NEAR(r.factor, cookieRatio, 1e-9);
        CHECK(r.factor > kMaxFactor);
        CHECK_NEAR(r.actual * r.factor, 13636.4, 0.1);
        CHECK_NEAR(r.actual * r.factor / 330000.0, 120.0 / 2904.0, 1e-9);
        CHECK(ScaleAmount(std::uint32_t(120), r.factor) == 13636);
        CHECK_NEAR(FactorOf(hot, Group::PeriodicHeal, sc), cookieRatio, 1e-9);     // a HoT too
    }

    void TestHealTickMarker()
    {
        int player = 0;
        int creature = 0;
        int spell = 0;
        int other = 0;
        std::uint32_t tickAmount = 0;
        std::uint32_t healAmount = 0;
        HealTickMarker m;

        // The periodic heal tick: (target, caster) marked, then consumed by the ModifyHealReceived on the same amount.
        m.Mark(&creature, &other, &spell, &tickAmount);
        CHECK(m.Consume(&creature, &other, &spell, &tickAmount));
        CHECK(!m.Consume(&creature, &other, &spell, &tickAmount));    // consumed once

        // A spell with both a periodic heal and a periodic damage aura marks on its damage tick (the player); no heal
        // follows. A direct heal of the same spell later (HealBySpell: healer, target swapped) never matches.
        m.Mark(&player, &creature, &spell, &tickAmount);
        CHECK(!m.Consume(&creature, &player, &spell, &healAmount));
        m.Mark(&player, &creature, &spell, &tickAmount);
        CHECK(!m.Consume(&player, &creature, &spell, &healAmount));   // even with the units in tick order

        // Any damage hook clears a stale mark before it can be read.
        m.Mark(&creature, &other, &spell, &tickAmount);
        m.Clear();
        CHECK(!m.Consume(&creature, &other, &spell, &tickAmount));

        // Another spell or another caster is not the tick.
        m.Mark(&creature, &other, &spell, &tickAmount);
        CHECK(!m.Consume(&creature, &other, &other, &tickAmount));
        m.Mark(&creature, &other, &spell, &tickAmount);
        CHECK(!m.Consume(&creature, &player, &spell, &tickAmount));
        // An empty mark never matches, even null against null.
        CHECK(!m.Consume(nullptr, nullptr, nullptr, nullptr));
    }

    void TestSwaps()
    {
        // Real rows (stock SpellDifficulty.dbc 1263, 1321 and spelldifficulty_dbc overlay rows).
        std::vector<SpellDifficultyRow> rows = {
            { 12167, { 12167, 59863, 0, 0 } },
            { 12471, { 12471, 15232, 0, 0 } },
            { 12548, { 12548, 21401, 0, 0 } },
            { 14032, { 14032, 15654, 0, 0 } },
            { 15654, { 15654, 59864, 0, 0 } },
            { 21401, { 21401, 46180, 0, 0 } },
            { 8374, { 8374, 38761, 0, 0 } },
            { 1263, { 20203, 68969, 0, 0 } },
            { 6, { 50864, 69848, 69849, 69850 } },
            { 7, { 0, 1, 0, 0 } },              // slot 0 empty: ignored, as DBCStores does
        };
        SpellDifficultyIndex index;
        index.Build(rows);
        CHECK(index.Size() == 9);

        // The engine's forward swap, chains not followed (14032 -> 15654, not 59864).
        CHECK(index.SwapFor(12167, 1) == 59863);
        CHECK(index.SwapFor(14032, 1) == 15654);
        CHECK(index.SwapFor(15654, 1) == 59864);
        CHECK(index.SwapFor(15654, 0) == 15654);
        CHECK(index.SwapFor(6660, 1) == 6660);
        CHECK(index.SwapFor(12167, 3) == 59863);            // raid 25H with no slot 3 -> slot 1
        CHECK(index.SwapFor(50864, 3) == 69850);
        CHECK(index.SwapFor(12167, 2) == 12167);            // slot 2 empty -> slot 0

        SpellDifficultyIndex::Source s = index.SourceOf(59863, 1);
        CHECK(s.swapped && s.spellId == 12167 && !s.ambiguous);
        // 15654 on heroic only ever comes from 14032 (a native 15654 cast becomes 59864): unambiguous.
        s = index.SourceOf(15654, 1);
        CHECK(s.swapped && s.spellId == 14032 && !s.ambiguous);
        // 15232 is 12471's heroic swap and also a spell some creatures cast as themselves (it stays 15232): always
        // read as the swap, rescaled against 12471.
        s = index.SourceOf(15232, 1);
        CHECK(s.swapped && s.spellId == 12471 && !s.ambiguous);
        // Two normal spells swapping to one heroic id: ambiguous, the lowest wins.
        SpellDifficultyIndex twoSources;
        twoSources.Build({ { 100, { 100, 300, 0, 0 } }, { 200, { 200, 300, 0, 0 } } });
        s = twoSources.SourceOf(300, 1);
        CHECK(s.swapped && s.spellId == 100 && s.ambiguous);
        // Not swapped.
        s = index.SourceOf(6660, 1);
        CHECK(!s.swapped && s.spellId == 6660);
        s = index.SourceOf(59863, 0);
        CHECK(!s.swapped && s.spellId == 59863);

        // Lightning Bolt 12167 (scales) cast by a level 58-59 caster swaps to 59863 (flat 3824 + d1351 = 4500 mean)
        // on heroic: rescaled against the normal spell, F brings it back to Fref x the normal bolt (~432).
        Scenario sc;
        sc.heroicLevel = 60;
        sc.heroicBaseDamage = Table(kMage);
        sc.normalMinLevel = 58;
        sc.normalMaxLevel = 59;
        sc.normalBaseDamage = Table(kMage);
        sc.wantedRatio = [](std::uint32_t level) { return ReferenceRatio(Rows(), 60, level); };
        sc.targetMaxHealth = 4120.0;
        SpellData bolt = Spell(12167, 20, 20, 0, true, { Fx(Effect::SchoolDamage, 0, 95, 33) });
        SpellData boltHeroic = Spell(59863, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 3824, 1351) });
        GroupResult r = GroupFactor(boltHeroic, bolt, Group::DirectDamage, sc);
        CHECK_NEAR(r.actual, 4500.0, 1e-9);
        CHECK_NEAR(r.wanted, 431.61, 0.05);
        CHECK_NEAR(r.factor, 0.0959, 1e-3);

        // Arcing Smash 8374 (weapon damage -1+1 = +0) -> 38761 (weapon 75 + d451 percent, ~301%): undone to 1x weapon.
        sc.weaponAverage = 500.0;
        SpellData smash = Spell(8374, 1, 1, 0, false, { Fx(Effect::WeaponDamage, 0, -1, 1) });
        SpellData smashHeroic = Spell(38761, 1, 1, 0, false, { Fx(Effect::WeaponPercentDamage, 0, 74, 451) });
        r = GroupFactor(smashHeroic, smash, Group::DirectDamage, sc);
        CHECK_NEAR(r.actual, 500.0 * 3.0, 1e-9);
        CHECK_NEAR(r.wanted, 500.0, 1e-9);
        CHECK_NEAR(r.factor, 1.0 / 3.0, 1e-9);

        // A damage factor never leaves [0, kMaxFactor]; a heal's is not capped.
        SpellData tiny = Spell(26, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 0, 1) });
        SpellData huge = Spell(27, 20, 20, 0, false, { Fx(Effect::SchoolDamage, 0, 9999, 1) });
        CHECK(GroupFactor(tiny, huge, Group::DirectDamage, sc).factor == kMaxFactor);
        SpellData tinyHeal = Spell(29, 20, 20, 0, false, { Fx(Effect::Heal, 0, 0, 1) });
        SpellData hugeHeal = Spell(30, 20, 20, 0, false, { Fx(Effect::Heal, 0, 9999, 1) });
        sc.wantedRatio = [](std::uint32_t) { return 1.0; };
        CHECK_NEAR(GroupFactor(tinyHeal, hugeHeal, Group::DirectHeal, sc).factor, 10000.0, 1e-9);
    }

    void TestGate()
    {
        Settings s;
        s.enabled = true;
        std::string err;
        CHECK(ParseIdList("36", s.heroicMaps, err));
        CHECK(ParseIdList("5208", s.exceptions, err));

        GateInputs in;
        in.kind = Kind::Damage;
        in.hasCaster = true;
        in.casterIsCreature = true;
        in.dungeon = true;
        in.heroic = true;
        in.mapId = 36;
        in.castSpellId = 9053;
        in.sourceSpellId = 9053;
        in.targetControlledByPlayer = true;
        CHECK(Check(s, in) == Verdict::NoHeroicTemplate);     // default: require a heroic template
        GateInputs noTpl = in;
        in.casterHasHeroicTemplate = true;
        CHECK(Check(s, in) == Verdict::Act);
        Settings anyCaster = s;
        anyCaster.requireHeroicTemplate = false;              // the playtest switch
        CHECK(Check(anyCaster, noTpl) == Verdict::Act);
        GateInputs normalMap = noTpl;
        normalMap.heroic = false;
        CHECK(Check(s, normalMap) == Verdict::NotHeroic);     // checked before the template
        noTpl.kind = Kind::Heal;
        noTpl.targetIsCreature = true;
        noTpl.targetControlledByPlayer = false;
        CHECK(Check(s, noTpl) == Verdict::NoHeroicTemplate);  // heals too
        CHECK(Check(anyCaster, noTpl) == Verdict::Act);
        CHECK(std::string(VerdictName(Verdict::NoHeroicTemplate)) == "caster has no heroic template");

        GateInputs x = in;
        x.heroic = false;
        CHECK(Check(s, x) == Verdict::NotHeroic);
        x = in;
        x.mapId = 33;
        CHECK(Check(s, x) == Verdict::NotListedMap);
        x = in;
        x.dungeon = false;
        CHECK(Check(s, x) == Verdict::NotDungeon);
        x = in;
        x.casterControlledByPlayer = true;          // a pet or a charmed mob
        CHECK(Check(s, x) == Verdict::PlayerControlled);
        x = in;
        x.casterIsCreature = false;                 // a player
        CHECK(Check(s, x) == Verdict::NotCreature);
        x = in;
        x.hasCaster = false;                        // a DoT whose caster despawned
        CHECK(Check(s, x) == Verdict::NoCaster);
        x = in;
        x.castSpellId = x.sourceSpellId = 205003;   // a mechanics resolve spell
        CHECK(Check(s, x) == Verdict::CustomSpell);
        x.castSpellId = x.sourceSpellId = 199999;
        CHECK(Check(s, x) == Verdict::Act);
        x = in;
        x.castSpellId = x.sourceSpellId = 5208;
        CHECK(Check(s, x) == Verdict::Exception);
        x.castSpellId = 99999;                      // a swap whose source is excepted
        CHECK(Check(s, x) == Verdict::Exception);
        x = in;
        x.targetControlledByPlayer = false;         // a creature hitting a creature
        CHECK(Check(s, x) == Verdict::TargetNotPlayer);
        x = in;
        x.kind = Kind::Heal;
        x.targetControlledByPlayer = false;
        x.targetIsCreature = true;
        CHECK(Check(s, x) == Verdict::Act);
        x.targetControlledByPlayer = true;          // a creature healing a player's pet
        CHECK(Check(s, x) == Verdict::TargetNotCreature);
        x.targetIsCreature = false;
        x.targetControlledByPlayer = true;
        CHECK(Check(s, x) == Verdict::TargetNotCreature);
        Settings off = s;
        off.enabled = false;
        CHECK(Check(off, in) == Verdict::Disabled);
        CHECK(std::string(VerdictName(Verdict::Act)) == "scaled");
    }

    void TestScaleAmount()
    {
        CHECK(ScaleAmount(std::uint32_t(100), 0.8157) == 82);
        CHECK(ScaleAmount(std::uint32_t(0), 3.0) == 0);
        CHECK(ScaleAmount(std::int32_t(-5), 3.0) == -5);
        CHECK(ScaleAmount(std::int32_t(100), 3.1692) == 317);
        CHECK(ScaleAmount(std::uint32_t(4000000000u), 20.0) == 4294967295u);
        CHECK(ScaleAmount(std::int32_t(2000000000), 20.0) == 2147483647);
    }

    void TestConfigParsing()
    {
        std::set<std::uint32_t> ids;
        std::string err;
        CHECK(ParseIdList("", ids, err) && ids.empty());
        CHECK(ParseIdList("36 33, 289;36", ids, err));
        CHECK(ids.size() == 3 && ids.count(33) && ids.count(36) && ids.count(289));
        CHECK(FormatIdList(ids) == "33 36 289");
        CHECK(!ParseIdList("36 x", ids, err));
        CHECK(ids.empty() && err.find("'x'") != std::string::npos);
        CHECK(!ParseIdList("-36", ids, err));
        CHECK(!ParseIdList("4294967296", ids, err));

        // The reference health string is BM's format: the same paste works in both modules.
        RefHealth::Table t;
        CHECK(RefHealth::ParseTable(kRows, t, err));
        CHECK(RefHealth::Format(t) == kRows);
        CHECK(t.normal.size() == 2 && t.heroic.size() == 1);
        CHECK(RefHealth::ParseTable("20:1000:1500 60:5000:8000", t, err));     // the old stub string
        CHECK(t.heroic.empty());
        CHECK(RefHealth::ParseTable("20:1300:1290 60h:4120:4110 60e1:5000:5000", t, err));
        CHECK(t.empowered.size() == 1);
        CHECK(!RefHealth::ParseTable("60h:4120:4110", t, err));                 // no normal row
        CHECK(!RefHealth::ParseTable("20:1300:1290 20:1:1", t, err));            // duplicate
        CHECK(!RefHealth::ParseTable("20:1300", t, err));
        CHECK(!RefHealth::ParseTable("20:0:1290", t, err));
        CHECK(!RefHealth::ParseTable("20x:1300:1290", t, err));
        CHECK(!RefHealth::ParseTable("256:1300:1290", t, err));
        ContentKey k;
        k.level = 40;
        CHECK(RefHealth::ParseTable(kRows, t, err));
        CHECK(RefHealth::Lookup(t, k).nonTank == 2485);
        k.heroic = true;
        CHECK(RefHealth::Lookup(t, k).nonTank == 4120);
    }
}

int main()
{
    TestReferenceRatio();
    TestMeanEffectValue();
    TestDeadminesCategories();
    TestMixedEffects();
    TestWeaponPartIsThePerHitDamage();
    TestHeals();
    TestHealTickMarker();
    TestSwaps();
    TestGate();
    TestScaleAmount();
    TestConfigParsing();
    return Report("heroic_spell_test");
}
