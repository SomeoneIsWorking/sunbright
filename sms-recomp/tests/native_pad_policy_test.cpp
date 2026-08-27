#include "overrides/native_pad_policy.h"

#include <cstdio>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    using namespace sunbright::pad;

    CHECK(parse_script_clock("") == ScriptClock::ReadCount);
    CHECK(parse_script_clock("read-count") == ScriptClock::ReadCount);
    CHECK(parse_script_clock("guest-retrace") == ScriptClock::GuestRetrace);
    CHECK(!parse_script_clock("guest_retrace"));
    CHECK(!parse_script_clock("frame"));
    CHECK(script_clock_name(ScriptClock::ReadCount) == "read-count");
    CHECK(script_clock_name(ScriptClock::GuestRetrace) == "guest-retrace");
    CHECK(select_script_key(ScriptClock::ReadCount, 401, 800) == 401);
    CHECK(select_script_key(ScriptClock::GuestRetrace, 401, 800) == 800);
    CHECK(select_script_key(ScriptClock::GuestRetrace, 200, 800) ==
          select_script_key(ScriptClock::GuestRetrace, 400, 800));
    CHECK(select_script_key(ScriptClock::ReadCount, 200, 800) !=
          select_script_key(ScriptClock::ReadCount, 400, 800));

    CHECK(combine_buttons(0x0100, 0x1000, false) == 0x1100);
    CHECK(combine_buttons(0x0100, 0x1000, true) == 0x0100);
    CHECK(select_axis(12, -40, false) == 12);
    CHECK(select_axis(0x8000, -40, false) == -40);
    CHECK(select_axis(0x8000, -40, true) == 0);
    CHECK(select_trigger(127, 255, false) == 127);
    CHECK(select_trigger(-1, 255, false) == 255);
    CHECK(select_trigger(-1, 255, true) == 0);
}
