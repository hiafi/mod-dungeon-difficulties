/*
 * mod-dungeon-difficulties: configuration (conf/mod_dungeon_difficulties.conf.dist). Read at startup and on
 * `.reload config`; the hooks read the cached values only.
 */

#include "Config.h"
#include "DungeonDifficulties.h"
#include "Log.h"

namespace DungeonDifficulties
{
    namespace
    {
        ModuleConfig sConfig;
    }

    ModuleConfig const& GetConfig()
    {
        return sConfig;
    }

    void LoadConfig(bool reload)
    {
        ModuleConfig c;
        std::string err;

        c.heroic.enabled = sConfigMgr->GetOption<bool>("DungeonDifficulties.Enable", true);
        c.debug = sConfigMgr->GetOption<bool>("DungeonDifficulties.Debug", false);
        c.heroic.requireHeroicTemplate =
            sConfigMgr->GetOption<bool>("DungeonDifficulties.Heroic.RequireHeroicTemplate", true);

        c.heroicMapsText = sConfigMgr->GetOption<std::string>("DungeonDifficulties.Heroic.Maps", kDefaultHeroicMaps);
        if (!Heroic::ParseIdList(c.heroicMapsText, c.heroic.heroicMaps, err))
        {
            LOG_WARN(kLog, "DungeonDifficulties.Heroic.Maps '{}' is malformed: {}; using '{}'", c.heroicMapsText, err,
                kDefaultHeroicMaps);
            c.heroicMapsText = kDefaultHeroicMaps;
            Heroic::ParseIdList(c.heroicMapsText, c.heroic.heroicMaps, err);
        }

        c.exceptionsText = sConfigMgr->GetOption<std::string>("DungeonDifficulties.Heroic.SpellExceptions", "");
        if (!Heroic::ParseIdList(c.exceptionsText, c.heroic.exceptions, err))
        {
            LOG_WARN(kLog, "DungeonDifficulties.Heroic.SpellExceptions '{}' is malformed: {}; using none",
                c.exceptionsText, err);
            c.exceptionsText.clear();
            c.heroic.exceptions.clear();
        }

        c.referenceHealthText = sConfigMgr->GetOption<std::string>("DungeonDifficulties.ReferenceHealth",
            kDefaultReferenceHealthRows);
        if (!RefHealth::ParseTable(c.referenceHealthText, c.referenceHealth, err))
        {
            LOG_WARN(kLog, "DungeonDifficulties.ReferenceHealth '{}' is malformed: {}; using '{}'",
                c.referenceHealthText, err, kDefaultReferenceHealthRows);
            c.referenceHealthText = kDefaultReferenceHealthRows;
            RefHealth::ParseTable(c.referenceHealthText, c.referenceHealth, err);
        }
        if (c.referenceHealth.heroic.empty())
            LOG_WARN(kLog, "DungeonDifficulties.ReferenceHealth has no heroic (h) row: heroic spells scale by the "
                "normal rows' level ratio only");

        sConfig = std::move(c);

        LOG_INFO(kLog, "mod-dungeon-difficulties {}: heroic maps '{}', reference health '{}', spell exceptions '{}', "
            "require heroic template {}{}",
            reload ? "reloaded" : "loaded", Heroic::FormatIdList(sConfig.heroic.heroicMaps),
            RefHealth::Format(sConfig.referenceHealth), Heroic::FormatIdList(sConfig.heroic.exceptions),
            sConfig.heroic.requireHeroicTemplate ? "on" : "off",
            sConfig.heroic.enabled ? "" : " (DISABLED)");
    }
}
