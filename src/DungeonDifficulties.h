/*
 * mod-dungeon-difficulties: module-wide declarations (config, the heroic spell scaling glue).
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_H
#define MOD_DUNGEON_DIFFICULTIES_H

#include "HeroicSpellMath.h"
#include <string>

class SpellInfo;
class Unit;

namespace DungeonDifficulties
{
    constexpr char const* kLog = "module.dungeondifficulties";
    // The E3 measurement (2026-11-05), the same string as BossMechanics.ReferenceHealth.Rows.
    constexpr char const* kDefaultReferenceHealthRows = "20:1300:1290 60:3670:3710 60h:4120:4110";
    constexpr char const* kDefaultHeroicMaps = "36";

    struct ModuleConfig
    {
        Heroic::Settings heroic;
        std::string heroicMapsText = kDefaultHeroicMaps;
        std::string exceptionsText;
        std::string referenceHealthText = kDefaultReferenceHealthRows;
        RefHealth::Table referenceHealth;
        bool debug = false;
    };

    ModuleConfig const& GetConfig();
    void LoadConfig(bool reload);                   // DungeonDifficultiesConfig.cpp

    // HeroicSpellScaling.cpp
    void LoadSpellData();                           // SpellDifficulty index + spell_potency_correction (startup)
    std::size_t SpellDifficultyRowCount();
    std::size_t PotencyRowCount();

    // One evaluation of the rule, for the hooks and `.dd factor`.
    struct Evaluation
    {
        Heroic::Verdict verdict = Heroic::Verdict::NoCaster;
        Heroic::SpellDifficultyIndex::Source source;
        Heroic::GroupResult result;
        std::uint32_t heroicLevel = 0;
        std::uint32_t normalMinLevel = 0;
        std::uint32_t normalMaxLevel = 0;
        double refRatioLow = 1.0;                   // Fref (or the heal health ratio) at the normal min level
        double refRatioHigh = 1.0;                  // ... and at the normal max level
    };

    // `force` computes the factor even when the gate says no (the command's preview); the verdict is still set.
    Evaluation Evaluate(Unit* caster, Unit* target, SpellInfo const* spellInfo, Heroic::Group group, bool force);

    char const* GroupName(Heroic::Group group);
}

#endif
