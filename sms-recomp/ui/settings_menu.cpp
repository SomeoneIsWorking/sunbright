#include "settings_menu.h"

#include "app/frame_rate.h"

#include <lucent/log.h>

#include <array>

namespace sb::ui {
namespace {

const Rml::String kDocument = R"RML(
<rml>
<head><link type="text/rcss" href="res/rml/settings.rcss" /></head>
<body>
  <div id="settings-panel" class="window">
    <div class="header">
      <div class="eyebrow">SUPER MARIO SUNSHINE</div>
      <h1>Sunbright</h1>
      <p>Choose the rendering and frame cadence used for this session.</p>
    </div>
    <div class="content">
      <div class="section">
        <h2>Renderer</h2>
        <div class="choices two">
          <button id="renderer-aurora">Aurora</button>
          <button id="renderer-native">Native</button>
        </div>
        <p id="renderer-detail" class="detail" />
      </div>
      <div class="section">
        <h2>Framerate</h2>
        <div class="choices">
          <button id="fps-vanilla">Vanilla</button>
          <button id="fps-interpolated-60">Interpolated 60 FPS</button>
          <button id="fps-interpolated-unlocked">Interpolated Unlocked <span>Unavailable</span></button>
          <button id="fps-native-60">Native 60 FPS</button>
          <button id="fps-native-unlocked">Native Unlocked</button>
        </div>
        <p id="framerate-detail" class="detail" />
      </div>
    </div>
    <div class="footer">
      <span>Settings are saved automatically.</span>
      <button id="play" class="play">Play</button>
    </div>
  </div>
</body>
</rml>
)RML";

constexpr std::array kRendererValues{app::Renderer::Aurora,
                                     app::Renderer::Native};
constexpr std::array kFrameRateValues{
    app::FrameRateMode::Vanilla,
    app::FrameRateMode::Interpolated60,
    app::FrameRateMode::InterpolatedUnlocked,
    app::FrameRateMode::Native60,
    app::FrameRateMode::NativeUnlocked,
};

constexpr std::array<const char *, 2> kRendererIds{"renderer-aurora",
                                                   "renderer-native"};
constexpr std::array<const char *, 5> kFrameRateIds{
    "fps-vanilla",   "fps-interpolated-60", "fps-interpolated-unlocked",
    "fps-native-60", "fps-native-unlocked",
};

const char *renderer_detail(app::Renderer renderer) noexcept {
  switch (renderer) {
  case app::Renderer::Aurora:
    return "Aurora translates the game's GX command stream through WebGPU. "
           "This is the current "
           "faithful renderer and the parity reference for the native path.";
  case app::Renderer::Native:
    return "The SDL3 GPU renderer is still a parity preview. It runs beside "
           "Aurora and is "
           "guarded because it has previously reset the GPU; Aurora remains "
           "the displayed "
           "picture until the native renderer reaches parity.";
  }
  return "";
}

const char *frame_rate_detail(app::FrameRateMode mode) noexcept {
  switch (mode) {
  case app::FrameRateMode::Vanilla:
    return "The original game cadence: one simulation frame every two NTSC "
           "retraces (30 FPS).";
  case app::FrameRateMode::Interpolated60:
    return "Game logic remains at 30 FPS; one renderer-generated in-between "
           "frame produces a "
           "60 FPS presentation.";
  case app::FrameRateMode::InterpolatedUnlocked:
    return "Game logic remains at 30 FPS; presentation frames are interpolated "
           "at the display "
           "rate without a 60 FPS cap.";
  case app::FrameRateMode::Native60:
    return "The game's own TDisplay retrace interval is overridden from two "
           "fields to one, so "
           "simulation and rendering both run at native 60 FPS.";
  case app::FrameRateMode::NativeUnlocked:
    return "The same game-native one-field override with host pacing removed. "
           "Simulation runs "
           "as fast as the machine can sustain.";
  }
  return "";
}

} // namespace

SettingsMenu::SettingsMenu() : Document(kDocument) {
  if (!valid())
    return;

  m_panel = element("settings-panel");
  for (std::size_t i = 0; i < m_rendererButtons.size(); ++i) {
    m_rendererButtons[i] = element(kRendererIds[i]);
    listen(m_rendererButtons[i], Rml::EventId::Click,
           [this, i](Rml::Event &) { choose_renderer(kRendererValues[i]); });
  }
  for (std::size_t i = 0; i < m_frameRateButtons.size(); ++i) {
    m_frameRateButtons[i] = element(kFrameRateIds[i]);
    listen(m_frameRateButtons[i], Rml::EventId::Click,
           [this, i](Rml::Event &) { choose_frame_rate(kFrameRateValues[i]); });
  }
  m_playButton = element("play");
  listen(m_playButton, Rml::EventId::Click, [this](Rml::Event &) {
    if (!app::frame_rate::is_supported(app::settings().effective().frameRate)) {
      lucent::error("ui", "cannot launch with {}: {}",
                    app::display_name(app::settings().effective().frameRate),
                    app::frame_rate::unsupported_reason(
                        app::settings().effective().frameRate));
      return;
    }
    if (app::settings().effective().renderer == app::Renderer::Native)
      app::settings().approve_native_renderer_session();
    m_launchRequested = true;
  });
  m_rendererDetail = element("renderer-detail");
  m_frameRateDetail = element("framerate-detail");
  refresh();
}

