#include "window.h"

namespace sb::ui {

Window::Window(const Rml::String &source) : Document(source) {
  if (!valid())
    return;
  m_root = element("window");
  listen(element("close"), Rml::EventId::Click,
         [this](Rml::Event &) { hide(); });
}

void Window::show() {
  Document::show();
  if (m_root != nullptr)
    m_root->SetAttribute("open", "");
}

void Window::hide() {
  if (m_root != nullptr)
    m_root->RemoveAttribute("open");
  Document::hide();
}

bool Window::visible() const noexcept {
  return m_root != nullptr && m_root->HasAttribute("open");
}

} // namespace sb::ui
