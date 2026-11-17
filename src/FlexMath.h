/*
 * mod-dungeon-difficulties: the flex 5-10 scaler's rules and hold state (Blackrock Spire build handoff §3.12, spec
 * rulings 6-14 and 20). Pure: no AzerothCore includes, so tests/flex_scale_test.cpp compiles it with the host
 * compiler. FlexScale.cpp feeds it the engine (the player count, the creature's health fields) through a small unit
 * adapter; the health steps below are templates over that adapter so the tests run the exact same sequence.
 *
 * Rules (SYS section 3, "Flex dungeons"):
 *   count     players on the map at the pull (no GMs, bots included), clamped to [minPlayers, maxPlayers] = [5, 10]
 *   health    base x max(kMinLadder, 1 + step x (count - 5)), step 0.25: 1.00 .. 2.25
 *   targets   the normal count at 5 or fewer, the doubled one at 6 or more (odd sizes are harder on purpose)
 *   loot      max(1, count / 5) items
 *   taunts    a tank swap only at 10
 *   damage    never scales
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_FLEX_MATH_H
#define MOD_DUNGEON_DIFFICULTIES_FLEX_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace DungeonDifficulties::FlexMath
{
    constexpr std::uint8_t kReferenceSize = 5;         // the ladder's 1.00 (ruling 20)
    constexpr std::uint8_t kDoubleTargetsAt = 6;       // ruling 8 and 12
    constexpr std::uint8_t kPlayersPerLootRoll = 5;    // ruling 11 (shared doc BS1)
    constexpr std::uint8_t kTankSwapAt = 10;           // ruling 63
    constexpr std::uint8_t kDefaultMinPlayers = 5;
    constexpr std::uint8_t kDefaultMaxPlayers = 10;
    constexpr double kDefaultHealthStep = 0.25;
    constexpr double kMinLadder = 0.1;                 // floor for a MinPlayers below 5 with a large step
    constexpr std::uint32_t kMaxConfigPlayers = 40;    // bound for MinPlayers / MaxPlayers and the GM override
    constexpr double kMaxHealthStep = 2.0;

    struct Settings
    {
        std::uint8_t minPlayers = kDefaultMinPlayers;
        std::uint8_t maxPlayers = kDefaultMaxPlayers;
        double healthStep = kDefaultHealthStep;
    };

    // Config values -> settings. False (and `err`) when out of range; `out` then keeps the defaults.
    inline bool ValidateSettings(std::int64_t minPlayers, std::int64_t maxPlayers, double step, Settings& out,
        std::string& err)
    {
        out = Settings{};
        err.clear();
        if (minPlayers < 1 || minPlayers > std::int64_t(kMaxConfigPlayers))
            err = "MinPlayers " + std::to_string(minPlayers) + " is outside 1-40";
        else if (maxPlayers < minPlayers || maxPlayers > std::int64_t(kMaxConfigPlayers))
            err = "MaxPlayers " + std::to_string(maxPlayers) + " is outside MinPlayers-40";
        else if (!(step >= 0.0 && step <= kMaxHealthStep))
            err = "HealthStep " + std::to_string(step) + " is outside 0-2";
        if (!err.empty())
            return false;
        out.minPlayers = std::uint8_t(minPlayers);
        out.maxPlayers = std::uint8_t(maxPlayers);
        out.healthStep = step;
        return true;
    }

    inline std::uint8_t Clamp(std::uint32_t players, Settings const& s)
    {
        return std::uint8_t(std::clamp<std::uint32_t>(players, s.minPlayers, s.maxPlayers));
    }

    inline double HealthLadder(std::uint32_t count, Settings const& s)
    {
        double const n = double(Clamp(count, s));
        return std::max(kMinLadder, 1.0 + s.healthStep * (n - double(kReferenceSize)));
    }

    // base x ladder, rounded, at least 1, saturating at uint32 max.
    inline std::uint32_t ScaledHealth(std::uint32_t base, std::uint32_t count, Settings const& s)
    {
        double const v = std::round(double(std::max<std::uint32_t>(base, 1)) * HealthLadder(count, s));
        if (v >= double(std::numeric_limits<std::uint32_t>::max()))
            return std::numeric_limits<std::uint32_t>::max();
        return std::max<std::uint32_t>(1, std::uint32_t(v));
    }

    inline std::uint8_t TargetCount(std::uint32_t count, std::uint8_t normal, std::uint8_t big, Settings const& s)
    {
        return Clamp(count, s) >= kDoubleTargetsAt ? big : normal;
    }

    inline std::uint8_t LootRolls(std::uint32_t count, Settings const& s)
    {
        return std::uint8_t(std::max<std::uint32_t>(1, Clamp(count, s) / kPlayersPerLootRoll));
    }

    inline bool TankSwap(std::uint32_t count, Settings const& s)
    {
        return Clamp(count, s) >= kTankSwapAt;
    }

    // Current health carried to a new max at the same percent: full stays full, 0 stays 0, a living unit keeps >= 1.
    inline std::uint32_t KeepPercent(std::uint32_t health, std::uint32_t oldMax, std::uint32_t newMax)
    {
        if (!health)
            return 0;
        if (!oldMax || health >= oldMax)
            return newMax;
        double const v = std::round(double(health) * double(newMax) / double(oldMax));
        return std::clamp<std::uint32_t>(std::uint32_t(std::min(v, double(newMax))), 1, std::max<std::uint32_t>(
            newMax, 1));
    }

    // --- Which creatures the engage hook holds -------------------------------------------------------------------

    struct GateInputs
    {
        bool enabled = false;               // Flex.Enable
        bool flexMap = false;               // map listed in Flex.Maps
        bool alive = true;
        bool controlledByPlayer = false;    // Unit::IsControlledByPlayer (pets, charmed, player summons)
        bool playerOwned = false;           // owner or charmer is a player (guardians included)
        bool trigger = false;               // CREATURE_FLAG_EXTRA_TRIGGER
        bool critter = false;
        bool hostileToPlayers = false;      // faction template hostile to the player mask
        bool victimPlayerControlled = false; // the unit it engaged is a player or player controlled
    };

    // The API (boss scripts) may hold any creature no player controls on a flex map.
    inline bool Holdable(GateInputs const& in)
    {
        return in.enabled && in.flexMap && in.alive && !in.controlledByPlayer && !in.playerOwned;
    }

    // The engage hook holds hostile creatures only: hostile by faction, or engaging a player-controlled unit (a
    // neutral beast a player pulls). Friendly NPCs fighting mobs, triggers and critters stay unscaled.
    inline bool HookHolds(GateInputs const& in)
    {
        return Holdable(in) && !in.trigger && !in.critter && (in.hostileToPlayers || in.victimPlayerControlled);
    }

    // --- Which count a pull holds ----------------------------------------------------------------------------------

    enum class CountSource : std::uint8_t
    {
        Players,        // a fresh count of the map
        Override,       // the GM test override
        Encounter,      // a held dungeon boss of the map is engaged
        Summoner,       // a summon of a held (or engaged) creature
    };

    inline char const* CountSourceName(CountSource source)
    {
        switch (source)
        {
            case CountSource::Players: return "players";
            case CountSource::Override: return "GM override";
            case CountSource::Encounter: return "encounter";
            case CountSource::Summoner: return "summoner";
        }
        return "?";
    }

    struct CountInputs
    {
        std::uint8_t summonerCount = 0;     // the summoner's held count, 0 = none
        std::uint8_t encounterCount = 0;    // the live encounter's count, 0 = none
        std::uint8_t overrideCount = 0;     // `.dd flex set <n>`, 0 = none
        std::uint32_t players = 0;          // players on the map, GMs excluded
    };

    struct CountResult
    {
        std::uint8_t count = kDefaultMinPlayers;
        CountSource source = CountSource::Players;
    };

    // Summoner, then encounter, then override, then players; always clamped. A held count is never replaced by a
    // fresh one: adds join their boss's fight at the boss's size even if a player joined since the pull.
    inline CountResult ResolveCount(CountInputs const& in, Settings const& s)
    {
        if (in.summonerCount)
            return { Clamp(in.summonerCount, s), CountSource::Summoner };
        if (in.encounterCount)
            return { Clamp(in.encounterCount, s), CountSource::Encounter };
        if (in.overrideCount)
            return { Clamp(in.overrideCount, s), CountSource::Override };
        return { Clamp(in.players, s), CountSource::Players };
    }

    // The map's live encounter: the count held by the first dungeon boss pulled while none was live, shared by every
    // pull until the last held boss of it evades, dies or leaves the world.
    struct Encounter
    {
        std::uint8_t count = 0;
        std::vector<std::uint64_t> owners;  // raw guids of the held bosses

        bool Live() const
        {
            return count != 0 && !owners.empty();
        }

        void Join(std::uint64_t owner, std::uint8_t heldCount)
        {
            if (!Live())
            {
                owners.clear();
                count = heldCount;
            }
            if (std::find(owners.begin(), owners.end(), owner) == owners.end())
                owners.push_back(owner);
        }

        void Leave(std::uint64_t owner)
        {
            owners.erase(std::remove(owners.begin(), owners.end(), owner), owners.end());
            if (owners.empty())
                count = 0;
        }
    };

    // --- The hold and the health steps -----------------------------------------------------------------------------

    struct Hold
    {
        std::uint8_t count = 0;             // 0 = not held
        CountSource source = CountSource::Players;
        std::uint64_t stamp = 0;            // game time (ms) of the hold
        std::uint32_t base = 0;             // base (5-player) max health
        std::uint32_t applied = 0;          // the create health the scaler wrote; 0 = nothing of ours applied

        bool Held() const
        {
            return count != 0;
        }
    };

    // The base to scale from at a new hold:
    // - the stored base while the creature still carries the create health the scaler wrote (an evade the hook never
    //   saw: a script's EnterEvadeMode without the core's): never base x ladder x ladder;
    // - else the max health when a script set the field directly (SetMaxHealth only: it no longer matches what the
    //   engine's UpdateMaxHealth would compute from the create health and auras);
    // - else the create health (the template, a heroic template, UpdateEntry or a script's SetCreateHealth).
    inline std::uint32_t ResolveBase(Hold const& h, std::uint32_t createHealth, std::uint32_t maxHealth,
        std::uint32_t modMaxHealth)
    {
        if (h.applied && h.base && createHealth == h.applied)
            return h.base;
        if (maxHealth && maxHealth != modMaxHealth)
            return maxHealth;
        return std::max<std::uint32_t>(createHealth, 1);
    }

    // At the engage hook: a hold from an earlier tick belongs to a fight whose evade the hook did not see.
    inline bool IsStale(Hold const& h, std::uint64_t now)
    {
        return h.Held() && h.stamp != now;
    }

    // UpdateEntry / Respawn wrote a fresh template health (SelectLevel) on a held creature. Engaged: a mid-fight
    // UpdateEntry, the new create health is the new base at the held count. Not engaged: a respawn (respawn
    // compatibility mode keeps the object and its hold) or an idle UpdateEntry, so the hold is dropped and the next
    // pull counts again.
    enum class SelectLevelAction : std::uint8_t
    {
        None,
        Rebase,
        Release,
    };

    inline SelectLevelAction OnSelectLevel(Hold const& h, bool engaged)
    {
        if (!h.Held())
            return SelectLevelAction::None;
        return engaged ? SelectLevelAction::Rebase : SelectLevelAction::Release;
    }

    /*
     * The unit adapter U (FlexScale.cpp wraps a Creature; the tests a fake):
     *   std::uint32_t CreateHealth() const;   UNIT_FIELD_BASE_HEALTH
     *   std::uint32_t MaxHealth() const;
     *   std::uint32_t ModMaxHealth() const;   what Creature::UpdateMaxHealth would set now
     *   std::uint32_t Health() const;
     *   void SetBaseHealth(std::uint32_t);    create health + UNIT_MOD_HEALTH BASE_VALUE, then UpdateMaxHealth
     *   void SetHealth(std::uint32_t);
     */
    template <class U>
    void Rescale(U& unit, std::uint32_t createHealth)
    {
        std::uint32_t const oldMax = unit.MaxHealth();
        std::uint32_t const health = unit.Health();
        unit.SetBaseHealth(std::max<std::uint32_t>(createHealth, 1));
        unit.SetHealth(KeepPercent(health, oldMax, unit.MaxHealth()));
    }

    template <class U>
    void TakeHold(U& unit, Hold& h, CountResult const& r, std::uint64_t stamp, Settings const& s)
    {
        std::uint32_t const base = ResolveBase(h, unit.CreateHealth(), unit.MaxHealth(), unit.ModMaxHealth());
        h.count = r.count;
        h.source = r.source;
        h.stamp = stamp;
        h.base = base;
        Rescale(unit, ScaledHealth(base, r.count, s));
        h.applied = unit.CreateHealth();
    }

    // Evade or death: back to the base when the creature still carries our value (a script that set its own health
    // since keeps it), and drop the hold. The base stays stored for BaseHealth() and the next ResolveBase.
    template <class U>
    void ReleaseHold(U& unit, Hold& h)
    {
        if (h.applied && h.base && unit.CreateHealth() == h.applied)
            Rescale(unit, h.base);
        h.count = 0;
        h.source = CountSource::Players;
        h.stamp = 0;
        h.applied = 0;
    }

    // Script-set base (SetScaledMaxHealth): held, base x ladder now; not held, the base itself.
    template <class U>
    void SetBase(U& unit, Hold& h, std::uint32_t base, Settings const& s)
    {
        h.base = std::max<std::uint32_t>(base, 1);
        if (h.Held())
        {
            Rescale(unit, ScaledHealth(h.base, h.count, s));
            h.applied = unit.CreateHealth();
        }
        else
        {
            Rescale(unit, h.base);
            h.applied = 0;
        }
    }

    // --- Keeping a hold honest between the pull and the release ---------------------------------------------------

    // Something wrote the max health field alone on a held creature while our create health still stands:
    // mod-autobalance's ResetCreatureIfNeeded does that on every dungeon map, its disabled ones included, after a
    // player levels inside or a `.reload config` (SetMaxHealth(template) with the percent kept). The engine would
    // compute modMaxHealth from our create health, so the field is put back to it. A script that sets its own health
    // goes through SetScaledMaxHealth (the create health changes, so this never fires on it).
    inline bool NeedsReassert(Hold const& h, std::uint32_t createHealth, std::uint32_t maxHealth,
        std::uint32_t modMaxHealth)
    {
        return h.Held() && h.applied && createHealth == h.applied && maxHealth != modMaxHealth;
    }

    // The re-assert: max health back to what the create health gives, the current percent kept. True if it acted.
    template <class U>
    bool Reassert(U& unit, Hold const& h)
    {
        if (!NeedsReassert(h, unit.CreateHealth(), unit.MaxHealth(), unit.ModMaxHealth()))
            return false;
        Rescale(unit, unit.CreateHealth());
        return true;
    }

    // A hold whose fight ended without the evade or death hook (a script's EnterEvadeMode that skips the core's, then
    // home): alive, no longer engaged, not walking home, not charmed (a charm swaps the AI, so its engaged flag says
    // nothing). Released by the update hook so an idle creature never keeps scaled health or a stale count.
    inline bool IsOrphaned(Hold const& h, bool alive, bool engaged, bool evading, bool charmed)
    {
        return h.Held() && alive && !engaged && !evading && !charmed;
    }

    // --- AutoBalance check -----------------------------------------------------------------------------------------

    // AutoBalance.Disable.PerInstance as mod-autobalance reads it (LoadDisabledDungeons: split on ',' only, the first
    // whitespace token of each piece through atoi), so "33 229" disables only 33. Used to warn when a flex map is
    // still scaled by AutoBalance (which would stack with the ladder and scale damage).
    inline std::set<std::uint32_t> ParseAutoBalanceDisabled(std::string_view text)
    {
        std::set<std::uint32_t> out;
        std::size_t pos = 0;
        while (pos <= text.size())
        {
            std::size_t const comma = text.find(',', pos);
            std::size_t const end = comma == std::string_view::npos ? text.size() : comma;
            std::string_view const piece = text.substr(pos, end - pos);
            std::size_t i = 0;
            while (i < piece.size() && (piece[i] == ' ' || piece[i] == '\t'))
                ++i;
            std::uint64_t v = 0;
            bool digits = false;
            constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
            while (i < piece.size() && piece[i] >= '0' && piece[i] <= '9' && v <= kMax)
            {
                v = v * 10 + std::uint64_t(piece[i] - '0');
                digits = true;
                ++i;
            }
            if (digits && v <= kMax)
                out.insert(std::uint32_t(v));
            if (comma == std::string_view::npos)
                break;
            pos = comma + 1;
        }
        return out;
    }

    inline std::vector<std::uint32_t> FlexMapsStillAutoBalanced(std::set<std::uint32_t> const& flexMaps,
        std::set<std::uint32_t> const& autoBalanceDisabled)
    {
        std::vector<std::uint32_t> out;
        for (std::uint32_t map : flexMaps)
            if (!autoBalanceDisabled.count(map))
                out.push_back(map);
        return out;
    }
}

#endif
