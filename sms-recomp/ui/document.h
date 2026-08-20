#pragma once

#include "event.h"

#include <memory>
#include <vector>

namespace sb::ui {

// The focused subset of Dusklight's Document abstraction that Sunbright needs
// today. It owns the Rml document and every listener attached to it; feature
// menus own their own document subclass.
class Document {
public:
  explicit Document(const Rml::String &source);
  virtual ~Document();

  Document(const Document &) = delete;
  Document &operator=(const Document &) = delete;

  bool valid() const noexcept { return m_document != nullptr; }
  void show();

protected:
  void listen(Rml::Element *element, Rml::EventId event,
              ScopedEventListener::Callback callback, bool capture = false);
  Rml::Element *element(const char *id) const noexcept;

  Rml::ElementDocument *m_document = nullptr;
  std::vector<std::unique_ptr<ScopedEventListener>> m_listeners;
};

} // namespace sb::ui