bool SettingsMenu::layout_valid() const {
  if (m_panel == nullptr || m_playButton == nullptr)
    return false;
  const float panelWidth = m_panel->GetOffsetWidth();
  const float panelHeight = m_panel->GetOffsetHeight();
  const float playWidth = m_playButton->GetOffsetWidth();
  const float playHeight = m_playButton->GetOffsetHeight();
  const Rml::Vector2f panelOffset =
      m_panel->GetAbsoluteOffset(Rml::BoxArea::Border);
  const Rml::Vector2i viewport = m_document->GetContext()->GetDimensions();
  std::size_t visibleChoices = 0;
  for (Rml::Element *button : m_rendererButtons)
    if (button != nullptr && button->GetOffsetWidth() > 0 &&
        button->GetOffsetHeight() > 0)
      ++visibleChoices;
  for (Rml::Element *button : m_frameRateButtons)
    if (button != nullptr && button->GetOffsetWidth() > 0 &&
        button->GetOffsetHeight() > 0)
      ++visibleChoices;
  const bool insideViewport = panelOffset.x >= 0 && panelOffset.y >= 0 &&
                              panelOffset.x + panelWidth <= viewport.x &&
                              panelOffset.y + panelHeight <= viewport.y;
  const bool valid =
      panelWidth > 0 && panelHeight > 0 && playWidth > 0 && playHeight > 0 &&
      insideViewport &&
      visibleChoices == m_rendererButtons.size() + m_frameRateButtons.size();
  lucent::info(
      "ui",
      "settings layout: panel=({}, {}) {}x{} in {}x{}, play={}x{}, visible "
      "choices={}/{}{}",
      panelOffset.x, panelOffset.y, panelWidth, panelHeight, viewport.x,
      viewport.y, playWidth, playHeight, visibleChoices,
      m_rendererButtons.size() + m_frameRateButtons.size(),
      valid ? "" : " — INVALID: a required control has zero computed area");
  return valid;
}

void SettingsMenu::choose_renderer(app::Renderer renderer) {
  app::settings().set_renderer(renderer);
  persist();
  refresh();
}

void SettingsMenu::choose_frame_rate(app::FrameRateMode mode) {
  if (!app::frame_rate::is_supported(mode)) {
    lucent::warn("ui", "{} is not available: {}", app::display_name(mode),
                 app::frame_rate::unsupported_reason(mode));
    return;
  }
  app::settings().set_frame_rate(mode);
  persist();
  refresh();
}

void SettingsMenu::persist() {
  if (!app::settings().save()) {
    lucent::error("ui",
                  "settings changed in memory but could not be saved to {}",
                  app::settings().path().string());
  }
}

void SettingsMenu::refresh() {
  const auto &selected = app::settings().effective();
  for (std::size_t i = 0; i < m_rendererButtons.size(); ++i) {
    if (m_rendererButtons[i] != nullptr)
      m_rendererButtons[i]->SetClass("selected",
                                     kRendererValues[i] == selected.renderer);
  }
  for (std::size_t i = 0; i < m_frameRateButtons.size(); ++i) {
    if (m_frameRateButtons[i] != nullptr) {
      m_frameRateButtons[i]->SetClass("selected", kFrameRateValues[i] ==
                                                      selected.frameRate);
      m_frameRateButtons[i]->SetClass(
          "unavailable", !app::frame_rate::is_supported(kFrameRateValues[i]));
    }
  }
  if (m_rendererDetail != nullptr)
    m_rendererDetail->SetInnerRML(renderer_detail(selected.renderer));
  if (m_frameRateDetail != nullptr) {
    if (const char *reason =
            app::frame_rate::unsupported_reason(selected.frameRate)) {
      m_frameRateDetail->SetInnerRML(Rml::String("Unavailable: ") + reason);
    } else {
      m_frameRateDetail->SetInnerRML(frame_rate_detail(selected.frameRate));
    }
  }
  if (m_playButton != nullptr)
    m_playButton->SetClass("unavailable",
                           !app::frame_rate::is_supported(selected.frameRate));
}

} // namespace sb::ui
