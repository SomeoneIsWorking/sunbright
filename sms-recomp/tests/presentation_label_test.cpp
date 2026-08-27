#include "frame_interp/presentation_label.h"

#include <cstdio>
#include <string_view>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    using sb::frame_interp::presentation_dump_label;
    using sb::frame_interp::PresentationRole;
    using sb::frame_interp::replay_sample_dump_label;

    const auto main = presentation_dump_label(PresentationRole::Main, 4812);
    const auto sub = presentation_dump_label(PresentationRole::Sub, 4812);
    CHECK(std::string_view(main.data()) == "main-t4812");
    CHECK(std::string_view(sub.data()) == "sub-t4812");

    // Fixed interpolated-60 has one retained replay. It must produce the OTHER role while carrying
    // the exact same guest-tick anchor as the primary emission.
    const auto fixed60 = replay_sample_dump_label(4812, 1, 2);
    CHECK(fixed60.has_value());
    CHECK(std::string_view(fixed60->data()) == "sub-t4812");

    // Match-refresh may present more than twice per tick. Every retained sample is a sub-frame;
    // the dump series sequence number, not a second role vocabulary, orders those samples.
    const auto firstOfFour = replay_sample_dump_label(4812, 1, 4);
    const auto lastOfFour = replay_sample_dump_label(4812, 3, 4);
    CHECK(firstOfFour.has_value());
    CHECK(lastOfFour.has_value());
    CHECK(std::string_view(firstOfFour->data()) == "sub-t4812");
    CHECK(std::string_view(lastOfFour->data()) == "sub-t4812");

    CHECK(!replay_sample_dump_label(4812, 0, 2).has_value());
    CHECK(!replay_sample_dump_label(4812, 2, 2).has_value());
    CHECK(!replay_sample_dump_label(4812, 1, 1).has_value());
}
