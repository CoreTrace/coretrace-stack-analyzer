#include "analyzer/HotspotProfiler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ctrace::stack::analyzer
{
    namespace
    {
        struct HotspotStorage
        {
            std::mutex mutex;
            std::unordered_map<std::string, HotspotSample> samples;
        };

        struct NamedHotspotSample
        {
            std::string name;
            HotspotSample sample;
        };

        static HotspotStorage& hotspotStorage()
        {
            static HotspotStorage storage;
            return storage;
        }

        static std::size_t parsePositiveSize(const char* rawValue, std::size_t fallback)
        {
            if (!rawValue || *rawValue == '\0')
                return fallback;

            std::uint64_t parsed = 0;
            for (const char* p = rawValue; *p != '\0'; ++p)
            {
                const unsigned char ch = static_cast<unsigned char>(*p);
                if (!std::isdigit(ch))
                    return fallback;
                parsed = parsed * 10u + static_cast<std::uint64_t>(ch - '0');
                if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    return fallback;
            }

            if (parsed == 0)
                return fallback;
            return static_cast<std::size_t>(parsed);
        }

        static std::size_t hotspotTopCountFromEnv()
        {
            constexpr std::size_t kDefaultTopCount = 20;
            constexpr std::size_t kMaxTopCount = 200;

            const std::size_t parsed =
                parsePositiveSize(std::getenv("CTRACE_HOTSPOT_TOP"), kDefaultTopCount);
            return std::min(parsed, kMaxTopCount);
        }

        static double nsToMs(std::uint64_t ns)
        {
            return static_cast<double>(ns) / 1000000.0;
        }
    } // namespace

    void HotspotProfiler::record(std::string_view name, std::chrono::nanoseconds elapsed)
    {
        if (name.empty())
            return;

        const std::uint64_t elapsedNs = static_cast<std::uint64_t>(elapsed.count());
        HotspotStorage& storage = hotspotStorage();
        std::lock_guard<std::mutex> lock(storage.mutex);
        HotspotSample& sample = storage.samples[std::string(name)];
        ++sample.calls;
        sample.totalNs += elapsedNs;
        sample.maxNs = std::max(sample.maxNs, elapsedNs);
    }

    void HotspotProfiler::dumpTop(std::ostream& os, std::size_t topN)
    {
        if (topN == 0)
            topN = 1;

        std::vector<NamedHotspotSample> rows;
        std::uint64_t trackedTotalNs = 0;
        {
            HotspotStorage& storage = hotspotStorage();
            std::lock_guard<std::mutex> lock(storage.mutex);
            rows.reserve(storage.samples.size());
            for (const auto& kv : storage.samples)
            {
                rows.push_back(NamedHotspotSample{kv.first, kv.second});
                trackedTotalNs += kv.second.totalNs;
            }
        }

        std::sort(rows.begin(), rows.end(),
                  [](const NamedHotspotSample& lhs, const NamedHotspotSample& rhs)
                  {
                      if (lhs.sample.totalNs != rhs.sample.totalNs)
                          return lhs.sample.totalNs > rhs.sample.totalNs;
                      return lhs.name < rhs.name;
                  });

        const std::size_t count = std::min(topN, rows.size());
        os << "Hotspot summary (top " << count << " / " << rows.size()
           << ", tracked_total_ms=" << std::fixed << std::setprecision(3) << nsToMs(trackedTotalNs)
           << "):\n";

        if (rows.empty())
            return;

        for (std::size_t idx = 0; idx < count; ++idx)
        {
            const NamedHotspotSample& row = rows[idx];
            const double totalMs = nsToMs(row.sample.totalNs);
            const double avgMs =
                row.sample.calls == 0 ? 0.0 : totalMs / static_cast<double>(row.sample.calls);
            const double maxMs = nsToMs(row.sample.maxNs);
            const double share = trackedTotalNs == 0
                                     ? 0.0
                                     : (100.0 * static_cast<double>(row.sample.totalNs) /
                                        static_cast<double>(trackedTotalNs));
            os << "  " << (idx + 1) << ". " << row.name << " total_ms=" << std::fixed
               << std::setprecision(3) << totalMs << " avg_ms=" << avgMs << " max_ms=" << maxMs
               << " calls=" << row.sample.calls << " share_pct=" << share << "\n";
        }
    }

    void HotspotProfiler::dumpTop(std::ostream& os)
    {
        dumpTop(os, hotspotTopCountFromEnv());
    }

    ScopedHotspot::ScopedHotspot(bool enabled, std::string_view name)
        : name_(name), enabled_(enabled ? 1u : 0u)
    {
        if (enabled_ != 0)
            start_ = std::chrono::steady_clock::now();
    }

    ScopedHotspot::~ScopedHotspot()
    {
        if (enabled_ == 0)
            return;
        const auto end = std::chrono::steady_clock::now();
        HotspotProfiler::record(name_,
                                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_));
    }

    void dumpHotspotSummary(std::ostream& os, bool enabled)
    {
        if (!enabled)
            return;
        HotspotProfiler::dumpTop(os);
    }

} // namespace ctrace::stack::analyzer
