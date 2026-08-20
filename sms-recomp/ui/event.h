#pragma once

#include <RmlUi/Core.h>

#include <functional>

namespace sb::ui {

// Copied from Dusklight's scoped listener ownership pattern: an event
// subscription and its callback have exactly the same lifetime, so a closed
// document cannot retain a dangling handler.
class ScopedEventListener final : public Rml::EventListener {
public:
  using Callback = std::function<void(Rml::Event &)>;

  ScopedEventListener(Rml::Element *element, Rml::EventId event,
                      Callback callback, bool capture = false);
  ~ScopedEventListener() override;

  ScopedEventListener(const ScopedEventListener &) = delete;
  ScopedEventListener &operator=(const ScopedEventListener &) = delete;

  void ProcessEvent(Rml::Event &event) override;
  void OnDetach(Rml::Element *element) override;

private:
  Rml::Element *m_element = nullptr;
  Rml::EventId m_event = Rml::EventId::Invalid;
  bool m_capture = false;
  Callback m_callback;
};

} // namespace sb::ui
