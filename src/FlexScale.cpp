/*
 * mod-dungeon-difficulties: the flex 5-10 scaler, engine side (Blackrock Spire build handoff §3.12). The rules and the
 * hold state are pure (FlexMath.h, host-tested); the API is FlexScale.h. Zero core edits, ScriptMgr hooks only:
 *
 * - Pull: UnitScript::OnUnitEnterCombat (Creature::AtEngage, AFTER the AI's JustEngagedWith) -> PullCount. A hold
 *   left by an earlier fight whose evade this module never saw (a script's EnterEvadeMode that skips the core's) is
 *   released first, so the new pull counts again.
 * - Evade: OnUnitEnterEvadeMode (CreatureAI::EnterEvadeMode and SmartAI, after Reset) -> restore the base, drop.
 * - Death: OnUnitDeath (after JustDied) -> restore the base, drop.
 * - Removed from the world: AllCreatureScript::OnCreatureRemoveWorld -> drop (leaves the encounter and the list).
 * - Template change while held (UpdateEntry -> SelectLevel): OnCreatureSelectLevel -> engaged, the new create
 *   health is the new base, scaled by the held count; not engaged (a respawn, an idle UpdateEntry), the hold drops.
 * - Every creature update on a flex map: AllCreatureScript::OnAllCreatureUpdate -> a held creature whose max health
 *   was overwritten alone (mod-autobalance's reset on a player level-up or `.reload config`, which also runs on its
 *   disabled maps) gets the scaled max health back in the same update (AutoBalance's script runs first); a hold left
 *   on an idle creature (alive, not engaged, not evading) is released.
 *
 * State: the hold lives in the creature's CustomData (freed with it); the GM override, the live encounter and the
 * held list (for `.dd flex`) in the map's CustomData (freed with the instance, so the override clears on reset). All
 * of it is touched on the map's thread (hooks) or between map updates (chat commands).
 */

#include "FlexScale.h"
#include "AllCreatureScript.h"
#include "Creature.h"
#include "DBCStructure.h"
#include "DungeonDifficulties.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "TemporarySummon.h"
#include "UnitScript.h"

namespace DungeonDifficulties::Flex
{
    namespace
    {
        // Built once: DataMap::Get takes a std::string, and the release and update hooks look the key up on every
        // creature (a char const* key would build a heap string per call).
        std::string const kCreatureKey = "DungeonDifficultiesFlexHold";
        std::string const kMapKey = "DungeonDifficultiesFlexMap";
        constexpr int kMaxSummonerDepth = 4;    // add of an add of a boss ...; guards a summoner loop

        struct CreatureFlex : DataMap::Base
        {
            FlexMath::Hold hold;
        };

        struct MapFlex : DataMap::Base
        {
            uint8 overrideCount = 0;
            FlexMath::Encounter encounter;
            std::vector<ObjectGuid> held;
        };

        // FlexMath's unit adapter: create health is the base the engine rebuilds max health from (the hard modes'
        // SetCreatureHealth pattern), so max health follows auras through UpdateMaxHealth.
        class CreatureHealth
        {
        public:
            explicit CreatureHealth(Creature* creature) : _c(creature) { }

            uint32 CreateHealth() const { return _c->GetCreateHealth(); }
            uint32 MaxHealth() const { return _c->GetMaxHealth(); }
            uint32 ModMaxHealth() const { return uint32(_c->GetTotalAuraModValue(UNIT_MOD_HEALTH)); }
            uint32 Health() const { return _c->GetHealth(); }

            void SetBaseHealth(uint32 value)
            {
                _c->SetCreateHealth(value);
                _c->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(value));
                _c->UpdateMaxHealth();
            }

            void SetHealth(uint32 value)
            {
                _c->SetHealth(value);
                _c->ResetPlayerDamageReq();
            }

        private:
            Creature* _c;
        };

        FlexConfig const& Cfg()
        {
            return GetConfig().flex;
        }

        FlexMath::Hold const* HoldOf(Creature const* creature)
        {
            if (CreatureFlex const* d = creature->CustomData.Get<CreatureFlex>(kCreatureKey))
                return &d->hold;
            return nullptr;
        }

        FlexMath::Hold& HoldFor(Creature* creature)
        {
            return creature->CustomData.GetDefault<CreatureFlex>(kCreatureKey)->hold;
        }

        MapFlex* MapDataOf(Map* map)
        {
            return map ? map->CustomData.Get<MapFlex>(kMapKey) : nullptr;
        }

        MapFlex& MapDataFor(Map* map)
        {
            return *map->CustomData.GetDefault<MapFlex>(kMapKey);
        }

