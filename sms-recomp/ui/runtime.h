#pragma once

#include <memory>

struct AuroraEvent;

namespace sb::ui {

class SettingsMenu;

using RuntimeCallback = void (*)();
using RuntimePredicate = bool (*)();

class Runtime {
public:
  ~Runtime();

  bool initialize();
  void shutdown();
  bool handle_events(const AuroraEvent *events);
  bool pause_while_open(bool &frameActive, RuntimePredicate quitRequested,
                        RuntimeCallback beforePresent);

  void toggle();
  bool visible() const noexcept;
  bool layout_valid() const;

private:
  SettingsMenu *menu() noexcept;

  std::unique_ptr<SettingsMenu> m_menu;
};

Runtime &runtime();

} // namespace sb::ui
