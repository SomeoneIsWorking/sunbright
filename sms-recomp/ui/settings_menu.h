#pragma once

#include "window.h"

#include "app/settings.h"

#include <array>

namespace sb::ui {

class SettingsMenu final : public Window {
public:
  explicit SettingsMenu(bool prelaunch = false);

  bool launch_requested() const noexcept { return m_launchRequested; }
  bool layout_valid() const;

private:
  void choose_renderer(app::Renderer renderer);
  void choose_frame_rate(app::FrameRateMode mode);
  void refresh();
  void persist();

  bool m_launchRequested = false;
  bool m_prelaunch = false;
  std::array<Rml::Element *, 2> m_rendererButtons{};
  std::array<Rml::Element *, 5> m_frameRateButtons{};
  Rml::Element *m_rendererDetail = nullptr;
  Rml::Element *m_frameRateDetail = nullptr;
  Rml::Element *m_playButton = nullptr;
  Rml::Element *m_optionsPane = nullptr;
  Rml::Element *m_detailsPane = nullptr;
};

} // namespace sb::ui