        bool IsBoss(Creature const* creature)
        {
            return creature->IsDungeonBoss() || creature->isWorldBoss();
        }

        FlexMath::GateInputs GateOf(Creature const* creature, Unit const* victim)
        {
            FlexMath::GateInputs in;
            in.enabled = Cfg().enabled;
            in.flexMap = creature->IsInWorld() && IsFlexMap(creature->FindMap());
            in.alive = creature->IsAlive();
            in.controlledByPlayer = creature->IsControlledByPlayer();
            in.playerOwned = creature->GetCharmerOrOwnerGUID().IsPlayer();
            in.trigger = creature->IsTrigger();
            in.critter = creature->IsCritter();
            FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
            in.hostileToPlayers = faction && faction->IsHostileToPlayers();
            in.victimPlayerControlled = victim && (victim->IsPlayer() || victim->IsControlledByPlayer());
            return in;
        }

        // A dungeon boss that is no longer held (or gone) leaves the encounter: covers an evade or a death this
        // module did not see.
        void PruneEncounter(Map* map, MapFlex& md)
        {
            std::vector<uint64> const owners = md.encounter.owners;
            for (uint64 raw : owners)
            {
                Creature* owner = map->GetCreature(ObjectGuid(raw));
                FlexMath::Hold const* h = owner ? HoldOf(owner) : nullptr;
                if (!owner || !owner->IsAlive() || !owner->IsEngaged() || !h || !h->Held())
                    md.encounter.Leave(raw);
            }
        }

        uint8 PullCountImpl(Creature* creature, int depth);

        // The count a pull of `creature` would hold now. `holdSummoner`: an engaged summoner that is not held yet is
        // held first (the first-caller rule); the command's preview passes false and changes nothing.
        FlexMath::CountResult ResolveCountFor(Creature* creature, bool holdSummoner, int depth)
        {
            FlexMath::CountInputs in;
            Map* map = creature->GetMap();

            if (TempSummon* summon = creature->ToTempSummon())
            {
                Unit* summonerUnit = summon->GetSummonerUnit();
                Creature* summoner = summonerUnit ? summonerUnit->ToCreature() : nullptr;
                if (summoner && summoner != creature && summoner->IsInWorld() && summoner->GetMap() == map)
                {
                    in.summonerCount = HeldCount(summoner);
                    if (!in.summonerCount && holdSummoner && depth < kMaxSummonerDepth && summoner->IsAlive()
                        && summoner->IsEngaged() && !summoner->GetCharmerOrOwnerGUID().IsPlayer())
                    {
                        PullCountImpl(summoner, depth + 1);
                        in.summonerCount = HeldCount(summoner);
                    }
                }
            }

            if (MapFlex* md = MapDataOf(map))
            {
                if (holdSummoner)
                    PruneEncounter(map, *md);
                if (md->encounter.Live())
                    in.encounterCount = md->encounter.count;
                in.overrideCount = md->overrideCount;
            }
            in.players = map->GetPlayersCountExceptGMs();
            return FlexMath::ResolveCount(in, Cfg().settings);
        }

        void Track(Map* map, Creature* creature, bool boss, uint8 count)
        {
            MapFlex& md = MapDataFor(map);
            if (std::find(md.held.begin(), md.held.end(), creature->GetGUID()) == md.held.end())
                md.held.push_back(creature->GetGUID());
            if (boss)
                md.encounter.Join(creature->GetGUID().GetRawValue(), count);
        }

        void Untrack(Map* map, Creature* creature)
        {
            MapFlex* md = MapDataOf(map);
            if (!md)
                return;
            md->held.erase(std::remove(md->held.begin(), md->held.end(), creature->GetGUID()), md->held.end());
            md->encounter.Leave(creature->GetGUID().GetRawValue());
        }

        uint8 PullCountImpl(Creature* creature, int depth)
        {
            if (!creature || !creature->IsInWorld())
                return Cfg().settings.minPlayers;
            if (!FlexMath::Holdable(GateOf(creature, nullptr)))
                return Cfg().settings.minPlayers;
            if (FlexMath::Hold const* h = HoldOf(creature); h && h->Held())
                return h->count;

            FlexMath::CountResult const r = ResolveCountFor(creature, true, depth);
            if (!creature->IsEngaged())
                return r.count;                         // preview: holds only once the creature is engaged

            Map* map = creature->GetMap();
            FlexMath::Hold& h = HoldFor(creature);
            CreatureHealth unit(creature);
            uint32 const maxBefore = creature->GetMaxHealth();
            FlexMath::TakeHold(unit, h, r, GameTime::GetGameTimeMS().count(), Cfg().settings);
            bool const boss = IsBoss(creature);
            Track(map, creature, boss, h.count);

            if (boss || Cfg().debug)
                LOG_INFO(kLog, "flex pull: {} ({}) map {} instance {}: holds {} ({}, {} players), health {} -> {} "
                    "(base {} x {:.2f})", creature->GetName(), creature->GetEntry(), map->GetId(),
                    map->GetInstanceId(), h.count, FlexMath::CountSourceName(h.source),
                    map->GetPlayersCountExceptGMs(), maxBefore, creature->GetMaxHealth(), h.base,
                    FlexMath::HealthLadder(h.count, Cfg().settings));
            return h.count;
        }

