/*
 * mod-dungeon-difficulties: configuration (conf/mod_dungeon_difficulties.conf.dist). Read at startup and on
 * `.reload config`; the hooks read the cached values only. Flex keys: DungeonDifficulties.Flex.* (FlexScale.h).
 */

#include "Config.h"
#include "DungeonDifficulties.h"
#include "Log.h"

namespace DungeonDifficulties
{
    namespace
    {
        ModuleConfig sConfig;

        void LoadFlexConfig(FlexConfig& f)
        {
            std::string err;
            f.enabled = sConfigMgr->GetOption<bool>("DungeonDifficulties.Flex.Enable", true);
            f.debug = sConfigMgr->GetOption<bool>("DungeonDifficulties.Flex.Debug", false);

            f.mapsText = sConfigMgr->GetOption<std::string>("DungeonDifficulties.Flex.Maps", kDefaultFlexMaps);
            if (!Heroic::ParseIdList(f.mapsText, f.maps, err))
            {
                LOG_WARN(kLog, "DungeonDifficulties.Flex.Maps '{}' is malformed: {}; no flex map", f.mapsText, err);
                f.mapsText = kDefaultFlexMaps;
                f.maps.clear();
            }

            int32 const minPlayers = sConfigMgr->GetOption<int32>("DungeonDifficulties.Flex.MinPlayers",
                FlexMath::kDefaultMinPlayers);
            int32 const maxPlayers = sConfigMgr->GetOption<int32>("DungeonDifficulties.Flex.MaxPlayers",
                FlexMath::kDefaultMaxPlayers);
            float const step = sConfigMgr->GetOption<float>("DungeonDifficulties.Flex.HealthStep",
                float(FlexMath::kDefaultHealthStep));
            if (!FlexMath::ValidateSettings(minPlayers, maxPlayers, step, f.settings, err))
                LOG_WARN(kLog, "DungeonDifficulties.Flex: {}; using MinPlayers {}, MaxPlayers {}, HealthStep {}", err,
                    FlexMath::kDefaultMinPlayers, FlexMath::kDefaultMaxPlayers, FlexMath::kDefaultHealthStep);

            // AutoBalance would stack its own health (and damage) scaling on a flex map. Its keys are read the way
            // mod-autobalance reads them (Enable.Global falls back to the old AutoBalance.enable, default on;
            // PerInstance comma separated); no check when AutoBalance is not loaded (no AutoBalance.* key) or is off.
            bool const autoBalanceLoaded = !sConfigMgr->GetKeysByString("AutoBalance.").empty();
            bool const autoBalanceOn = autoBalanceLoaded && sConfigMgr->GetOption<bool>("AutoBalance.Enable.Global",
                sConfigMgr->GetOption<bool>("AutoBalance.enable", true, false), false);
            if (f.enabled && !f.maps.empty() && autoBalanceOn)
            {
                std::set<uint32> const disabled = FlexMath::ParseAutoBalanceDisabled(
                    sConfigMgr->GetOption<std::string>("AutoBalance.Disable.PerInstance", "", false));
                for (uint32 map : FlexMath::FlexMapsStillAutoBalanced(f.maps, disabled))
                    LOG_WARN(kLog, "DungeonDifficulties.Flex.Maps has map {} but AutoBalance still scales it: add {} "
                        "to AutoBalance.Disable.PerInstance (comma separated)", map, map);
            }
        }
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

        LoadFlexConfig(c.flex);

        sConfig = std::move(c);

        LOG_INFO(kLog, "mod-dungeon-difficulties {}: heroic maps '{}', reference health '{}', spell exceptions '{}', "
            "require heroic template {}{}",
            reload ? "reloaded" : "loaded", Heroic::FormatIdList(sConfig.heroic.heroicMaps),
            RefHealth::Format(sConfig.referenceHealth), Heroic::FormatIdList(sConfig.heroic.exceptions),
            sConfig.heroic.requireHeroicTemplate ? "on" : "off",
            sConfig.heroic.enabled ? "" : " (DISABLED)");
        FlexConfig const& f = sConfig.flex;
        LOG_INFO(kLog, "mod-dungeon-difficulties flex: maps '{}', players {}-{}, health step {:.2f}{}",
            Heroic::FormatIdList(f.maps), f.settings.minPlayers, f.settings.maxPlayers, f.settings.healthStep,
            f.enabled ? "" : " (DISABLED)");
    }
}
