/*
 * mod-dungeon-difficulties: GM commands (SEC_GAMEMASTER, in game only).
 *
 *   .dd status            config, heroic maps, index sizes
 *   .dd factor <spell>    the heroic factor of <spell> (id or link) cast by the selected creature: gate verdict here,
 *                         SpellDifficulty source, normal levels, Fref, and F per group (damage groups against you,
 *                         heal groups on the creature itself). Off a listed heroic map it still computes F, as a
 *                         preview of what the rule would do with this creature's current template and level.
 *   .dd flex              the flex scaler on this instance: players, the count a fresh pull holds, the GM override,
 *                         the live encounter and every held creature; with a creature selected, its hold (count,
 *                         source, base and max health, the target / loot / tank swap rules at its count) or the count
 *                         a pull would hold now. Read only.
 *   .dd flex set <n>      test override for this instance: pulls with nothing to inherit hold <n> (clamped to the
 *                         flex range); 0 clears. Lives with the instance (cleared on reset); held fights keep theirs.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "DungeonDifficulties.h"
#include "FlexScale.h"
#include "Map.h"
#include "Player.h"
#include "SpellInfo.h"

using namespace Acore::ChatCommands;
using namespace DungeonDifficulties;

class dungeon_difficulties_commandscript : public CommandScript
{
public:
    dungeon_difficulties_commandscript() : CommandScript("dungeon_difficulties_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable flexTable =
        {
            { "", HandleFlex, SEC_GAMEMASTER, Console::No },
            { "set", HandleFlexSet, SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable ddTable =
        {
            { "status", HandleStatus, SEC_GAMEMASTER, Console::No },
            { "factor", HandleFactor, SEC_GAMEMASTER, Console::No },
            { "flex", flexTable },
        };
        static ChatCommandTable commandTable =
        {
            { "dd", ddTable },
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        ModuleConfig const& cfg = GetConfig();
        handler->PSendSysMessage("Dungeon difficulties: {}; heroic maps '{}'; spell exceptions '{}'; "
            "require heroic template {}; debug {}.", cfg.heroic.enabled ? "enabled" : "DISABLED",
            Heroic::FormatIdList(cfg.heroic.heroicMaps), Heroic::FormatIdList(cfg.heroic.exceptions),
            cfg.heroic.requireHeroicTemplate ? "on" : "off", cfg.debug ? "on" : "off");
        handler->PSendSysMessage("Reference health: {}.", RefHealth::Format(cfg.referenceHealth));
        handler->PSendSysMessage("{} SpellDifficulty rows indexed, {} potency rows.", SpellDifficultyRowCount(),
            PotencyRowCount());
        return true;
    }

    static bool HandleFactor(ChatHandler* handler, SpellInfo const* spell)
    {
        Player* gm = handler->GetSession()->GetPlayer();
        Creature* caster = handler->getSelectedCreature();
        if (!caster || !spell)
        {
            handler->SendErrorMessage("Select a creature: .dd factor <spell id or link>");
            return false;
        }

        Heroic::Group const groups[] = { Heroic::Group::DirectDamage, Heroic::Group::PeriodicDamage,
            Heroic::Group::DirectHeal, Heroic::Group::PeriodicHeal };
        bool any = false;
        bool header = false;
        for (Heroic::Group group : groups)
        {
            bool const heal = group == Heroic::Group::DirectHeal || group == Heroic::Group::PeriodicHeal;
            Evaluation const ev = Evaluate(caster, heal ? static_cast<Unit*>(caster) : static_cast<Unit*>(gm), spell,
                group, true);
            if (!header)
            {
                header = true;
                handler->PSendSysMessage("{} (level {}, normal {}-{}) casting {} {}{}: here {}.", caster->GetName(),
                    ev.heroicLevel, ev.normalMinLevel, ev.normalMaxLevel, spell->Id, spell->SpellName[0],
                    ev.source.swapped ? Acore::StringFormat(", heroic swap of {}{}", ev.source.spellId,
                        ev.source.ambiguous ? " (ambiguous)" : "") : "", Heroic::VerdictName(ev.verdict));
                handler->PSendSysMessage("Fref (non-tank reference health ratio) {:.3f} at the normal min level, "
                    "{:.3f} at the max.", ev.refRatioLow, ev.refRatioHigh);
            }
            if (!ev.result.scaled && ev.result.wanted == 0.0 && ev.result.actual == 0.0)
                continue;
            any = true;
            handler->PSendSysMessage("  {}: F {:.3f} (wanted {:.1f}, engine {:.1f}){}; verdict {}.", GroupName(group),
                ev.result.factor, ev.result.wanted, ev.result.actual,
                heal ? Acore::StringFormat(", heal ratio {:.3f}", ev.refRatioLow) : "",
                Heroic::VerdictName(ev.verdict));
        }
        if (!any)
            handler->PSendSysMessage("  No damage or heal effect: nothing to scale.");
        return true;
    }

    static std::string RulesAt(uint8 count)
    {
        return Acore::StringFormat("x{:.2f} health, {} target counts, {} loot roll{}, tank swap {}",
            Flex::HealthLadder(count), Flex::TargetCount(count, 0, 1) ? "doubled" : "normal", Flex::LootRolls(count),
            Flex::LootRolls(count) == 1 ? "" : "s", Flex::TankSwap(count) ? "on" : "off");
    }

    static void ShowCreature(ChatHandler* handler, Creature* creature)
    {
        Flex::CreatureView const v = Flex::ViewCreature(creature);
        if (!v.holdable)
        {
            handler->PSendSysMessage("{} ({}): not scaled here (not on a flex map, dead, or player controlled).",
                creature->GetName(), creature->GetEntry());
            return;
        }
        if (v.hold.Held())
        {
            handler->PSendSysMessage("{} ({}): holds {} ({}) for {:.1f} s; max health {} = base {} x {:.2f}.",
                creature->GetName(), creature->GetEntry(), v.hold.count, FlexMath::CountSourceName(v.hold.source),
                v.heldForMs / 1000.0, creature->GetMaxHealth(), v.hold.base, Flex::HealthLadder(v.hold.count));
            handler->PSendSysMessage("  At {}: {}.", v.hold.count, RulesAt(v.hold.count));
            return;
        }
        handler->PSendSysMessage("{} ({}): not held; a pull now would hold {} ({}): {}. Max health now {}{}.",
            creature->GetName(), creature->GetEntry(), v.preview.count, FlexMath::CountSourceName(v.preview.source),
            RulesAt(v.preview.count), creature->GetMaxHealth(),
            v.hold.base ? Acore::StringFormat(" (stored base {})", v.hold.base) : "");
    }

    static bool HandleFlex(ChatHandler* handler)
    {
        Player* gm = handler->GetSession()->GetPlayer();
        Map* map = gm->GetMap();
        FlexConfig const& f = GetConfig().flex;
        handler->PSendSysMessage("Flex scaler: {}; maps '{}', players {}-{}, health step {:.2f}, debug {}.",
            f.enabled ? "enabled" : "DISABLED", Heroic::FormatIdList(f.maps), f.settings.minPlayers,
            f.settings.maxPlayers, f.settings.healthStep, f.debug ? "on" : "off");

        Flex::MapView const v = Flex::ViewMap(map);
        if (!v.flexMap)
            handler->PSendSysMessage("Map {} is not a flex map here.", map->GetId());
        else
        {
            handler->PSendSysMessage("Map {} instance {}: {} players (no GMs); a fresh pull holds {} ({}). "
                "GM override: {}.", map->GetId(), map->GetInstanceId(), v.players, v.freshCount,
                RulesAt(v.freshCount), v.overrideCount ? std::to_string(v.overrideCount) : std::string("none"));
            if (v.encounterCount)
            {
                std::string owners;
                for (ObjectGuid const& guid : v.encounterOwners)
                    if (Creature* owner = map->GetCreature(guid))
                        owners += (owners.empty() ? "" : ", ") + owner->GetName();
                handler->PSendSysMessage("Encounter live: every pull holds {} (bosses: {}).", v.encounterCount,
                    owners);
            }
            else
                handler->PSendSysMessage("No live encounter.");

            constexpr std::size_t kMaxLines = 15;
            handler->PSendSysMessage("{} held creature{}:", v.held.size(), v.held.size() == 1 ? "" : "s");
            std::size_t shown = 0;
            for (ObjectGuid const& guid : v.held)
            {
                Creature* c = map->GetCreature(guid);
                if (!c)
                    continue;
                if (shown++ == kMaxLines)
                {
                    handler->PSendSysMessage("  ... {} more", v.held.size() - kMaxLines);
                    break;
                }
                handler->PSendSysMessage("  {} ({}): {}, max health {} (base {})", c->GetName(), c->GetEntry(),
                    Flex::HeldCount(c), c->GetMaxHealth(), Flex::BaseHealth(c));
            }
        }

        if (Creature* selected = handler->getSelectedCreature())
            ShowCreature(handler, selected);
        return true;
    }

    static bool HandleFlexSet(ChatHandler* handler, uint32 count)
    {
        Map* map = handler->GetSession()->GetPlayer()->GetMap();
        if (count > FlexMath::kMaxConfigPlayers)
        {
            handler->SendErrorMessage("Usage: .dd flex set <0-{}> (0 clears)", FlexMath::kMaxConfigPlayers);
            return false;
        }
        if (!Flex::SetOverride(map, uint8(count)))
        {
            handler->SendErrorMessage("Map {} is not a flex map (DungeonDifficulties.Flex.Maps).", map->GetId());
            return false;
        }
        if (count)
            handler->PSendSysMessage("Flex override on this instance: pulls with nothing to inherit hold {} ({}). "
                "Fights already held keep their count.", Flex::ClampCount(count), RulesAt(Flex::ClampCount(count)));
        else
            handler->PSendSysMessage("Flex override cleared: pulls count the players again.");
        return true;
    }
};

void AddSC_dungeon_difficulties_command()
{
    new dungeon_difficulties_commandscript();
}
