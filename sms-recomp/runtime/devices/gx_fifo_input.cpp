#include "gx_fifo_input.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

void GxFifoInput::reserve(std::size_t bytes) {
    mStorage.reserve(bytes);
}

void GxFifoInput::setStatsEnabled(bool enabled) {
    mStatsEnabled = enabled;
}

void GxFifoInput::prepareAppend(std::size_t bytes) {
    if (empty()) {
        mStorage.clear();
        mBegin = 0;
        return;
    }

    if (mStorage.capacity() - mStorage.size() >= bytes || mBegin == 0) {
        return;
    }

    const std::size_t activeBytes = size();
    std::memmove(mStorage.data(), data(), activeBytes);
    mStorage.resize(activeBytes);
    mBegin = 0;
    if (mStatsEnabled) {
        ++mStats.compactions;
        mStats.compactedBytes += activeBytes;
    }
}

void GxFifoInput::appendBigEndian(unsigned width, std::uint32_t value) {
    if (width != 1 && width != 2 && width != 4) {
        std::abort();
    }
    prepareAppend(width);

    const std::size_t oldSize = mStorage.size();
    const std::size_t oldCapacity = mStorage.capacity();
    mStorage.resize(oldSize + width);
    if (mStatsEnabled) {
        mStats.capacityGrowths += mStorage.capacity() != oldCapacity;
        ++mStats.appendCalls;
        mStats.appendedBytes += width;
    }

    for (unsigned byte = 0; byte < width; ++byte) {
        mStorage[oldSize + byte] = static_cast<std::uint8_t>(value >> (8 * (width - 1 - byte)));
    }
}

void GxFifoInput::consume(std::size_t bytes) {
    if (bytes > size()) {
        std::abort();
    }
    mBegin += bytes;
    if (empty()) {
        mStorage.clear();
        mBegin = 0;
    }
}

void GxFifoInput::clear() {
    mStorage.clear();
    mBegin = 0;
}

const std::uint8_t* GxFifoInput::data() const {
    return mStorage.empty() ? nullptr : mStorage.data() + mBegin;
}

std::size_t GxFifoInput::size() const {
    return mStorage.size() - mBegin;
}

bool GxFifoInput::empty() const {
    return size() == 0;
}

GxFifoInputStats GxFifoInput::takeStats() {
    return std::exchange(mStats, {});
}
