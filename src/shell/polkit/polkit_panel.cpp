#include "shell/polkit/polkit_panel.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "dbus/polkit/polkit_agent.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>

namespace {

  int wrappedLineCount(std::string_view text, int charsPerLine, int maxLines) {
    if (text.empty()) {
      return 0;
    }
    int lines = 0;
    int col = 0;
    for (char ch : text) {
      if (ch == '\n') {
        ++lines;
        col = 0;
        if (lines >= maxLines) {
          return maxLines;
        }
        continue;
      }
      ++col;
      if (charsPerLine > 0 && col > charsPerLine) {
        ++lines;
        col = 1;
        if (lines >= maxLines) {
          return maxLines;
        }
      }
    }
    if (col > 0 || lines == 0) {
      ++lines;
    }
    return std::min(lines, maxLines);
  }

  std::string wrapLongRuns(std::string text, std::size_t maxRun = 48) {
    std::string out;
    out.reserve(text.size() + text.size() / maxRun);
    std::size_t run = 0;
    for (char ch : text) {
      const bool breakable = std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '/' || ch == ':' || ch == '-';
      out.push_back(ch);
      if (breakable) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= maxRun) {
        out.push_back('\n');
        run = 0;
      }
    }
    return out;
  }

} // namespace

PolkitPanel::PolkitPanel(ConfigService* config, std::function<PolkitAgent*()> agentProvider)
    : m_config(config), m_agentProvider(std::move(agentProvider)) {}

PanelPlacement PolkitPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.polkitPlacement : PanelPlacement::Floating;
}


float PolkitPanel::preferredHeight() const {
  return scaled(36.0f + 2.0f * Style::panelPadding);
}

void PolkitPanel::create() {
  const float scale = contentScale();

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .padding = 0.0f,
  });

  auto focusArea = std::make_unique<InputArea>();
  focusArea->setFocusable(true);
  focusArea->setVisible(false);
  m_focusArea = static_cast<InputArea*>(
      root->addChild(std::move(focusArea))
  );

  auto input = ui::input({
      .out = &m_input,
      .placeholder = "",
      .controlHeight = scaled(34.0f),
      .passwordMode = true,
          .frameVisible = false,
      .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0f,
      .onSubmit = [this](const std::string&) {
        submit();
      },
      .onKeyEvent = [this](
          std::uint32_t sym,
          std::uint32_t modifiers
      ) {
        return handleInputKeyEvent(sym, modifiers);
      },
  });

  m_input->setPasswordGlyphsVisible(false);
  m_input->setCaretVisible(false);

const float opacity = panelCardOpacity();
  const bool borders = panelBordersEnabled();

  auto field = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingH = scaled(10.0f),
      .height = scaled(36.0f),
      .configure = [scale, opacity, borders](Flex& row) {
        row.setCardStyle(scale, opacity, borders);
      },
  });

  field->addChild(ui::label({
      .text = "Password",
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));

  field->addChild(std::move(input));

  field->addChild(ui::glyph({
      .glyph = "lock",
      .glyphSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));

  root->addChild(std::move(field));
  setRoot(std::move(root));
}

void PolkitPanel::onOpen(std::string_view /*context*/) {
  m_lastResponseRequired = false;
  m_iconResolved = false;
  if (m_input != nullptr) {
    m_input->setValue("");
  }
}

void PolkitPanel::onClose() {
  if (PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr; agent != nullptr) {
    if (agent->hasPendingRequest()) {
      agent->cancelRequest();
    }
  }
  m_lastResponseRequired = false;
  clearReleasedRoot();

  m_rootLayout = nullptr;
  m_focusArea = nullptr;
  m_titleLabel = nullptr;
  m_messageLabel = nullptr;
  m_promptLabel = nullptr;
  m_supplementaryLabel = nullptr;
  m_input = nullptr;
  m_submitButton = nullptr;
  m_cancelButton = nullptr;
  m_iconContainer = nullptr;
  m_icon = nullptr;
  m_fallbackIcon = nullptr;
}

InputArea* PolkitPanel::initialFocusArea() const {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent != nullptr && !agent->isResponseRequired()) {
    return m_focusArea;
  }
  return m_input != nullptr ? m_input->inputArea() : m_focusArea;
}

void PolkitPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  if (m_iconContainer != nullptr) {
    if (m_icon != nullptr && m_icon->visible()) {
      m_icon->setSize(m_iconContainer->width(), m_iconContainer->height());
      m_icon->setPosition(0.0f, 0.0f);
    }
    if (m_fallbackIcon != nullptr && m_fallbackIcon->visible()) {
      const float ox = std::round((m_iconContainer->width() - m_fallbackIcon->width()) * 0.5f);
      const float oy = std::round((m_iconContainer->height() - m_fallbackIcon->height()) * 0.5f);
      m_fallbackIcon->setPosition(ox, oy);
    }
  }
}

void PolkitPanel::doUpdate(Renderer& /*renderer*/) {
  PolkitAgent* agent =
      m_agentProvider != nullptr ? m_agentProvider() : nullptr;

  if (agent == nullptr || m_input == nullptr) {
    return;
  }

  const bool needsInput = agent->isResponseRequired();

  m_input->setVisible(needsInput);
  m_input->setInvalid(agent->supplementaryIsError());

  if (needsInput && !m_lastResponseRequired) {
    if (auto* manager = PanelManager::current();
        manager != nullptr && manager->isOpenPanel("polkit")) {
      manager->focusArea(m_input->inputArea());
    }
  }

  m_lastResponseRequired = needsInput;
}

void PolkitPanel::resolveIcon(Renderer& renderer, const PolkitRequest& request) {
  const float iconSize = scaled(48.0f);
  if (m_iconContainer != nullptr) {
    m_iconContainer->setSize(iconSize, iconSize);
  }

  if (request.isInternal) {
    const auto logoPath = paths::assetPath("noctalia.svg");
    m_fallbackIcon->setVisible(false);
    m_icon->setSize(iconSize, iconSize);
    m_icon->setSourceFile(renderer, logoPath.string(), static_cast<int>(std::round(iconSize)), true);
    m_icon->setVisible(true);
    return;
  }

  if (!request.iconName.empty()) {
    const std::string& resolved = m_iconResolver.resolve(request.iconName, static_cast<int>(std::round(iconSize)));
    if (!resolved.empty()) {
      m_fallbackIcon->setVisible(false);
      m_icon->setSize(iconSize, iconSize);
      m_icon->setSourceFile(renderer, resolved, static_cast<int>(std::round(iconSize)), true);
      m_icon->setVisible(true);
      return;
    }
  }

  m_icon->clear(renderer);
  m_icon->setVisible(false);
  m_fallbackIcon->setVisible(true);
}

void PolkitPanel::submit() {
  PolkitAgent* agent = m_agentProvider != nullptr ? m_agentProvider() : nullptr;
  if (agent == nullptr || m_input == nullptr) {
    return;
  }
  agent->submitResponse(m_input->value());
  m_input->setValue("");
}

bool PolkitPanel::handleInputKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    submit();
    return true;
  }
  const bool shift = (modifiers & KeyMod::Shift) != 0;
  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretLeft(shift);
    }
    return true;
  }
  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (m_input != nullptr) {
      m_input->moveCaretRight(shift);
    }
    return true;
  }
  return false;
}

void PolkitPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
}
