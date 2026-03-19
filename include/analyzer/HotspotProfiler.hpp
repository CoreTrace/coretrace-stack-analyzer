#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace ctrace::stack::analyzer
{

    struct HotspotSample
    {
        std::uint64_t calls = 0;
        std::uint64_t totalNs = 0;
        std::uint64_t maxNs = 0;
    };

    class HotspotProfiler
    {
      public:
        static void record(std::string_view name, std::chrono::nanoseconds elapsed);
        static void dumpTop(std::ostream& os, std::size_t topN);
        static void dumpTop(std::ostream& os);
    };

    class ScopedHotspot
    {
      public:
        explicit ScopedHotspot(bool enabled, std::string_view name);
        ~ScopedHotspot();

        ScopedHotspot(const ScopedHotspot&) = delete;
        ScopedHotspot& operator=(const ScopedHotspot&) = delete;

      private:
        std::chrono::steady_clock::time_point start_{};
        std::string_view name_;
        std::uint8_t enabled_ = 0;
        std::uint8_t reservedPadding_[7] = {};
    };

    void dumpHotspotSummary(std::ostream& os, bool enabled);

} // namespace ctrace::stack::analyzer
