// Host tests for src/FlexMath.h, the flex 5-10 scaler's rules and hold state (no server build). FakeUnit models the
// engine's health fields the way FlexScale.cpp's adapter drives them: create health = UNIT_MOD_HEALTH BASE_VALUE,
// max health = (base + flat) x percent mods (Creature::UpdateMaxHealth), unless a script wrote max health directly.

#include "FlexMath.h"
#include "check.h"

using namespace DungeonDifficulties::FlexMath;

namespace
{
    Settings const kDefault{};

    struct FakeUnit
    {
        std::uint32_t create = 0;
        float baseValue = 0.f;
        float flatMod = 0.f;
        float pctMod = 1.f;
        std::uint32_t max = 0;
        std::uint32_t health = 0;

        // SelectLevel: a fresh template (create = max = health, BASE_VALUE = create).
        static FakeUnit Spawn(std::uint32_t templateHealth, float pct = 1.f)
        {
            FakeUnit u;
            u.pctMod = pct;
            u.create = templateHealth;
            u.baseValue = float(templateHealth);
            u.max = u.ModMaxHealth();
            u.health = u.max;
            return u;
        }

        std::uint32_t CreateHealth() const { return create; }
        std::uint32_t MaxHealth() const { return max; }
        std::uint32_t ModMaxHealth() const { return std::uint32_t((baseValue + flatMod) * pctMod); }
        std::uint32_t Health() const { return health; }

        void SetBaseHealth(std::uint32_t v)
        {
            create = v;
            baseValue = float(v);
            max = ModMaxHealth();
        }

        void SetHealth(std::uint32_t v)
        {
            health = std::min(v, max);
        }
    };

    // FlexScale.cpp's PullCountImpl for one engaged creature: held -> its count; else resolve and hold.
    std::uint8_t Pull(FakeUnit& u, Hold& h, CountInputs const& in, std::uint64_t now, Settings const& s = kDefault)
    {
        if (h.Held())
            return h.count;
        TakeHold(u, h, ResolveCount(in, s), now, s);
        return h.count;
    }

    CountInputs Players(std::uint32_t n)
    {
        CountInputs in;
        in.players = n;
        return in;
    }

    void TestLadder()
    {
        double const expected[] = { 1.00, 1.25, 1.50, 1.75, 2.00, 2.25 };
        for (std::uint32_t n = 5; n <= 10; ++n)
            CHECK_NEAR(HealthLadder(n, kDefault), expected[n - 5], 1e-12);
        CHECK(ScaledHealth(10000, 5, kDefault) == 10000);
        CHECK(ScaledHealth(10000, 6, kDefault) == 12500);
        CHECK(ScaledHealth(10000, 10, kDefault) == 22500);
        CHECK(ScaledHealth(3, 6, kDefault) == 4);                   // 3.75 rounds up
        CHECK(ScaledHealth(0, 10, kDefault) == 2);                  // base floored at 1 -> 2.25 -> 2
        CHECK(ScaledHealth(4000000000u, 10, kDefault) == std::numeric_limits<std::uint32_t>::max());
    }

    void TestClamp()
    {
        CHECK(Clamp(0, kDefault) == 5);
        CHECK(Clamp(1, kDefault) == 5);
        CHECK(Clamp(4, kDefault) == 5);
        CHECK(Clamp(7, kDefault) == 7);
        CHECK(Clamp(11, kDefault) == 10);
        CHECK(Clamp(15, kDefault) == 10);
        CHECK(Clamp(4000000000u, kDefault) == 10);
        CHECK_NEAR(HealthLadder(0, kDefault), 1.0, 1e-12);          // "not held" reads as 5
        CHECK_NEAR(HealthLadder(2, kDefault), 1.0, 1e-12);          // 1-4 players play the 5-player fight
        CHECK_NEAR(HealthLadder(15, kDefault), 2.25, 1e-12);        // 11-15 (normal 229 admits 15) play the 10
    }

