#include "settings_menu.h"

#include "app/frame_rate.h"

#include <lucent/log.h>

#include <array>

namespace sb::ui {
namespace {

const Rml::String kDocument = R"RML(
<rml>
<head>
  <link type="text/rcss" href="res/rml/tabbing.rcss" />
  <link type="text/rcss" href="res/rml/window.rcss" />
  <link type="text/rcss" href="res/rml/settings.rcss" />
</head>
<body>
  <window id="window">
    <tab-bar closable>
      <tab selected>Settings</tab><tab-end-spacer/><close id="close">&#xd7;</close>
    </tab-bar>
    <content>
      <pane id="options-pane">
        <div class="section-heading">Renderer</div>
        <select-button id="renderer-aurora"><key>Aurora</key><value/></select-button>
        <select-button id="renderer-native"><key>Native</key><value/></select-button>
        <div class="section-heading">Framerate</div>
        <select-button id="fps-vanilla"><key>Vanilla</key><value/></select-button>
        <select-button id="fps-interpolated-60"><key>Interpolated 60 FPS</key><value/></select-button>
        <select-button id="fps-interpolated-match-refresh"><key>Interpolated Match Refresh</key><value/></select-button>
        <select-button id="fps-native-60"><key>Native 60 FPS</key><value/></select-button>
        <select-button id="fps-native-match-refresh"><key>Native Match Refresh</key><value/></select-button>
        <div class="section-heading">Effects</div>
        <select-button id="haze-toggle"><key>Heat Haze</key><value/></select-button>
        <spacer/>
      </pane>
      <pane id="details-pane">
        <div class="brand">Sunbright</div>
        <div class="detail-heading">Renderer</div>
        <div id="renderer-detail" class="detail"/>
        <div class="detail-heading">Framerate</div>
        <div id="framerate-detail" class="detail"/>
        <div class="detail-heading">Effects</div>
        <div id="haze-detail" class="detail"/>
        <div class="grow"/>
        <div class="saved">Changes are saved automatically.</div>
        <div class="escape-hint">ESC closes settings</div>
        <spacer/>
      </pane>
    </content>
  </window>
</body>
</rml>
)RML";

constexpr std::array kRendererValues{app::Renderer::Aurora, app::Renderer::Native};
constexpr std::array kFrameRateValues{
    app::FrameRateMode::Vanilla,
    app::FrameRateMode::Interpolated60,
    app::FrameRateMode::InterpolatedMatchRefresh,
    app::FrameRateMode::Native60,
    app::FrameRateMode::NativeMatchRefresh,
};
constexpr std::array<const char*, 2> kRendererIds{"renderer-aurora", "renderer-native"};
constexpr std::array<const char*, 5> kFrameRateIds{"fps-vanilla", "fps-interpolated-60",
                                                   "fps-interpolated-match-refresh",
                                                   "fps-native-60", "fps-native-match-refresh"};

const char* renderer_detail(app::Renderer renderer) noexcept {
    if (renderer == app::Renderer::Aurora)
        return "Aurora translates and presents the game's GX command stream through WebGPU. "
               "It is the default renderer and Native's parity oracle. Restart required.";
    return "Native owns the SDL3 GPU device, window swapchain, and displayed picture. Aurora "
           "renders offscreen only as its oracle. Restart with run-render.sh to apply.";
}

const char* haze_detail(bool enabled) noexcept {
    if (enabled)
        return "Heat haze (TShimmer) is active. The shimmer effect samples the "
               "screen capture and distorts it through a scroll-animated mesh.";
    return "Heat haze is disabled. The shimmer overlay is suppressed without "
           "affecting other screen-space effects (dash blur, water refraction).";
}

const char* frame_rate_detail(app::FrameRateMode mode) noexcept {
    switch (mode) {
    case app::FrameRateMode::Vanilla:
        return "Original cadence: one simulation frame every two NTSC retraces (30 "
               "FPS).";
    case app::FrameRateMode::Interpolated60:
        return "Game logic stays at 30 FPS; a renderer-generated midpoint produces "
               "60 FPS presentation.";
    case app::FrameRateMode::InterpolatedMatchRefresh:
        return "Game logic stays at its original 30 FPS; Aurora presents reusable "
               "interpolation samples at the active display refresh rate.";
    case app::FrameRateMode::Native60:
        return "Overrides the game's own TDisplay retrace interval from two fields "
               "to one (native 60 FPS).";
    case app::FrameRateMode::NativeMatchRefresh:
        return "Overrides the game's own TDisplay retrace interval to one field "
               "and paces SMS game logic to the active display refresh rate.";
    }
    return "";
}

} // namespace

