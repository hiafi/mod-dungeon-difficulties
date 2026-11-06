/*
 * mod-dungeon-difficulties: GM commands (SEC_GAMEMASTER, in game only).
 *
 *   .dd status            config, heroic maps, index sizes
 *   .dd factor <spell>    the heroic factor of <spell> (id or link) cast by the selected creature: gate verdict here,
 *                         SpellDifficulty source, normal levels, Fref, and F per group (damage groups against you,
 *                         heal groups on the creature itself). Off a listed heroic map it still computes F, as a
 *                         preview of what the rule would do with this creature's current template and level.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "DungeonDifficulties.h"
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
        static ChatCommandTable ddTable =
        {
            { "status", HandleStatus, SEC_GAMEMASTER, Console::No },
            { "factor", HandleFactor, SEC_GAMEMASTER, Console::No },
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
};

void AddSC_dungeon_difficulties_command()
{
    new dungeon_difficulties_commandscript();
}
