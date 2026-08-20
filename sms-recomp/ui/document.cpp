#include "document.h"

#include <aurora/rmlui.hpp>

namespace sb::ui {

Document::Document(const Rml::String &source) {
  if (auto *context = aurora::rmlui::get_context()) {
    m_document = context->LoadDocumentFromMemory(source);
  }
}

Document::~Document() {
  m_listeners.clear();
  if (m_document != nullptr) {
    m_document->Close();
    m_document = nullptr;
  }
}

void Document::show() {
  if (m_document != nullptr) {
    m_document->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document,
                     Rml::ScrollFlag::None);
  }
}

void Document::hide() {
  if (m_document != nullptr)
    m_document->Hide();
}

void Document::listen(Rml::Element *element, Rml::EventId event,
                      ScopedEventListener::Callback callback, bool capture) {
  if (element == nullptr || !callback)
    return;
  m_listeners.emplace_back(std::make_unique<ScopedEventListener>(
      element, event, std::move(callback), capture));
}

Rml::Element *Document::element(const char *id) const noexcept {
  return m_document != nullptr ? m_document->GetElementById(id) : nullptr;
}

} // namespace sb::ui
