#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct GxFifoInputStats {
    std::uint64_t appendCalls = 0;
    std::uint64_t appendedBytes = 0;
    std::uint64_t compactions = 0;
    std::uint64_t compactedBytes = 0;
    std::uint64_t capacityGrowths = 0;
};

// Reassembles the 1/2/4-byte stores made to the GameCube write-gather pipe. Consuming a
// parsed prefix advances a cursor rather than erasing the vector, so the incomplete command
// at the end is not memmoved after every parser pass.
class GxFifoInput {
  public:
    void reserve(std::size_t bytes);
    void setStatsEnabled(bool enabled);
    void appendBigEndian(unsigned width, std::uint32_t value);
    void consume(std::size_t bytes);
    void clear();

    [[nodiscard]] const std::uint8_t* data() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] GxFifoInputStats takeStats();

  private:
    void prepareAppend(std::size_t bytes);

    std::vector<std::uint8_t> mStorage;
    std::size_t mBegin = 0;
    GxFifoInputStats mStats;
    bool mStatsEnabled = false;
};