        // Evade, death, removal: restore the base and drop the hold.
        void Release(Creature* creature, char const* why)
        {
            CreatureFlex* d = creature->CustomData.Get<CreatureFlex>(kCreatureKey);
            if (!d || !d->hold.Held())
                return;
            uint8 const count = d->hold.count;
            CreatureHealth unit(creature);
            FlexMath::ReleaseHold(unit, d->hold);
            Untrack(creature->FindMap(), creature);
            if (Cfg().debug)
                LOG_INFO(kLog, "flex {}: {} ({}) dropped its hold of {}, max health {} (base {})", why,
                    creature->GetName(), creature->GetEntry(), count, creature->GetMaxHealth(), d->hold.base);
        }
    }

    bool IsFlexMap(Map const* map)
    {
        FlexConfig const& cfg = Cfg();
        return map && cfg.enabled && map->IsDungeon() && cfg.maps.count(map->GetId());
    }

    uint8 HeldCount(Creature const* creature)
    {
        if (!creature)
            return 0;
        FlexMath::Hold const* h = HoldOf(creature);
        return h ? h->count : 0;
    }

    uint8 PullCount(Creature* creature)
    {
        return PullCountImpl(creature, 0);
    }

    uint32 BaseHealth(Creature const* creature)
    {
        if (!creature)
            return 0;
        FlexMath::Hold const* h = HoldOf(creature);
        return h ? h->base : 0;
    }

    void SetScaledMaxHealth(Creature* creature, uint32 base)
    {
        if (!creature)
            return;
        CreatureHealth unit(creature);
        if (!creature->IsInWorld() || !FlexMath::Holdable(GateOf(creature, nullptr)))
        {
            // Off a flex map: the plain script-set health (create health, BASE_VALUE, max), percent kept.
            FlexMath::Rescale(unit, base);
            if (CreatureFlex* d = creature->CustomData.Get<CreatureFlex>(kCreatureKey))
                d->hold.base = std::max<uint32>(base, 1);
            return;
        }
        FlexMath::SetBase(unit, HoldFor(creature), base, Cfg().settings);
    }

    uint8 ClampCount(uint32 players)
    {
        return FlexMath::Clamp(players, Cfg().settings);
    }

    float HealthLadder(uint8 count)
    {
        return float(FlexMath::HealthLadder(count, Cfg().settings));
    }

    uint8 TargetCount(uint8 count, uint8 normal, uint8 big)
    {
        return FlexMath::TargetCount(count, normal, big, Cfg().settings);
    }

    uint8 LootRolls(uint8 count)
    {
        return FlexMath::LootRolls(count, Cfg().settings);
    }

    bool TankSwap(uint8 count)
    {
        return FlexMath::TankSwap(count, Cfg().settings);
    }

    // --- `.dd flex` (DungeonDifficultiesCommand.cpp) ----------------------------------------------------------------

    MapView ViewMap(Map* map)
    {
        MapView v;
        if (!map)
            return v;
        v.flexMap = IsFlexMap(map);
        v.players = map->GetPlayersCountExceptGMs();
        v.freshCount = ClampCount(v.players);
        MapFlex* md = MapDataOf(map);
        if (!md)
            return v;
        PruneEncounter(map, *md);
        v.overrideCount = md->overrideCount;
        v.encounterCount = md->encounter.Live() ? md->encounter.count : 0;
        for (uint64 raw : md->encounter.owners)
            v.encounterOwners.push_back(ObjectGuid(raw));
        std::vector<ObjectGuid> keep;
        for (ObjectGuid const& guid : md->held)
        {
            Creature* c = map->GetCreature(guid);
            FlexMath::Hold const* h = c ? HoldOf(c) : nullptr;
            if (!c || !h || !h->Held())
                continue;                               // a release this module did not see: drop from the list
            keep.push_back(guid);
            v.held.push_back(guid);
        }
        md->held = std::move(keep);
        return v;
    }

    CreatureView ViewCreature(Creature* creature)
    {
        CreatureView v;
        if (!creature || !creature->IsInWorld())
            return v;
        v.holdable = FlexMath::Holdable(GateOf(creature, nullptr));
        if (FlexMath::Hold const* h = HoldOf(creature))
        {
            v.hold = *h;
            v.heldForMs = h->Held() ? GameTime::GetGameTimeMS().count() - h->stamp : 0;
        }
        if (!v.hold.Held() && v.holdable)
            v.preview = ResolveCountFor(creature, false, 0);
        return v;
    }

    bool SetOverride(Map* map, uint8 count)
    {
        if (!IsFlexMap(map))
            return false;
        MapDataFor(map).overrideCount = count;
        return true;
    }
}