    void TestTargetCounts()
    {
        CHECK(TargetCount(0, 1, 2, kDefault) == 1);
        CHECK(TargetCount(3, 1, 2, kDefault) == 1);
        CHECK(TargetCount(5, 1, 2, kDefault) == 1);
        CHECK(TargetCount(6, 1, 2, kDefault) == 2);                 // doubles at 6 (odd sizes harder on purpose)
        CHECK(TargetCount(9, 1, 2, kDefault) == 2);
        CHECK(TargetCount(10, 1, 2, kDefault) == 2);
        CHECK(TargetCount(12, 1, 2, kDefault) == 2);
        CHECK(TargetCount(6, 3, 5, kDefault) == 5);                 // the boss names both counts
        CHECK(TargetCount(5, 3, 5, kDefault) == 3);
    }

    void TestLootRolls()
    {
        CHECK(LootRolls(0, kDefault) == 1);
        for (std::uint32_t n = 1; n <= 9; ++n)
            CHECK(LootRolls(n, kDefault) == 1);
        CHECK(LootRolls(10, kDefault) == 2);
        CHECK(LootRolls(15, kDefault) == 2);                        // clamped to 10
    }

    void TestTankSwap()
    {
        for (std::uint32_t n = 0; n <= 9; ++n)
            CHECK(!TankSwap(n, kDefault));
        CHECK(TankSwap(10, kDefault));
        CHECK(TankSwap(14, kDefault));                              // clamped to 10
        Settings eight;
        eight.maxPlayers = 8;
        CHECK(!TankSwap(10, eight));                                // a clamp below 10 never swaps
    }

    void TestSettings()
    {
        Settings s;
        std::string err;
        CHECK(ValidateSettings(5, 10, 0.25, s, err) && err.empty());
        CHECK(s.minPlayers == 5 && s.maxPlayers == 10);
        CHECK(ValidateSettings(3, 12, 0.5, s, err));
        CHECK(s.minPlayers == 3 && s.maxPlayers == 12);
        CHECK_NEAR(HealthLadder(3, s), 0.1, 1e-12);                 // 1 + 0.5 x -2 = 0: floored
        CHECK_NEAR(HealthLadder(4, s), 0.5, 1e-12);
        CHECK_NEAR(HealthLadder(12, s), 4.5, 1e-12);
        CHECK(!ValidateSettings(0, 10, 0.25, s, err) && !err.empty());
        CHECK(s.minPlayers == 5 && s.maxPlayers == 10);             // defaults on failure
        CHECK(!ValidateSettings(6, 5, 0.25, s, err));
        CHECK(!ValidateSettings(5, 41, 0.25, s, err));
        CHECK(!ValidateSettings(5, 10, -0.1, s, err));
        CHECK(!ValidateSettings(5, 10, 2.5, s, err));
        CHECK_NEAR(s.healthStep, 0.25, 1e-12);
        CHECK(ValidateSettings(5, 10, 0.0, s, err));                // step 0: flex counts, no health change
        CHECK_NEAR(HealthLadder(10, s), 1.0, 1e-12);
    }

    void TestKeepPercent()
    {
        CHECK(KeepPercent(0, 1000, 2250) == 0);                     // dead stays dead
        CHECK(KeepPercent(1000, 1000, 2250) == 2250);               // full stays full
        CHECK(KeepPercent(500, 1000, 2250) == 1125);
        CHECK(KeepPercent(1, 100000, 10) == 1);                     // alive stays alive
        CHECK(KeepPercent(700, 0, 2250) == 2250);
        CHECK(KeepPercent(2000, 1000, 2250) == 2250);               // over max: capped
    }

    void TestGate()
    {
        GateInputs in;
        in.enabled = true;
        in.flexMap = true;
        in.hostileToPlayers = true;
        CHECK(Holdable(in) && HookHolds(in));
        GateInputs off = in;
        off.enabled = false;
        CHECK(!Holdable(off) && !HookHolds(off));
        off = in;
        off.flexMap = false;
        CHECK(!Holdable(off));
        off = in;
        off.alive = false;
        CHECK(!Holdable(off));
        off = in;
        off.controlledByPlayer = true;                              // a charmed mob, a pet
        CHECK(!Holdable(off));
        off = in;
        off.playerOwned = true;                                     // a player's guardian
        CHECK(!Holdable(off));
        off = in;
        off.trigger = true;                                         // the API may hold it, the hook does not
        CHECK(Holdable(off) && !HookHolds(off));
        off = in;
        off.critter = true;
        CHECK(!HookHolds(off));
        GateInputs neutral = in;
        neutral.hostileToPlayers = false;                           // a neutral beast ...
        CHECK(!HookHolds(neutral));                                 // ... fighting another NPC
        neutral.victimPlayerControlled = true;                      // ... pulled by a player
        CHECK(HookHolds(neutral));
    }

