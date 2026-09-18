// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_shape_probe.h"

#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

// gcnport hands the hook a GuestContext; title-adapter wants a plain reader. This is the whole
// adaptation: a `bool` answer either way, so a range the runtime refuses stays refused rather than
// becoming zeroes that read like an empty shape.
bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

void print_histogram(const char* label, const std::map<std::uint32_t, std::uint64_t>& histogram,
                     std::uint64_t untracked) {
    if (histogram.empty()) {
        std::printf("gmse01_boot:   %s: none recorded\n", label);
        return;
    }
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [value, count] : histogram) {
        std::printf(" %u=%llu", value, static_cast<unsigned long long>(count));
    }
    if (untracked != 0) {
        std::printf(" (+%llu past the tracked distinct values -- truncated, not complete)",
                    static_cast<unsigned long long>(untracked));
    }
    std::printf("\n");
}

} // namespace

void GuestShapeProbe::record(std::map<std::uint32_t, std::uint64_t>& histogram,
                             std::uint64_t& untracked, std::uint32_t value) {
    if (histogram.size() < MAX_DISTINCT_VALUES || histogram.contains(value)) {
        histogram[value] += 1;
        return;
    }
    untracked += 1;
}

gcnport::HookResult GuestShapeProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    // J3DShape::draw is a const member function, so the shape is `this` in r3 -- confirmed by its
    // own first instructions at 0x802e0390, which read 0x28(r3) (mGDCommands) and 8(r31) (mFlags).
    constexpr std::size_t THIS_REGISTER = 3;
    const auto shapeAddress =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestShape shape{};
    const sb::title_adapter::GuestShapeError error =
        read_guest_shape(memory, shapeAddress, system_, shape);
    shapeErrors_[error] += 1;
    if (error != sb::title_adapter::GuestShapeError::None) {
        return gcnport::HookResult::call_original_once();
    }

    shapesRead_ += 1;
    record(elementCounts_, elementCountsUntracked_, shape.elementCount);
    record(vertexSizes_, vertexSizesUntracked_, shape.layout.vertexSize);
    record(descriptorCounts_, descriptorCountsUntracked_, shape.descriptorCount);

    const bool reporting = reports_ < maxReports_;
    if (reporting) {
        reports_ += 1;
        std::printf("gmse01_boot: shape 0x%08x index=%u elements=%u flags=0x%08x nbt=%d "
                    "vtx=%u nrm=%u col=%u stride=%u descriptors=%u formats=%u\n",
                    shape.address, shape.index, shape.elementCount, shape.flags,
                    shape.normalBinormalTangent ? 1 : 0, shape.vertexCount, shape.normalCount,
                    shape.colorCount, shape.layout.vertexSize, shape.descriptorCount,
                    shape.attributeFormatCount);
        std::printf("gmse01_boot:   arrays pos=0x%08x nrm=0x%08x col=0x%08x tex0=0x%08x\n",
                    shape.positions, shape.normals, shape.colors, shape.textureCoordinates[0]);
    }

    for (std::uint16_t element = 0; element < shape.elementCount; ++element) {
        sb::title_adapter::GuestShapeElement group{};
        const sb::title_adapter::GuestShapeError elementError =
            read_guest_shape_element(memory, shape, element, group);
        elementErrors_[elementError] += 1;
        if (elementError != sb::title_adapter::GuestShapeError::None) {
            continue;
        }
        elementsRead_ += 1;
        displayListBytes_ += group.displayListSize;
        if (reporting) {
            std::printf("gmse01_boot:   element %u draw=0x%08x list=0x%08x size=%u\n", element,
                        group.draw, group.displayList, group.displayListSize);
        }
    }

    return gcnport::HookResult::call_original_once();
}

void GuestShapeProbe::report() const {
    std::printf("gmse01_boot: shape probe entered %llu time(s), read %llu shape(s) and %llu "
                "matrix group(s), %llu display-list byte(s)\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(shapesRead_),
                static_cast<unsigned long long>(elementsRead_),
                static_cast<unsigned long long>(displayListBytes_));
    if (entries_ == 0) {
        std::printf("gmse01_boot:   installed, never dispatched -- no shape was read and nothing "
                    "below was measured\n");
        return;
    }

    std::printf("gmse01_boot:   shape results:");
    for (const auto& [error, count] : shapeErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_shape_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   matrix-group results:");
    if (elementErrors_.empty()) {
        std::printf(" none attempted");
    }
    for (const auto& [error, count] : elementErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_shape_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    print_histogram("matrix groups per shape", elementCounts_, elementCountsUntracked_);
    print_histogram("vertex stride (bytes)", vertexSizes_, vertexSizesUntracked_);
    print_histogram("vertex descriptors per shape", descriptorCounts_, descriptorCountsUntracked_);
}

} // namespace sunbright::gcnport_boot