namespace DungeonDifficulties::Flex
{
    namespace
    {
        // At the pull: a hold from an earlier tick is a fight whose evade never reached this module.
        void ReleaseIfStale(Creature* creature)
        {
            CreatureFlex const* d = creature->CustomData.Get<CreatureFlex>(kCreatureKey);
            if (d && FlexMath::IsStale(d->hold, GameTime::GetGameTimeMS().count()))
                Release(creature, "stale hold");
        }

        class FlexUnitScript : public UnitScript
        {
        public:
            FlexUnitScript() : UnitScript("DungeonDifficultiesFlexUnitScript", true,
                { UNITHOOK_ON_UNIT_ENTER_COMBAT, UNITHOOK_ON_UNIT_ENTER_EVADE_MODE, UNITHOOK_ON_UNIT_DEATH }) { }

            void OnUnitEnterCombat(Unit* unit, Unit* victim) override
            {
                Creature* creature = unit ? unit->ToCreature() : nullptr;
                if (!creature || !IsFlexMap(creature->FindMap()))
                    return;
                ReleaseIfStale(creature);
                if (FlexMath::HookHolds(GateOf(creature, victim)))
                    PullCountImpl(creature, 0);
            }

            void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override
            {
                if (Creature* creature = unit ? unit->ToCreature() : nullptr)
                    Release(creature, "evade");
            }

            void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
            {
                if (Creature* creature = unit ? unit->ToCreature() : nullptr)
                    Release(creature, "death");
            }
        };

        class FlexCreatureScript : public AllCreatureScript
        {
        public:
            FlexCreatureScript() : AllCreatureScript("DungeonDifficultiesFlexCreatureScript") { }

            void OnCreatureRemoveWorld(Creature* creature) override
            {
                if (creature)
                    Release(creature, "removed from the world");
            }

            // SelectLevel wrote a fresh template health. UpdateEntry mid-fight: the new base, scaled by the held
            // count, so a held creature never drops to its 5-player health. A respawn or an idle UpdateEntry: drop
            // the hold (an idle creature never shows scaled health, and the next pull counts again).
            void OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature) override
            {
                CreatureFlex* d = creature ? creature->CustomData.Get<CreatureFlex>(kCreatureKey) : nullptr;
                if (!d)
                    return;
                switch (FlexMath::OnSelectLevel(d->hold, creature->IsEngaged()))
                {
                    case FlexMath::SelectLevelAction::Rebase:
                    {
                        CreatureHealth unit(creature);
                        FlexMath::SetBase(unit, d->hold, creature->GetCreateHealth(), Cfg().settings);
                        break;
                    }
                    case FlexMath::SelectLevelAction::Release:
                        Release(creature, "respawn or idle template change");
                        break;
                    case FlexMath::SelectLevelAction::None:
                        break;
                }
            }

            // Flex maps only (one set lookup first), held creatures only (one key lookup). AutoBalance loads before
            // this module, so its reset of the same update is already done when this runs.
            void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
            {
                if (!creature || !Cfg().enabled || !IsFlexMap(creature->FindMap()))
                    return;
                CreatureFlex* d = creature->CustomData.Get<CreatureFlex>(kCreatureKey);
                if (!d || !d->hold.Held())
                    return;
                if (FlexMath::IsOrphaned(d->hold, creature->IsAlive(), creature->IsEngaged(),
                    creature->IsInEvadeMode(), creature->IsCharmed()))
                {
                    Release(creature, "idle hold");
                    return;
                }
                CreatureHealth unit(creature);
                uint32 const maxBefore = creature->GetMaxHealth();
                if (FlexMath::Reassert(unit, d->hold) && Cfg().debug)
                    LOG_INFO(kLog, "flex re-assert: {} ({}) max health {} -> {} (held {}, base {})",
                        creature->GetName(), creature->GetEntry(), maxBefore, creature->GetMaxHealth(),
                        d->hold.count, d->hold.base);
            }
        };
    }
}

void AddSC_dungeon_difficulties_flex()
{
    new DungeonDifficulties::Flex::FlexUnitScript();
    new DungeonDifficulties::Flex::FlexCreatureScript();
}
