/*
 * mod-dungeon-difficulties: keyed reference health rows (pure, no AzerothCore includes).
 *
 * Copied from mod-boss-mechanics (src/MechanicRefHealth.h and the ReferenceHealthRow/InterpolateReferenceHealth part
 * of src/MechanicMath.h, WP-3P, hiafi/mod-boss-mechanics#6) so both modules read the same string without linking:
 * paste the same value into BossMechanics.ReferenceHealth.Rows and DungeonDifficulties.ReferenceHealth. Keep the two
 * copies in step when the format changes. Only the parse and lookup are copied (no map level pins: the heroic hook
 * keys on the caster's level).
 *
 * Format: entries "<level>[tier]:<nonTank>:<tank>" separated by spaces, commas, semicolons or tabs. The tier suffix is
 * none (normal), `h` (heroic) or `e<N>` (empowered level N, 1-255), any case. At least one normal row; a key repeated
 * within one tier is an error. Lookup interpolates by level within the key's tier, clamped at both ends; empowered N
 * without `e<N>` rows uses the heroic rows, heroic without heroic rows uses the normal rows.
 */

#ifndef MOD_DUNGEON_DIFFICULTIES_REFERENCE_HEALTH_H
#define MOD_DUNGEON_DIFFICULTIES_REFERENCE_HEALTH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace DungeonDifficulties
{
    struct ReferenceHealth
    {
        std::uint32_t nonTank = 0;
        std::uint32_t tank = 0;
    };

    struct ReferenceHealthRow
    {
        std::uint32_t level = 0;
        ReferenceHealth health;
    };

    // What a reference health row is keyed by. empoweredLevel > 0 is an empowered heroic (E4); heroic is then implied.
    struct ContentKey
    {
        std::uint8_t level = 0;
        bool heroic = false;
        std::uint8_t empoweredLevel = 0;
    };

    namespace RefHealth
    {
        enum class Tier : std::uint8_t
        {
            Normal,
            Heroic,
            Empowered
        };

        // The rows of every tier, each sorted by level.
        struct Table
        {
            std::vector<ReferenceHealthRow> normal;
            std::vector<ReferenceHealthRow> heroic;
            std::map<std::uint8_t, std::vector<ReferenceHealthRow>> empowered;     // by empowered level
        };

        namespace Detail
        {
            inline std::vector<std::string_view> Tokens(std::string_view text)
            {
                std::vector<std::string_view> out;
                std::size_t i = 0;
                auto const isSep = [](char c)
                {
                    return c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n';
                };
                while (i < text.size())
                {
                    while (i < text.size() && isSep(text[i]))
                        ++i;
                    std::size_t const start = i;
                    while (i < text.size() && !isSep(text[i]))
                        ++i;
                    if (i > start)
                        out.push_back(text.substr(start, i - start));
                }
                return out;
            }

            // Decimal digits only (no sign, no blank), at most `max`.
            inline bool ParseUnsigned(std::string_view s, std::uint64_t max, std::uint64_t& out)
            {
                if (s.empty() || s.size() > 19)
                    return false;
                std::uint64_t v = 0;
                for (char c : s)
                {
                    if (c < '0' || c > '9')
                        return false;
                    v = v * 10 + static_cast<std::uint64_t>(c - '0');
                }
                if (v > max)
                    return false;
                out = v;
                return true;
            }

            // Splits "a:b:c" into its fields; false unless exactly `n` fields.
            inline bool SplitColons(std::string_view tok, std::size_t n, std::vector<std::string_view>& out)
            {
                out.clear();
                std::size_t start = 0;
                for (std::size_t i = 0; i <= tok.size(); ++i)
                {
                    if (i == tok.size() || tok[i] == ':')
                    {
                        out.push_back(tok.substr(start, i - start));
                        start = i + 1;
                    }
                }
                return out.size() == n;
            }

            inline void SortRows(std::vector<ReferenceHealthRow>& rows)
            {
                std::sort(rows.begin(), rows.end(), [](ReferenceHealthRow const& a, ReferenceHealthRow const& b)
                {
                    return a.level < b.level;
                });
            }

            inline bool HasLevel(std::vector<ReferenceHealthRow> const& rows, std::uint32_t level)
            {
                for (ReferenceHealthRow const& r : rows)
                    if (r.level == level)
                        return true;
                return false;
            }
        }

        // Linear interpolation by level between the rows, clamped at both ends.
        inline ReferenceHealth Interpolate(std::vector<ReferenceHealthRow> const& rows, std::uint32_t level)
        {
            if (rows.empty())
                return {};
            if (level <= rows.front().level)
                return rows.front().health;
            if (level >= rows.back().level)
                return rows.back().health;
            for (std::size_t i = 1; i < rows.size(); ++i)
            {
                ReferenceHealthRow const& hi = rows[i];
                if (level > hi.level)
                    continue;
                ReferenceHealthRow const& lo = rows[i - 1];
                double const f = hi.level == lo.level ? 0.0
                    : static_cast<double>(level - lo.level) / static_cast<double>(hi.level - lo.level);
                auto lerp = [f](std::uint32_t a, std::uint32_t b)
                {
                    return static_cast<std::uint32_t>(std::lround(a + (static_cast<double>(b) - a) * f));
                };
                return { lerp(lo.health.nonTank, hi.health.nonTank), lerp(lo.health.tank, hi.health.tank) };
            }
            return rows.back().health;
        }

        // Parses the key part "<level>[h|e<N>]". Level 0-255.
        inline bool ParseRowKey(std::string_view s, std::uint32_t& level, Tier& tier, std::uint8_t& empowered)
        {
            std::size_t digits = 0;
            while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9')
                ++digits;
            std::uint64_t v = 0;
            if (!Detail::ParseUnsigned(s.substr(0, digits), std::numeric_limits<std::uint8_t>::max(), v))
                return false;
            level = static_cast<std::uint32_t>(v);
            std::string_view const suffix = s.substr(digits);
            empowered = 0;
            if (suffix.empty())
            {
                tier = Tier::Normal;
                return true;
            }
            if (suffix == "h" || suffix == "H")
            {
                tier = Tier::Heroic;
                return true;
            }
            if (suffix[0] == 'e' || suffix[0] == 'E')
            {
                std::uint64_t n = 0;
                if (!Detail::ParseUnsigned(suffix.substr(1), std::numeric_limits<std::uint8_t>::max(), n) || n == 0)
                    return false;
                tier = Tier::Empowered;
                empowered = static_cast<std::uint8_t>(n);
                return true;
            }
            return false;
        }

        // Parses the rows. On failure `out` is left empty and `err` names the bad entry.
        inline bool ParseTable(std::string_view text, Table& out, std::string& err)
        {
            out = Table();
            err.clear();
            std::vector<std::string_view> parts;
            for (std::string_view tok : Detail::Tokens(text))
            {
                std::uint32_t level = 0;
                Tier tier = Tier::Normal;
                std::uint8_t empowered = 0;
                std::uint64_t nonTank = 0;
                std::uint64_t tank = 0;
                std::uint64_t const maxHealth = std::numeric_limits<std::int32_t>::max();
                if (!Detail::SplitColons(tok, 3, parts) || !ParseRowKey(parts[0], level, tier, empowered)
                    || !Detail::ParseUnsigned(parts[1], maxHealth, nonTank)
                    || !Detail::ParseUnsigned(parts[2], maxHealth, tank) || nonTank == 0 || tank == 0)
                {
                    err = "bad entry '" + std::string(tok) + "' (want <level>[h|e<N>]:<nonTank>:<tank>, health > 0)";
                    out = Table();
                    return false;
                }
                std::vector<ReferenceHealthRow>& rows = tier == Tier::Normal ? out.normal
                    : tier == Tier::Heroic ? out.heroic : out.empowered[empowered];
                if (Detail::HasLevel(rows, level))
                {
                    err = "duplicate entry '" + std::string(tok) + "'";
                    out = Table();
                    return false;
                }
                rows.push_back({ level, { static_cast<std::uint32_t>(nonTank), static_cast<std::uint32_t>(tank) } });
            }
            if (out.normal.empty())
            {
                err = "no normal row (an entry without a suffix, e.g. 20:1300:1290)";
                out = Table();
                return false;
            }
            Detail::SortRows(out.normal);
            Detail::SortRows(out.heroic);
            for (auto& [n, rows] : out.empowered)
                Detail::SortRows(rows);
            return true;
        }

        // The canonical text of a table: normal, heroic, then empowered 1..N, each by level.
        inline std::string Format(Table const& t)
        {
            std::string s;
            auto add = [&s](std::vector<ReferenceHealthRow> const& rows, std::string const& suffix)
            {
                for (ReferenceHealthRow const& r : rows)
                {
                    if (!s.empty())
                        s += ' ';
                    s += std::to_string(r.level) + suffix + ':' + std::to_string(r.health.nonTank) + ':'
                        + std::to_string(r.health.tank);
                }
            };
            add(t.normal, "");
            add(t.heroic, "h");
            for (auto const& [n, rows] : t.empowered)
                add(rows, "e" + std::to_string(n));
            return s;
        }

        // The row for a key: empowered N -> its rows, else heroic; heroic -> its rows, else normal; normal rows.
        inline ReferenceHealth Lookup(Table const& t, ContentKey const& key)
        {
            if (key.empoweredLevel > 0)
            {
                auto const it = t.empowered.find(key.empoweredLevel);
                if (it != t.empowered.end() && !it->second.empty())
                    return Interpolate(it->second, key.level);
            }
            if ((key.heroic || key.empoweredLevel > 0) && !t.heroic.empty())
                return Interpolate(t.heroic, key.level);
            return Interpolate(t.normal, key.level);
        }
    }
}

#endif
