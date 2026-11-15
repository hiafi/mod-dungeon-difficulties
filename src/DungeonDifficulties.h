/*
 * mod-dungeon-difficulties: module-wide declarations (config, the heroic spell scaling glue, the flex scaler's command
 * views). Boss scripts in other modules include FlexScale.h, not this.
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_H
#define MOD_DUNGEON_DIFFICULTIES_H

#include "FlexMath.h"
#include "HeroicSpellMath.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Creature;
class Map;
class SpellInfo;
class Unit;

namespace DungeonDifficulties
{
    constexpr char const* kLog = "module.dungeondifficulties";
    // The E3 measurement (2026-11-05), the same string as BossMechanics.ReferenceHealth.Rows.
    constexpr char const* kDefaultReferenceHealthRows = "20:1300:1290 60:3670:3710 60h:4120:4110";
    constexpr char const* kDefaultHeroicMaps = "36";
    // No flex map unless configured: a flex map must also leave AutoBalance (AutoBalance.Disable.PerInstance), so the
    // map goes live with the conf line, never with the module build.
    constexpr char const* kDefaultFlexMaps = "";

    struct FlexConfig
    {
        bool enabled = true;
        std::set<std::uint32_t> maps;
        std::string mapsText = kDefaultFlexMaps;
        FlexMath::Settings settings;
        bool debug = false;
    };

    struct ModuleConfig
    {
        Heroic::Settings heroic;
        std::string heroicMapsText = kDefaultHeroicMaps;
        std::string exceptionsText;
        std::string referenceHealthText = kDefaultReferenceHealthRows;
        RefHealth::Table referenceHealth;
        bool debug = false;
        FlexConfig flex;
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

    // FlexScale.cpp: read-only views for `.dd flex` (the API for boss scripts is FlexScale.h).
    namespace Flex
    {
        struct MapView
        {
            bool flexMap = false;
            std::uint32_t players = 0;              // on the map, GMs excluded
            std::uint8_t freshCount = 0;            // players clamped: what a pull with no hold to inherit would read
            std::uint8_t overrideCount = 0;
            std::uint8_t encounterCount = 0;        // 0 = no live encounter
            std::vector<ObjectGuid> encounterOwners;
            std::vector<ObjectGuid> held;
        };

        struct CreatureView
        {
            bool holdable = false;                  // on a flex map, alive, no player controls it
            FlexMath::Hold hold;
            std::uint64_t heldForMs = 0;
            FlexMath::CountResult preview;          // not held: what a pull now would hold
        };

        MapView ViewMap(Map* map);                  // also prunes holds and owners this module missed releasing
        CreatureView ViewCreature(Creature* creature);
        bool SetOverride(Map* map, std::uint8_t count);   // false off a flex map; 0 clears
    }
}

#endif
