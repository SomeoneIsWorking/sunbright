#pragma once

#include "window.h"

#include "app/settings.h"

#include <array>

namespace sb::ui {

class SettingsMenu final : public Window {
  public:
    SettingsMenu();

    bool layout_valid() const;

  private:
    void choose_renderer(app::Renderer renderer);
    void choose_frame_rate(app::FrameRateMode mode);
    void toggle_haze();
    void refresh();
    void persist();

    std::array<Rml::Element*, 2> m_rendererButtons{};
    std::array<Rml::Element*, 5> m_frameRateButtons{};
    Rml::Element* m_hazeToggle = nullptr;
    Rml::Element* m_rendererDetail = nullptr;
    Rml::Element* m_frameRateDetail = nullptr;
    Rml::Element* m_hazeDetail = nullptr;
    Rml::Element* m_optionsPane = nullptr;
    Rml::Element* m_detailsPane = nullptr;
};

} // namespace sb::ui
