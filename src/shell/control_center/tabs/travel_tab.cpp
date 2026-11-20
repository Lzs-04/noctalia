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
  constexpr const char* kCharge95State = "/run/noctalia-travel-charge95.state";

  int readThreshold(const char* path, int fallback) {
    std::ifstream in(path);
    int value = fallback;
    return (in >> value) ? value : fallback;
  }

  bool readCharge95Original(int& start, int& stop) {
    std::ifstream in(kCharge95State);
    int savedStart = -1;
    int savedStop = -1;
    if (!(in >> savedStart >> savedStop) ||
        savedStart < 0 || savedStart >= savedStop || savedStop > 100) return false;
    start = savedStart;
    stop = savedStop;
    return true;
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
      .text = "Choose your normal charging range",
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
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
        if (m_syncing || m_charge95Active) return;
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
        if (m_syncing || m_charge95Active) return;
        m_stop = std::clamp(static_cast<int>(std::lround(x)), 1, 100);
        if (m_stop <= m_start) m_start = std::max(0, m_stop - 1);
        syncLabels();
      },
  }));
  battery->addChild(ui::button({
      .out = &m_apply,
      .text = "Save limits",
      .glyph = "device-floppy",
      .controlHeight = Style::controlHeight * s,
      .variant = ButtonVariant::Primary,
      .onClick = [this]() { applyThresholds(); },
  }));
  battery->addChild(ui::label({
      .out = &m_status,
      .text = "",
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  battery->addChild(ui::label({
      .text = "One-time charge",
      .fontSize = Style::fontSizeBody * s,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  battery->addChild(ui::label({
      .text = "Charge to 95%, then return to your normal limits when safe",
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  battery->addChild(ui::button({
      .out = &m_charge95,
      .text = "Charge to 95%",
      .glyph = "battery-charging",
      .controlHeight = Style::controlHeight * s,
      .variant = ButtonVariant::Outline,
      .onClick = [this]() { chargeTo95(); },
  }));
  battery->addChild(ui::label({
      .out = &m_feedback,
      .text = "",
      .fontSize = Style::fontSizeCaption * s,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
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
  if (m_startSlider != nullptr) m_startSlider->setEnabled(!m_charge95Active);
  if (m_stopSlider != nullptr) m_stopSlider->setEnabled(!m_charge95Active);
  m_syncing = false;
  if (m_startLabel != nullptr) m_startLabel->setText("Start charging: " + std::to_string(m_start) + "%");
  if (m_stopLabel != nullptr) m_stopLabel->setText("Stop charging: " + std::to_string(m_stop) + "%");
  if (m_radio != nullptr) {
    m_radio->setText(m_radioActive ? "Airplane Mode: ON" : "Airplane Mode: OFF");
    m_radio->setVariant(m_radioActive ? ButtonVariant::TabActive : ButtonVariant::Default);
  }
  if (m_status != nullptr) {
    if (m_charge95Active) {
      m_status->setText("Temporary charge active  -  Target 95%");
    } else if (m_realStart >= m_realStop) {
      m_status->setText("System reports " + std::to_string(m_realStart) + "% / " +
          std::to_string(m_realStop) + "%  -  Check charging limits");
    } else {
      m_status->setText("Applied limits: " + std::to_string(m_realStart) + "% - " +
          std::to_string(m_realStop) + "%");
    }
  }
  if (m_feedback != nullptr) {
    m_feedback->setText(m_charge95Active
        ? "Normal limits return when safe, or after unplugging"
        : m_message);
  }
  if (m_apply != nullptr) {
    m_apply->setEnabled(!m_charge95Active &&
        std::filesystem::exists("/usr/local/libexec/noctalia-travel-charge"));
  }
  if (m_charge95 != nullptr) {
    const int capacity = readThreshold("/sys/class/power_supply/BAT0/capacity", -1);
    m_charge95->setEnabled(!m_charge95Active && capacity >= 0 && capacity < 95 &&
        std::filesystem::exists("/usr/local/libexec/noctalia-travel-charge95"));
    m_charge95->setText(m_charge95Active ? "Charging to 95%" : "Charge to 95%");
  }
}

void TravelTab::refresh() {
  m_realStart = readThreshold("/sys/class/power_supply/BAT0/charge_control_start_threshold", m_realStart);
  m_realStop = readThreshold("/sys/class/power_supply/BAT0/charge_control_end_threshold", m_realStop);
  const bool wasCharge95Active = m_charge95Active;
  m_charge95Active = std::filesystem::exists(kCharge95State);
  if (wasCharge95Active && !m_charge95Active)
    m_message = "Temporary charging session ended";
  if (m_charge95Active) {
    int savedStart = m_start;
    int savedStop = m_stop;
    if (readCharge95Original(savedStart, savedStop)) {
      m_start = savedStart;
      m_stop = savedStop;
    }
  } else if (m_realStart < m_realStop &&
      (m_lastRefresh == std::chrono::steady_clock::time_point{} ||
       wasCharge95Active ||
       (m_start == m_realStart && m_stop == m_realStop))) {
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
    m_message = "Waiting for authorization to save limits";
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
    m_message = "Waiting for authorization to charge to 95%";
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
  m_feedback = nullptr;
  m_startSlider = nullptr;
  m_stopSlider = nullptr;
  m_radio = nullptr;
  m_apply = nullptr;
  m_charge95 = nullptr;
  m_active = false;
  m_charge95Active = false;
  m_lastRefresh = {};
}
