#pragma once

extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);

namespace sb {

// Game-thread new routes through JKR. Native semantic capture owns host STL storage, so every
// adapter uses this one scoped gate while it copies a command into the process collector.
class HostAllocationScope {
  public:
    HostAllocationScope() { sb_host_alloc_push(); }
    ~HostAllocationScope() { sb_host_alloc_pop(); }

    HostAllocationScope(const HostAllocationScope&) = delete;
    HostAllocationScope& operator=(const HostAllocationScope&) = delete;
};

} // namespace sb