    void TestResolveCount()
    {
        CountInputs in = Players(7);
        CountResult r = ResolveCount(in, kDefault);
        CHECK(r.count == 7 && r.source == CountSource::Players);
        in.players = 2;
        CHECK(ResolveCount(in, kDefault).count == 5);
        in.players = 13;
        CHECK(ResolveCount(in, kDefault).count == 10);

        in = Players(2);
        in.overrideCount = 10;                                      // `.dd flex set 10` while solo
        r = ResolveCount(in, kDefault);
        CHECK(r.count == 10 && r.source == CountSource::Override);
        in.overrideCount = 30;
        CHECK(ResolveCount(in, kDefault).count == 10);              // the override is clamped too

        in = Players(9);
        in.overrideCount = 10;
        in.encounterCount = 6;                                      // a boss fight held at 6 wins over both
        r = ResolveCount(in, kDefault);
        CHECK(r.count == 6 && r.source == CountSource::Encounter);
        in.summonerCount = 8;                                       // the summoner's own count wins over all
        r = ResolveCount(in, kDefault);
        CHECK(r.count == 8 && r.source == CountSource::Summoner);
    }

    void TestEncounter()
    {
        Encounter e;
        CHECK(!e.Live());
        e.Join(1001, 6);                                            // Halycon pulled at 6
        CHECK(e.Live() && e.count == 6);
        e.Join(1002, 9);                                            // Gizrul runs in later: keeps 6
        CHECK(e.count == 6 && e.owners.size() == 2);
        e.Join(1002, 6);                                            // no duplicate owner
        CHECK(e.owners.size() == 2);
        e.Leave(1001);                                              // Halycon dies: Gizrul still holds it
        CHECK(e.Live() && e.count == 6);
        e.Leave(1002);
        CHECK(!e.Live() && e.count == 0);
        e.Join(1003, 10);                                           // the next boss starts fresh
        CHECK(e.Live() && e.count == 10);
        e.Leave(4242);                                              // not an owner: nothing changes
        CHECK(e.Live());
    }

    void TestPullEvadePull()
    {
        FakeUnit u = FakeUnit::Spawn(10000);
        Hold h;
        CHECK(Pull(u, h, Players(10), 100) == 10);
        CHECK(u.create == 22500 && u.max == 22500 && u.health == 22500);
        CHECK(h.base == 10000 && h.applied == 22500 && h.stamp == 100);

        u.health = 9000;                                            // the fight goes on
        CHECK(Pull(u, h, Players(5), 200) == 10);                   // held: a later read never recounts
        CHECK(u.max == 22500 && u.health == 9000);

        ReleaseHold(u, h);                                          // evade: base restored, hold dropped
        CHECK(!h.Held() && u.create == 10000 && u.max == 10000 && u.health == 4000);
        CHECK(h.base == 10000 && h.applied == 0);
        u.health = u.max;                                           // back home, full

        CHECK(Pull(u, h, Players(6), 300) == 6);                    // never 22500 x 1.25
        CHECK(u.max == 12500 && u.health == 12500);
        ReleaseHold(u, h);
        CHECK(Pull(u, h, Players(3), 400) == 5);
        CHECK(u.max == 10000);
    }

    void TestMissedEvade()
    {
        FakeUnit u = FakeUnit::Spawn(10000);
        Hold h;
        Pull(u, h, Players(10), 100);
        CHECK(u.max == 22500);
        // The script's EnterEvadeMode skipped the core's: no release. The next pull, a later tick:
        CHECK(IsStale(h, 900));
        CHECK(!IsStale(h, 100));                                    // the pull's own tick (script first, then hook)
        ReleaseHold(u, h);                                          // FlexScale.cpp releases a stale hold first
        CHECK(Pull(u, h, Players(5), 900) == 5);
        CHECK(u.max == 10000);

        // Even without that release, a new hold resolves the stored base while our value is still on the unit.
        FakeUnit v = FakeUnit::Spawn(10000);
        Hold g;
        Pull(v, g, Players(10), 100);
        g.count = 0;                                                // hold forgotten, scaled health left behind
        CHECK(Pull(v, g, Players(6), 900) == 6);
        CHECK(v.max == 12500);
    }

