#pragma once

#include "document.h"

namespace sb::ui {

// Dusklight's window ownership boundary: a document supplies its content, while
// Window owns the modal shell and its open/close lifetime.
class Window : public Document {
public:
  explicit Window(const Rml::String &source);

  void show();
  void hide();
  bool visible() const noexcept;

protected:
  Rml::Element *m_root = nullptr;
};

} // namespace sb::ui
