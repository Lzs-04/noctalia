#include "shell/control_center/tabs/travel_tab.h"

#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <glib.h>

namespace {
  int readThreshold(const char* path, int fallback) {
    std::ifstream in(path);
    int value = fallback;
    return (in >> value) ? value : fallback;
  }

  std::string homeDir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string{};
  }

  bool airplaneState() {
    return std::filesystem::exists(homeDir() + "/.local/state/noctalia/travel-radios.json");
  }

  bool spawn(std::initializer_list<std::string> strings) {
    std::vector<std::string> args(strings);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    GError* error = nullptr;
    const gboolean ok = g_spawn_async(nullptr, argv.data(), nullptr,
        G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
    if (error != nullptr) g_error_free(error);
    return ok != FALSE;
  }
}

std::unique_ptr<Flex> TravelTab::create() {
  const float s = contentScale();
  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * s,
  });
  auto scroll = ui::scrollView({
      .scrollbarVisible = true,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& view) { view.clearFill(); view.clearBorder(); },
  });
  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * s);

  auto connectivity = ui::column({
      .configure = [s, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        control_center::applySectionCardStyle(card, s, opacity, borders);
      },
  });
  connectivity->addChild(control_center::makeCardHeaderRow("Airplane Mode", s));
  connectivity->addChild(ui::label({
      .text = "Disable radios except Bluetooth",
      .fontSize = Style::fontSizeBody * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  connectivity->addChild(ui::button({
      .out = &m_radio,
      .text = "Airplane Mode: OFF",
      .glyph = "plane",
      .variant = ButtonVariant::Default,
      .onClick = [this]() { toggleRadio(); },
  }));
  content->addChild(std::move(connectivity));

  auto battery = ui::column({
      .configure = [s, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        control_center::applySectionCardStyle(card, s, opacity, borders);
      },
  });
  battery->addChild(control_center::makeCardHeaderRow("Battery charging limits", s));
  battery->addChild(ui::label({
      .out = &m_startLabel,
      .text = "Start charging: --",
      .fontSize = Style::fontSizeBody * s,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  battery->addChild(ui::slider({
      .out = &m_startSlider,
      .minValue = 0.0,
      .maxValue = 99.0,
      .step = 1.0,
      .value = static_cast<double>(m_start),
      .onValueChanged = [this](double x) {
        if (m_syncing) return;
        m_start = std::clamp(static_cast<int>(std::lround(x)), 0, 99);
        if (m_start >= m_stop) m_stop = std::min(100, m_start + 1);
        syncLabels();
      },
  }));
  battery->addChild(ui::label({
      .out = &m_stopLabel,
      .text = "Stop charging: --",
      .fontSize = Style::fontSizeBody * s,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  battery->addChild(ui::slider({
      .out = &m_stopSlider,
      .minValue = 1.0,
      .maxValue = 100.0,
      .step = 1.0,
      .value = static_cast<double>(m_stop),
      .onValueChanged = [this](double x) {
        if (m_syncing) return;
        m_stop = std::clamp(static_cast<int>(std::lround(x)), 1, 100);
        if (m_stop <= m_start) m_start = std::max(0, m_stop - 1);
        syncLabels();
      },
  }));
  battery->addChild(ui::label({
      .out = &m_status,
      .text = "",
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  battery->addChild(ui::button({
      .out = &m_apply,
      .text = "Save charging limits",
      .glyph = "device-floppy",
      .variant = ButtonVariant::Default,
      .onClick = [this]() { applyThresholds(); },
  }));
  battery->addChild(ui::button({
      .out = &m_charge95,
      .text = "Charge now to 95%",
      .glyph = "battery-charging",
      .variant = ButtonVariant::Default,
      .onClick = [this]() { chargeTo95(); },
  }));
  content->addChild(std::move(battery));
  tab->addChild(std::move(scroll));
  refresh();
  return tab;
}

void TravelTab::syncLabels() {
  m_syncing = true;
  if (m_startSlider != nullptr && !m_startSlider->dragging())
    m_startSlider->setValue(static_cast<double>(m_start));
  if (m_stopSlider != nullptr && !m_stopSlider->dragging())
    m_stopSlider->setValue(static_cast<double>(m_stop));
  m_syncing = false;
  if (m_startLabel != nullptr) m_startLabel->setText("Start charging: " + std::to_string(m_start) + "%");
  if (m_stopLabel != nullptr) m_stopLabel->setText("Stop charging: " + std::to_string(m_stop) + "%");
  if (m_radio != nullptr) {
    m_radio->setText(m_radioActive ? "Airplane Mode: ON" : "Airplane Mode: OFF");
    m_radio->setVariant(m_radioActive ? ButtonVariant::TabActive : ButtonVariant::Default);
  }
  if (m_status != nullptr) {
    m_status->setText("Applied: " + std::to_string(m_realStart) + "% / " +
        std::to_string(m_realStop) + "%" + (m_message.empty() ? "" : "  |  " + m_message));
  }
  if (m_apply != nullptr) {
    m_apply->setEnabled(std::filesystem::exists("/usr/local/libexec/noctalia-travel-charge"));
  }
  if (m_charge95 != nullptr) {
    const int capacity = readThreshold("/sys/class/power_supply/BAT0/capacity", -1);
    m_charge95->setEnabled(capacity >= 0 && capacity < 95 &&
        std::filesystem::exists("/usr/local/libexec/noctalia-travel-charge95"));
    m_charge95->setText(m_realStart == 0 && m_realStop == 95
        ? "Charge to 95%: active" : "Charge now to 95%");
  }
}

void TravelTab::refresh() {
  m_realStart = readThreshold("/sys/class/power_supply/BAT0/charge_control_start_threshold", m_realStart);
  m_realStop = readThreshold("/sys/class/power_supply/BAT0/charge_control_end_threshold", m_realStop);
  if (m_lastRefresh == std::chrono::steady_clock::time_point{} ||
      (m_start == m_realStart && m_stop == m_realStop)) {
    m_start = m_realStart;
    m_stop = m_realStop;
  }
  m_radioActive = airplaneState();
  m_lastRefresh = std::chrono::steady_clock::now();
  syncLabels();
}

void TravelTab::toggleRadio() {
  auto script = homeDir() + "/.config/noctalia/scripts/travel-radios.py";
  if (!std::filesystem::exists(script)) {
    m_message = "Radio helper not installed";
    syncLabels();
    return;
  }
  if (spawn({"/usr/bin/python3", script, "toggle"})) {
    m_message = "Applying radio state...";
  } else {
    m_message = "Could not launch radio helper";
  }
  syncLabels();
}

void TravelTab::applyThresholds() {
  if (m_start >= m_stop || m_start < 0 || m_stop > 100) {
    m_message = "Invalid charging limits";
    syncLabels();
    return;
  }
  if (!std::filesystem::exists("/usr/local/libexec/noctalia-travel-charge")) {
    m_message = "Install the root-owned charging helper first";
    syncLabels();
    return;
  }
  if (spawn({"pkexec", "/usr/local/libexec/noctalia-travel-charge",
             std::to_string(m_start), std::to_string(m_stop)})) {
    m_message = "Authorization requested; refresh after applying";
  } else {
    m_message = "Could not launch authorization";
  }
  syncLabels();
}

void TravelTab::chargeTo95() {
  const std::string helper = "/usr/local/libexec/noctalia-travel-charge95";
  if (!std::filesystem::exists(helper)) {
    m_message = "Charge-to-95% helper not installed";
  } else if (spawn({"pkexec", helper})) {
    m_message = "Authorization requested; charge to 95% will start after approval";
  } else {
    m_message = "Could not launch charge-to-95% authorization";
  }
  syncLabels();
}

void TravelTab::setActive(bool active) {
  if (active && !m_active) refresh();
  m_active = active;
}

void TravelTab::doUpdate(Renderer&) {
  if (m_active && std::chrono::steady_clock::now() - m_lastRefresh > std::chrono::seconds(2)) refresh();
}

void TravelTab::doLayout(Renderer& renderer, float width, float height) {
  if (m_root != nullptr) { m_root->setSize(width, height); m_root->layout(renderer); }
}

void TravelTab::onClose() {
  m_root = nullptr;
  m_startLabel = nullptr;
  m_stopLabel = nullptr;
  m_status = nullptr;
  m_startSlider = nullptr;
  m_stopSlider = nullptr;
  m_radio = nullptr;
  m_apply = nullptr;
  m_charge95 = nullptr;
  m_active = false;
  m_lastRefresh = {};
}