    void TestHookOrder()
    {
        // Creature::AtEngage: JustEngagedWith (the boss script's PullCount) then OnUnitEnterCombat (the hook), same
        // tick. A player joins between the two calls: both orders hold the first caller's number, health scaled once.
        FakeUnit a = FakeUnit::Spawn(50000);
        Hold ha;
        std::uint8_t const scriptA = Pull(a, ha, Players(6), 100);
        std::uint8_t const hookA = (IsStale(ha, 100) ? 0 : Pull(a, ha, Players(7), 100));
        FakeUnit b = FakeUnit::Spawn(50000);
        Hold hb;
        std::uint8_t const hookB = Pull(b, hb, Players(6), 100);
        std::uint8_t const scriptB = Pull(b, hb, Players(7), 100);
        CHECK(scriptA == 6 && hookA == 6 && hookB == 6 && scriptB == 6);
        CHECK(a.max == 62500 && b.max == 62500);
    }

    void TestScriptSetHealth()
    {
        // Hard mode set at the pull (held): base x ladder now; evade restores the hard mode base.
        FakeUnit u = FakeUnit::Spawn(40000);
        Hold h;
        Pull(u, h, Players(10), 100);
        SetBase(u, h, 60000, kDefault);
        CHECK(u.max == 135000 && u.health == 135000 && h.base == 60000 && h.applied == 135000);
        ReleaseHold(u, h);
        CHECK(u.max == 60000);
        u.health = u.max;
        Pull(u, h, Players(5), 200);                                // the script's base, not the template's
        CHECK(u.max == 60000);
        ReleaseHold(u, h);

        // Set before the pull (not held): the base itself; the pull scales it.
        FakeUnit v = FakeUnit::Spawn(40000);
        Hold g;
        SetBase(v, g, 30000, kDefault);
        CHECK(v.max == 30000 && v.health == 30000 && !g.Held() && g.applied == 0);
        CHECK(Pull(v, g, Players(8), 300) == 8);
        CHECK(v.max == 52500);

        // A script that wrote max health only (the kit's SummonWave health): that field is the base.
        FakeUnit w = FakeUnit::Spawn(10000);
        w.max = 4000;
        w.health = 4000;
        Hold k;
        Pull(w, k, Players(10), 100);
        CHECK(k.base == 4000 && w.max == 9000 && w.health == 9000);

        // A script that set its own create health mid-fight: the release leaves it alone.
        FakeUnit x = FakeUnit::Spawn(10000);
        Hold m;
        Pull(x, m, Players(10), 100);
        x.SetBaseHealth(15000);
        ReleaseHold(x, m);
        CHECK(x.max == 15000 && !m.Held());
    }

    void TestAurasAndTemplates()
    {
        // A +10% health aura: the ladder scales the base, the aura stays one multiplication.
        FakeUnit u = FakeUnit::Spawn(10000, 1.1f);
        CHECK(u.max == 11000);
        Hold h;
        Pull(u, h, Players(10), 100);
        CHECK(h.base == 10000 && u.create == 22500 && u.max == 24750);
        ReleaseHold(u, h);
        CHECK(u.create == 10000 && u.max == 11000);

        // UpdateEntry mid-fight (SelectLevel writes the new template, full health): rescaled at the held count.
        FakeUnit v = FakeUnit::Spawn(10000);
        Hold g;
        Pull(v, g, Players(10), 100);
        v = FakeUnit::Spawn(20000);
        SetBase(v, g, v.CreateHealth(), kDefault);                  // OnCreatureSelectLevel
        CHECK(v.max == 45000 && v.health == 45000 && g.Held() && g.count == 10);

        // Death: max restored, health stays 0.
        FakeUnit w = FakeUnit::Spawn(10000);
        Hold k;
        Pull(w, k, Players(7), 100);
        w.health = 0;
        ReleaseHold(w, k);
        CHECK(w.max == 10000 && w.health == 0 && !k.Held());

        // A heroic template: its own create health is the base.
        FakeUnit hc = FakeUnit::Spawn(330000);
        Hold hh;
        Pull(hc, hh, Players(10), 100);
        CHECK(hc.max == 742500);
    }

