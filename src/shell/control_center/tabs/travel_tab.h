#pragma once

#include "shell/control_center/tab.h"
#include <chrono>
#include <string>

class Flex;
class Label;
class Slider;
class Button;
class Renderer;

class TravelTab : public Tab {
public:
  std::unique_ptr<Flex> create() override;
  void onClose() override;
  void setActive(bool active) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void refresh();
  void syncLabels();
  void toggleRadio();
  void applyThresholds();
  void chargeTo95();

  Flex* m_root = nullptr;
  Label* m_startLabel = nullptr;
  Label* m_stopLabel = nullptr;
  Label* m_status = nullptr;
  Label* m_feedback = nullptr;
  Slider* m_startSlider = nullptr;
  Slider* m_stopSlider = nullptr;
  Button* m_radio = nullptr;
  Button* m_apply = nullptr;
  Button* m_charge95 = nullptr;
  bool m_active = false;
  bool m_syncing = false;
  bool m_radioActive = false;
  bool m_charge95Active = false;
  int m_start = 55;
  int m_stop = 85;
  int m_realStart = 55;
  int m_realStop = 85;
  std::string m_message;
  std::chrono::steady_clock::time_point m_lastRefresh{};
};