SettingsMenu::SettingsMenu() : Window(kDocument) {
    if (!valid())
        return;
    m_optionsPane = element("options-pane");
    m_detailsPane = element("details-pane");
    for (std::size_t i = 0; i < m_rendererButtons.size(); ++i) {
        m_rendererButtons[i] = element(kRendererIds[i]);
        listen(m_rendererButtons[i], Rml::EventId::Click,
               [this, i](Rml::Event&) { choose_renderer(kRendererValues[i]); });
    }
    for (std::size_t i = 0; i < m_frameRateButtons.size(); ++i) {
        m_frameRateButtons[i] = element(kFrameRateIds[i]);
        listen(m_frameRateButtons[i], Rml::EventId::Click,
               [this, i](Rml::Event&) { choose_frame_rate(kFrameRateValues[i]); });
    }
    m_hazeToggle = element("haze-toggle");
    listen(m_hazeToggle, Rml::EventId::Click, [this](Rml::Event&) { toggle_haze(); });
    m_rendererDetail = element("renderer-detail");
    m_frameRateDetail = element("framerate-detail");
    m_hazeDetail = element("haze-detail");
    refresh();
}

bool SettingsMenu::layout_valid() const {
    if (m_root == nullptr || m_optionsPane == nullptr || m_detailsPane == nullptr)
        return false;
    std::size_t visibleChoices = 0;
    for (Rml::Element* choice : m_rendererButtons)
        visibleChoices +=
            choice != nullptr && choice->GetOffsetWidth() > 0 && choice->GetOffsetHeight() > 0;
    for (Rml::Element* choice : m_frameRateButtons)
        visibleChoices +=
            choice != nullptr && choice->GetOffsetWidth() > 0 && choice->GetOffsetHeight() > 0;
    visibleChoices += m_hazeToggle != nullptr && m_hazeToggle->GetOffsetWidth() > 0 &&
                      m_hazeToggle->GetOffsetHeight() > 0;
    const Rml::Vector2f offset = m_root->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2i viewport = m_document->GetContext()->GetDimensions();
    const float width = m_root->GetOffsetWidth();
    const float height = m_root->GetOffsetHeight();
    const bool inside = offset.x >= 0 && offset.y >= 0 && offset.x + width <= viewport.x &&
                        offset.y + height <= viewport.y;
    const bool valid = width > 0 && height > 0 && inside &&
                       visibleChoices == kRendererValues.size() + kFrameRateValues.size() + 1;
    lucent::info("ui", "settings window: ({}, {}) {}x{} in {}x{}, choices={}/{}{}", offset.x,
                 offset.y, width, height, viewport.x, viewport.y, visibleChoices,
                 kRendererValues.size() + kFrameRateValues.size() + 1, valid ? "" : " — INVALID");
    return valid;
}

void SettingsMenu::choose_renderer(app::Renderer renderer) {
    app::settings().set_renderer(renderer);
    persist();
    refresh();
}

void SettingsMenu::choose_frame_rate(app::FrameRateMode mode) {
    app::settings().set_frame_rate(mode);
    persist();
    refresh();
}

void SettingsMenu::toggle_haze() {
    app::settings().set_haze_enabled(!app::settings().effective().hazeEnabled);
    persist();
    refresh();
}

void SettingsMenu::persist() {
    if (!app::settings().save())
        lucent::error("ui", "settings changed but could not be saved to {}",
                      app::settings().path().string());
}

void SettingsMenu::refresh() {
    const auto& selected = app::settings().effective();
    for (std::size_t i = 0; i < m_rendererButtons.size(); ++i) {
        auto* choice = m_rendererButtons[i];
        const bool active = kRendererValues[i] == selected.renderer;
        if (choice != nullptr) {
            choice->SetPseudoClass("selected", active);
            if (auto* value = choice->QuerySelector("value"))
                value->SetInnerRML(active ? "Selected" : "");
        }
    }
    for (std::size_t i = 0; i < m_frameRateButtons.size(); ++i) {
        auto* choice = m_frameRateButtons[i];
        const bool active = kFrameRateValues[i] == selected.frameRate;
        if (choice != nullptr) {
            choice->SetPseudoClass("selected", active);
            choice->RemoveAttribute("disabled");
            if (auto* value = choice->QuerySelector("value"))
                value->SetInnerRML(active ? "Selected" : "");
        }
    }
    if (m_hazeToggle != nullptr) {
        m_hazeToggle->SetPseudoClass("selected", selected.hazeEnabled);
        if (auto* value = m_hazeToggle->QuerySelector("value"))
            value->SetInnerRML(selected.hazeEnabled ? "On" : "Off");
    }
    if (m_rendererDetail != nullptr)
        m_rendererDetail->SetInnerRML(renderer_detail(selected.renderer));
    if (m_frameRateDetail != nullptr)
        m_frameRateDetail->SetInnerRML(frame_rate_detail(selected.frameRate));
    if (m_hazeDetail != nullptr)
        m_hazeDetail->SetInnerRML(haze_detail(selected.hazeEnabled));
}

} // namespace sb::ui