    void TestAddsTakeTheEncounterCount()
    {
        Encounter enc;
        FakeUnit boss = FakeUnit::Spawn(100000);
        Hold hb;
        Pull(boss, hb, Players(6), 100);
        enc.Join(1, hb.count);
        CHECK(boss.max == 125000);

        // A player joins mid-fight (7 on the map). A summoned add takes its summoner's count ...
        FakeUnit add = FakeUnit::Spawn(8000);
        Hold ha;
        CountInputs in = Players(7);
        in.summonerCount = hb.count;
        in.encounterCount = enc.count;
        CHECK(Pull(add, ha, in, 500) == 6 && ha.source == CountSource::Summoner);
        CHECK(add.max == 10000);
        // ... and a pre-placed add or trash pulled during the fight the encounter's.
        FakeUnit trash = FakeUnit::Spawn(8000);
        Hold ht;
        CountInputs t = Players(7);
        t.encounterCount = enc.Live() ? enc.count : 0;
        t.overrideCount = 10;
        CHECK(Pull(trash, ht, t, 600) == 6 && ht.source == CountSource::Encounter);

        // The boss dies: the encounter ends, the next pull counts 7.
        ReleaseHold(boss, hb);
        enc.Leave(1);
        FakeUnit next = FakeUnit::Spawn(8000);
        Hold hn;
        CountInputs n = Players(7);
        n.encounterCount = enc.Live() ? enc.count : 0;
        CHECK(Pull(next, hn, n, 900) == 7 && hn.source == CountSource::Players);
        CHECK(next.max == 12000);
    }

    // R-1-FW M-1: mod-autobalance's ResetCreatureIfNeeded writes SetMaxHealth(template) with the percent kept on a
    // held creature (its disabled maps too). The update hook puts the scaled max back, percent kept, in the same
    // update; the release and the next pull stay right.
    void TestAutoBalanceResetReassert()
    {
        FakeUnit u = FakeUnit::Spawn(10000);
        Hold h;
        CHECK(Pull(u, h, Players(10), 100) == 10);
        CHECK(u.max == 22500 && u.health == 22500);
        u.SetHealth(11250);                                         // 50%

        // Nothing touched: no re-assert.
        CHECK(!NeedsReassert(h, u.create, u.max, u.ModMaxHealth()));
        CHECK(!Reassert(u, h));

        // AutoBalance: max = template, health = template x percent; the create health is still ours.
        u.max = 10000;
        u.health = 5000;
        CHECK(u.create == h.applied);
        CHECK(NeedsReassert(h, u.create, u.max, u.ModMaxHealth()));
        CHECK(Reassert(u, h));
        CHECK(u.max == 22500 && u.health == 11250);                 // scaled again, still 50%
        CHECK(!Reassert(u, h));                                     // once
        CHECK(h.count == 10 && h.base == 10000);

        // At full health: stays full.
        u.SetHealth(u.max);
        u.max = 10000;
        u.health = 10000;
        CHECK(Reassert(u, h) && u.max == 22500 && u.health == 22500);

        // A health aura moves create and max together: not a re-assert case.
        u.pctMod = 1.1f;
        u.max = u.ModMaxHealth();
        CHECK(!NeedsReassert(h, u.create, u.max, u.ModMaxHealth()));
        u.pctMod = 1.f;
        u.max = u.ModMaxHealth();

        // A script set its own health through SetScaledMaxHealth (create changes with applied): no re-assert after.
        SetBase(u, h, 20000, kDefault);
        CHECK(u.max == 45000 && !NeedsReassert(h, u.create, u.max, u.ModMaxHealth()));
        // A script wrote the create health behind the scaler's back: not ours any more, left alone.
        u.SetBaseHealth(30000);
        u.max = 12345;
        CHECK(!NeedsReassert(h, u.create, u.max, u.ModMaxHealth()));
        u.SetBaseHealth(h.applied);

        // Release after a re-assert: back to the base, once; the next pull scales from the base, not twice.
        u.max = 10000;
        u.health = 10000;
        CHECK(Reassert(u, h));
        ReleaseHold(u, h);
        CHECK(!h.Held() && u.max == 20000);
        CHECK(!NeedsReassert(h, u.create, 1, u.ModMaxHealth()));    // not held: never
        CHECK(Pull(u, h, Players(6), 200) == 6 && u.max == 25000);

        // AutoBalance on an unheld creature: never re-asserted.
        FakeUnit idle = FakeUnit::Spawn(8000);
        Hold none;
        idle.max = 4000;
        CHECK(!Reassert(idle, none) && idle.max == 4000);
    }

