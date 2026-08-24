#pragma once

#include <cstddef>
#include <cstdint>

struct NativeEfbCopySource {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

[[nodiscard]] NativeEfbCopySource sbr_native_efb_copy_source(int x, int y, int width, int height,
                                                             int targetWidth,
                                                             int targetHeight) noexcept;

// A copy is an ordering barrier: a following same-state draw must start a new batch so the copy
// remains between the two draws. The epoch is stored on each batch and compared before merging.
class NativeEfbCopySequence {
  public:
    void reset() noexcept { m_epoch = 0; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return m_epoch; }
    [[nodiscard]] std::size_t note_copy(std::size_t batchCount) noexcept {
        ++m_epoch;
        return batchCount;
    }
    [[nodiscard]] bool may_merge(std::uint64_t batchEpoch) const noexcept {
        return batchEpoch == m_epoch;
    }

  private:
    std::uint64_t m_epoch = 0;
};
