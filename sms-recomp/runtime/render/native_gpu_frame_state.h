#pragma once

// Owns the one invariant shared by every native-render diagnostic consumer: a CPU frame is
// readable only after the current GPU pass completed its fence and mapped its download buffer.
// Beginning another frame or observing any failed step invalidates the previous bytes immediately.
class NativeGpuFrameState {
  public:
    void begin_frame() noexcept { m_fresh = false; }
    void complete_frame() noexcept { m_fresh = true; }
    void fail_frame() noexcept { m_fresh = false; }
    [[nodiscard]] bool readable() const noexcept { return m_fresh; }

  private:
    bool m_fresh = false;
};