    // R-1-FW m-1: a hold must not outlive its fight. An idle creature (alive, not engaged, not evading, not charmed)
    // drops it in the update hook; a respawn (SelectLevel while not engaged) drops it; a mid-fight UpdateEntry keeps
    // it and rescales the new template.
    void TestStaleHoldCatchAll()
    {
        Hold h;
        CHECK(!IsOrphaned(h, true, false, false, false));          // not held
        h.count = 7;
        CHECK(IsOrphaned(h, true, false, false, false));           // alive, idle: release
        CHECK(!IsOrphaned(h, true, true, false, false));           // engaged: in its fight
        CHECK(!IsOrphaned(h, true, false, true, false));           // walking home: the evade hook still comes
        CHECK(!IsOrphaned(h, false, false, false, false));         // dead: the death hook's
        CHECK(!IsOrphaned(h, true, false, false, true));           // charmed: the AI's flag says nothing

        Hold none;
        CHECK(OnSelectLevel(none, true) == SelectLevelAction::None);
        CHECK(OnSelectLevel(none, false) == SelectLevelAction::None);
        CHECK(OnSelectLevel(h, true) == SelectLevelAction::Rebase);
        CHECK(OnSelectLevel(h, false) == SelectLevelAction::Release);

        // The respawn path end to end: held at 10, despawned without a death or evade hook, respawned at a fresh
        // template health. The release does not touch the fresh health (create != applied); the idle creature is
        // unscaled and the next pull counts again.
        FakeUnit u = FakeUnit::Spawn(10000);
        Hold r;
        CHECK(Pull(u, r, Players(10), 100) == 10 && u.max == 22500);
        FakeUnit respawned = u;
        respawned.SetBaseHealth(10000);                             // SelectLevel: the template
        respawned.health = respawned.max;
        CHECK(OnSelectLevel(r, false) == SelectLevelAction::Release);
        ReleaseHold(respawned, r);
        CHECK(!r.Held() && respawned.max == 10000);
        CHECK(Pull(respawned, r, Players(6), 300) == 6 && respawned.max == 12500);
    }

    void TestAutoBalanceParse()
    {
        CHECK(ParseAutoBalanceDisabled("") .empty());
        CHECK(ParseAutoBalanceDisabled("229") == std::set<std::uint32_t>({ 229 }));
        CHECK(ParseAutoBalanceDisabled("33, 43, 229") == std::set<std::uint32_t>({ 33, 43, 229 }));
        CHECK(ParseAutoBalanceDisabled("33 229") == std::set<std::uint32_t>({ 33 }));   // spaces: AB reads 33 only
        CHECK(ParseAutoBalanceDisabled(" 229 ,x,") == std::set<std::uint32_t>({ 229 }));
        std::set<std::uint32_t> const flex = { 229 };
        CHECK(FlexMapsStillAutoBalanced(flex, ParseAutoBalanceDisabled("33 229")).size() == 1);
        CHECK(FlexMapsStillAutoBalanced(flex, ParseAutoBalanceDisabled("33,229")).empty());
    }
}

int main()
{
    TestLadder();
    TestClamp();
    TestTargetCounts();
    TestLootRolls();
    TestTankSwap();
    TestSettings();
    TestKeepPercent();
    TestGate();
    TestResolveCount();
    TestEncounter();
    TestPullEvadePull();
    TestMissedEvade();
    TestHookOrder();
    TestScriptSetHealth();
    TestAurasAndTemplates();
    TestAddsTakeTheEncounterCount();
    TestAutoBalanceParse();
    TestAutoBalanceResetReassert();
    TestStaleHoldCatchAll();
    return Report("flex_scale_test");
}
