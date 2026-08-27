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

struct NativeEfbCopyClear {
    bool enabled = false;
    bool colorUpdate = false;
    bool alphaUpdate = false;
    bool depthUpdate = false;
    float color[4]{};
    float depth = 1.0f;
};

// Decode the BP state that GXCopyTex consumes. Keeping this translation at the typed native-copy
// seam prevents the FIFO parser and renderer from growing independent interpretations of the
// copy-enable bit, clear register channel order, and PE write masks.
[[nodiscard]] NativeEfbCopyClear
sbr_native_efb_copy_clear_from_bp(std::uint32_t copyExecute, std::uint32_t clearAr,
                                  std::uint32_t clearGb, std::uint32_t clearDepth,
                                  std::uint32_t zMode, std::uint32_t colorMode) noexcept;

struct NativeEfbCopyRequest {
    std::uint32_t dest = 0;
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int destWidth = 0;
    int destHeight = 0;
    NativeEfbCopyClear clear{};
};

struct NativeEfbCopyPlan {
    std::uint32_t dest = 0;
    NativeEfbCopySource source{};
    int destWidth = 0;
    int destHeight = 0;
    NativeEfbCopyClear clear{};

    [[nodiscard]] bool has_copy() const noexcept { return dest != 0 && source.valid; }
    [[nodiscard]] bool has_clear() const noexcept {
        return source.valid && clear.enabled &&
               (clear.colorUpdate || clear.alphaUpdate || clear.depthUpdate);
    }
};

[[nodiscard]] NativeEfbCopyPlan sbr_native_efb_copy_plan(const NativeEfbCopyRequest& request,
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
