#include <sb_log.h>

#include <lucent/config.h>
#include <lucent/log.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>

namespace {

void emit(lucent::Level level, const char* channel, const char* format, va_list arguments) {
    std::array<char, 4096> message{};
    const int written = std::vsnprintf(message.data(), message.size(), format, arguments);
    if (written < 0) {
        lucent::log(lucent::Level::Error, "logging", "message formatting failed");
        return;
    }
    const auto size = static_cast<std::size_t>(written) < message.size()
                          ? static_cast<std::size_t>(written)
                          : message.size() - 1;
    lucent::log(level, channel, std::string_view{message.data(), size});
}

} // namespace

extern "C" void sb_log_configure(const char* channels) {
    lucent::config::set_prefix("SB_");
    if (channels != nullptr && channels[0] != '\0')
        lucent::enable_channels(channels);
}

extern "C" int sb_log_enabled(const char* channel) {
    return lucent::channel_on(channel) ? 1 : 0;
}

#define SB_DEFINE_LOG_FUNCTION(name, level)                                                        \
    extern "C" void name(const char* channel, const char* format, ...) {                           \
        va_list arguments;                                                                         \
        va_start(arguments, format);                                                               \
        emit(level, channel, format, arguments);                                                   \
        va_end(arguments);                                                                         \
    }

SB_DEFINE_LOG_FUNCTION(sb_logf, lucent::Level::Debug)
SB_DEFINE_LOG_FUNCTION(sb_infof, lucent::Level::Info)
SB_DEFINE_LOG_FUNCTION(sb_warnf, lucent::Level::Warn)
SB_DEFINE_LOG_FUNCTION(sb_errorf, lucent::Level::Error)

#undef SB_DEFINE_LOG_FUNCTION
