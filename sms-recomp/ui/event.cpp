#include "event.h"

#include <utility>

namespace sb::ui {

ScopedEventListener::ScopedEventListener(Rml::Element *element,
                                         Rml::EventId event, Callback callback,
                                         bool capture)
    : m_element(element), m_event(event), m_capture(capture),
      m_callback(std::move(callback)) {
  m_element->AddEventListener(m_event, this, m_capture);
}

ScopedEventListener::~ScopedEventListener() {
  if (m_element != nullptr) {
    m_element->RemoveEventListener(m_event, this, m_capture);
  }
}

void ScopedEventListener::ProcessEvent(Rml::Event &event) {
  if (m_callback)
    m_callback(event);
}

void ScopedEventListener::OnDetach(Rml::Element *element) {
  if (element == m_element)
    m_element = nullptr;
}

} // namespace sb::ui
