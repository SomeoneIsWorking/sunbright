#pragma once

#include "document.h"

#include "app/settings.h"

#include <array>

namespace sb::ui {

class SettingsMenu final : public Document {
public:
  SettingsMenu();

  bool launch_requested() const noexcept { return m_launchRequested; }
  bool layout_valid() const;

private:
  void choose_renderer(app::Renderer renderer);
  void choose_frame_rate(app::FrameRateMode mode);
  void refresh();
  void persist();

  bool m_launchRequested = false;
  std::array<Rml::Element *, 2> m_rendererButtons{};
  std::array<Rml::Element *, 5> m_frameRateButtons{};
  Rml::Element *m_rendererDetail = nullptr;
  Rml::Element *m_frameRateDetail = nullptr;
  Rml::Element *m_playButton = nullptr;
  Rml::Element *m_panel = nullptr;
};

} // namespace sb::ui
