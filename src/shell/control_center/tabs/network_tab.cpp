#include "shell/control_center/tabs/network_tab.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_glyphs.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  std::string currentTitle(const NetworkState& s) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("control-center.network.wired-connection") : s.interfaceName;
    }
    return i18n::tr("control-center.network.not-connected");
  }

  std::string currentDetail(const NetworkState& s) {
    if (!s.connected) {
      return s.wirelessEnabled ? i18n::tr("control-center.network.wifi-on")
                               : i18n::tr("control-center.network.wifi-off");
    }
    std::string out;
    if (!s.ipv4.empty()) {
      out = s.ipv4;
    }
    if (s.kind == NetworkConnectivity::Wireless && s.signalStrength > 0) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += std::to_string(static_cast<int>(s.signalStrength)) + "%";
    }
    return out;
  }

  std::string percentText(std::uint8_t percent) { return std::to_string(static_cast<int>(percent)) + "%"; }

  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_glyphs::wifiSignalBand(a.strength);
      const int bandB = network_glyphs::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

class AccessPointRow : public Flex {
public:
  AccessPointRow(
      float scale, AccessPointInfo ap, bool saved, bool referenceLayout,
      std::function<void(const AccessPointInfo&)> onActivate,
      std::function<void(const AccessPointInfo&, Button*)> onMenu
  )
      : m_ap(std::move(ap)), m_referenceLayout(referenceLayout), m_onActivate(std::move(onActivate)),
        m_onMenu(std::move(onMenu)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    clearFill();
    clearBorder();

    if (referenceLayout) {
      addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary, m_ap.active ? 1.0f : 0.0f),
          .radius = 2.0f * scale,
          .width = 4.0f * scale,
          .height = 30.0f * scale,
      }));
    }

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = network_glyphs::wifiGlyphForSignal(m_ap.strength),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    auto titleBox = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0f,
    });

    titleBox->addChild(
        ui::label({
            .out = &m_title,
            .text = m_ap.ssid,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_ap.active ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    if (m_ap.secured) {
      titleBox->addChild(
          ui::glyph({
              .glyph = "lock",
              .glyphSize = (Style::baseGlyphSize - 4.0f) * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7f),
          })
      );
    }

    addChild(std::move(titleBox));

    addChild(
        ui::label({
            .out = &m_signalValue,
            .text = percentText(m_ap.strength),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    if (m_ap.active || saved) {
      const float actionOpacity = (m_ap.active || !referenceLayout) ? 1.0f : 0.0f;
      auto action = ui::button({
          .out = &m_actionButton,
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .opacity = actionOpacity,
      });
      if (m_ap.active) {
        action->setGlyph("check");
      } else {
        action->setGlyph(referenceLayout ? "more-vertical" : "trash");
        action->setOnClick([this]() {
          if (m_onMenu) {
            m_onMenu(m_ap, m_actionButton);
          }
        });
        action->setOnEnter([this]() {
          m_actionHovered = true;
          applyState();
        });
        action->setOnLeave([this]() {
          m_actionHovered = false;
          applyState();
        });
      }
      addChild(std::move(action));
    }

    auto area = std::make_unique<InputArea>();
    area->setPropagateEvents(true);
    area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnLeave([this]() { applyState(); });
    area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnClick([this](const InputArea::PointerData& /*data*/) {
      if (m_onActivate) {
        m_onActivate(m_ap);
      }
    });
    m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

    applyState();
    m_paletteConn = paletteChanged().connect([this] { applyState(); });
  }

  void doLayout(Renderer& renderer) override {
    if (m_inputArea == nullptr) {
      return;
    }
    m_inputArea->setVisible(false);
    Flex::doLayout(renderer);
    m_inputArea->setVisible(true);
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setSize(width(), height());
    if (m_actionButton != nullptr) {
      const float areaWidth = std::max(0.0f, m_actionButton->x() - gap());
      m_inputArea->setSize(areaWidth, height());
    }
    applyState();
  }

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    return measureByLayout(renderer, constraints);
  }

  void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  [[nodiscard]] const std::string& ssid() const noexcept { return m_ap.ssid; }

  // Refresh the values that move while a scan runs — the signal glyph and percent.
  // Everything else about the row is structural and belongs to the list key.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const AccessPointInfo& ap) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(network_glyphs::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(ap.strength))) {
      changed = true;
    }
    m_ap.strength = ap.strength;
    return changed;
  }

private:
  void applyState() {
    const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
    const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
    if (pressed) {
      setFill(colorSpecFromRole(ColorRole::Primary));
      setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
      if (m_signalGlyph != nullptr) {
        m_signalGlyph->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
      if (m_actionButton != nullptr && m_actionButton->glyph() != nullptr) {
        m_actionButton->glyph()->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
    } else if (m_ap.active) {
      setFill(colorSpecFromRole(ColorRole::Primary, 0.12f));
      setBorder(colorSpecFromRole(ColorRole::Primary, 0.32f), Style::borderWidth);
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::Primary));
      }
      if (m_signalGlyph != nullptr) {
        m_signalGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
      }
      if (m_actionButton != nullptr && m_actionButton->glyph() != nullptr) {
        m_actionButton->glyph()->setColor(colorSpecFromRole(ColorRole::Primary));
      }
    } else if (hov) {
      setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f));
      setBorder(colorSpecFromRole(ColorRole::Outline, 0.2f), Style::borderWidth);
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      if (m_signalGlyph != nullptr) {
        m_signalGlyph->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      if (m_actionButton != nullptr && m_actionButton->glyph() != nullptr) {
        m_actionButton->glyph()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
    } else {
      clearFill();
      clearBorder();
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      if (m_signalGlyph != nullptr) {
        m_signalGlyph->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
      if (m_actionButton != nullptr && m_actionButton->glyph() != nullptr) {
        m_actionButton->glyph()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      }
    }
    if (m_referenceLayout && m_actionButton != nullptr && !m_ap.active) {
      m_actionButton->setOpacity((hov || m_actionHovered) ? 1.0f : 0.0f);
    }
  }

  AccessPointInfo m_ap;
  bool m_referenceLayout = false;
  std::function<void(const AccessPointInfo&)> m_onActivate;
  std::function<void(const AccessPointInfo&, Button*)> m_onMenu;
  bool m_actionHovered = false;
  Label* m_title = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Glyph* m_signalGlyph = nullptr;
  Label* m_signalValue = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};

namespace {

  class VpnConnectionRow : public Flex {
  public:
    VpnConnectionRow(
        float scale, VpnConnectionInfo vpn, std::function<void(const VpnConnectionInfo&)> onActivate,
        std::function<void(const VpnConnectionInfo&)> onDeactivate
    )
        : m_vpn(std::move(vpn)), m_onActivate(std::move(onActivate)), m_onDeactivate(std::move(onDeactivate)) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      setMinHeight(kRowMinHeight * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      addChild(
          ui::label({
              .out = &m_title,
              .text = m_vpn.name,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = m_vpn.active ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0f,
          })
      );

      addChild(
          ui::button({
              .out = &m_checkButton,
              .glyph = "check",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .opacity = m_vpn.active ? 1.0f : 0.0f,
          })
      );

      addChild(
          ui::button({
              .out = &m_actionButton,
              .glyph = m_vpn.active ? "plug-off" : "plug",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = m_vpn.active ? ButtonVariant::Destructive : ButtonVariant::Default,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() { triggerAction(); },
          })
      );

      auto area = std::make_unique<InputArea>();
      area->setPropagateEvents(true);
      area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnLeave([this]() { applyState(); });
      area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnClick([this](const InputArea::PointerData& /*data*/) { triggerAction(); });
      m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

      applyState();
      m_paletteConn = paletteChanged().connect([this] { applyState(); });
    }

    void doLayout(Renderer& renderer) override {
      if (m_inputArea == nullptr) {
        return;
      }
      m_inputArea->setVisible(false);
      Flex::doLayout(renderer);
      m_inputArea->setVisible(true);
      m_inputArea->setPosition(0.0f, 0.0f);
      m_inputArea->setSize(width(), height());
      if (m_actionButton != nullptr) {
        const float areaWidth = std::max(0.0f, m_actionButton->x() - gap());
        m_inputArea->setSize(areaWidth, height());
      }
      applyState();
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  private:
    void triggerAction() {
      if (m_vpn.active) {
        if (m_onDeactivate) {
          m_onDeactivate(m_vpn);
        }
      } else {
        if (m_onActivate) {
          m_onActivate(m_vpn);
        }
      }
    }

    void applyState() {
      const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
      const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
      if (pressed) {
        setFill(colorSpecFromRole(ColorRole::Primary));
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        if (m_checkButton != nullptr && m_checkButton->glyph() != nullptr) {
          m_checkButton->glyph()->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        return;
      }

      if (m_vpn.active) {
        setFill(colorSpecFromRole(ColorRole::Primary, 0.08f));
        setBorder(colorSpecFromRole(ColorRole::Primary, 0.4f), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::Primary));
        }
        if (m_checkButton != nullptr && m_checkButton->glyph() != nullptr) {
          m_checkButton->glyph()->setColor(colorSpecFromRole(ColorRole::Primary));
        }
      } else {
        setFill(colorSpecFromRole(ColorRole::Surface));
        if (hov) {
          setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
        } else {
          clearBorder();
        }
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
        }
        if (m_checkButton != nullptr && m_checkButton->glyph() != nullptr) {
          m_checkButton->glyph()->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        }
      }
    }

    VpnConnectionInfo m_vpn;
    std::function<void(const VpnConnectionInfo&)> m_onActivate;
    std::function<void(const VpnConnectionInfo&)> m_onDeactivate;
    Label* m_title = nullptr;
    Button* m_checkButton = nullptr;
    Button* m_actionButton = nullptr;
    InputArea* m_inputArea = nullptr;
    Signal<>::ScopedConnection m_paletteConn;
  };

} // namespace

NetworkTab::NetworkTab(
    INetworkService* network, NetworkSecretAgent* secrets, ConfigService* config, WaylandConnection* wayland,
    RenderContext* renderContext, NetworkTabPresentation presentation
)
    : m_network(network), m_secrets(secrets), m_config(config), m_wayland(wayland), m_renderContext(renderContext),
      m_presentation(presentation) {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
}

NetworkTab::~NetworkTab() {
  closeContextMenu();
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

void NetworkTab::openContextMenu(
    Button* anchor, std::vector<ContextMenuControlEntry> entries,
    std::function<void(const ContextMenuControlEntry&)> onActivate
) {
  if (anchor == nullptr || m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }
  if (m_contextMenuPopup == nullptr) {
    m_contextMenuPopup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
  } else if (m_contextMenuPopup->isOpen()) {
    m_contextMenuPopup->close();
  }
  if (m_config != nullptr) {
    m_contextMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }

  float anchorX = 0.0f;
  float anchorY = 0.0f;
  Node::absolutePosition(anchor, anchorX, anchorY);
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_contextMenuPopup.get());
  m_contextMenuPopup->setOnActivate(std::move(onActivate));
  m_contextMenuPopup->setOnDismissed([this, parentSurface = parentCtx->surface]() {
    m_contextMenuOpen = false;
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });
  m_contextMenuPopup->open(ContextMenuPopupRequest{
      .entries = std::move(entries),
      .menuWidth = 246.0f * contentScale(),
      .maxVisible = 10,
      .anchor = PopupAnchorRect{
          .x = static_cast<std::int32_t>(anchorX),
          .y = static_cast<std::int32_t>(anchorY),
          .width = static_cast<std::int32_t>(anchor->width()),
          .height = static_cast<std::int32_t>(anchor->height()),
      },
      .parent = PopupSurfaceParent{
          .layerSurface = parentCtx->layerSurface,
          .output = parentCtx->output,
      },
  });
  m_contextMenuOpen = true;
}

void NetworkTab::openVpnMenu() {
  if (m_network == nullptr || m_vpnMenuButton == nullptr) {
    return;
  }
  const auto vpns = m_network->vpnConnections();
  std::vector<ContextMenuControlEntry> entries;
  if (vpns.empty()) {
    entries.push_back(ContextMenuControlEntry{
        .id = 0,
        .label = i18n::tr("control-center.network.no-vpn-profiles"),
        .enabled = false,
    });
  } else {
    entries.reserve(vpns.size());
    for (std::size_t i = 0; i < vpns.size(); ++i) {
      entries.push_back(ContextMenuControlEntry{
          .id = static_cast<std::int32_t>(i + 1),
          .label = vpns[i].name,
          .checkmark = vpns[i].active,
      });
    }
  }
  openContextMenu(
      m_vpnMenuButton, std::move(entries),
      [this, vpns](const ContextMenuControlEntry& entry) {
        if (m_network == nullptr || entry.id <= 0) {
          return;
        }
        const auto index = static_cast<std::size_t>(entry.id - 1);
        if (index >= vpns.size()) {
          return;
        }
        if (vpns[index].active) {
          m_network->deactivateVpnConnection(vpns[index]);
        } else {
          m_network->activateVpnConnection(vpns[index]);
        }
        PanelManager::instance().refresh();
      }
  );
}

void NetworkTab::openAccessPointMenu(const AccessPointInfo& ap, Button* anchor) {
  openContextMenu(
      anchor,
      {ContextMenuControlEntry{
          .id = 1,
          .label = i18n::tr("control-center.network.forget-network"),
      }},
      [this, ssid = ap.ssid](const ContextMenuControlEntry& entry) {
        if (entry.id == 1 && m_network != nullptr) {
          m_network->forgetSsid(ssid);
          PanelManager::instance().refresh();
        }
      }
  );
}

void NetworkTab::closeContextMenu() {
  if (m_contextMenuPopup != nullptr && m_contextMenuPopup->isOpen()) {
    m_contextMenuPopup->close();
  }
  m_contextMenuOpen = false;
}

void NetworkTab::openPasswordSheet() {
  if (m_passwordOverlay == nullptr || m_passwordCard == nullptr) {
    return;
  }
  m_passwordSheetOpen = true;
  m_passwordOverlay->setVisible(true);
  m_passwordCard->setVisible(true);
  setPasswordSheetProgress(m_passwordSheetProgress);
  if (m_passwordSheetAnimation != 0 && m_rootLayout != nullptr && m_rootLayout->animationManager() != nullptr) {
    m_rootLayout->animationManager()->cancel(m_passwordSheetAnimation);
    m_passwordSheetAnimation = 0;
  }
  if (m_rootLayout == nullptr || m_rootLayout->animationManager() == nullptr) {
    setPasswordSheetProgress(1.0f);
    return;
  }
  m_passwordSheetAnimation = m_rootLayout->animationManager()->animate(
      m_passwordSheetProgress, 1.0f, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float value) { setPasswordSheetProgress(value); },
      [this]() { m_passwordSheetAnimation = 0; }, m_passwordOverlay
  );
  PanelManager::instance().requestFrameTick();
}

void NetworkTab::closePasswordSheet(bool animated) {
  if (m_passwordOverlay == nullptr) {
    m_passwordSheetOpen = false;
    m_passwordSheetProgress = 0.0f;
    return;
  }
  m_passwordSheetOpen = false;
  if (m_passwordSheetAnimation != 0 && m_rootLayout != nullptr && m_rootLayout->animationManager() != nullptr) {
    m_rootLayout->animationManager()->cancel(m_passwordSheetAnimation);
    m_passwordSheetAnimation = 0;
  }
  const auto finish = [this]() {
    m_passwordSheetAnimation = 0;
    m_passwordSheetProgress = 0.0f;
    if (m_passwordOverlay != nullptr) {
      m_passwordOverlay->setVisible(false);
    }
  };
  if (!animated || m_rootLayout == nullptr || m_rootLayout->animationManager() == nullptr) {
    finish();
    return;
  }
  m_passwordSheetAnimation = m_rootLayout->animationManager()->animate(
      m_passwordSheetProgress, 0.0f, static_cast<float>(Style::animFast), Easing::EaseInCubic,
      [this](float value) { setPasswordSheetProgress(value); }, finish, m_passwordOverlay
  );
  PanelManager::instance().requestFrameTick();
}

void NetworkTab::setPasswordSheetProgress(float progress) {
  m_passwordSheetProgress = std::clamp(progress, 0.0f, 1.0f);
  if (m_passwordOverlay != nullptr) {
    m_passwordOverlay->setOpacity(m_passwordSheetProgress);
  }
  if (m_passwordCard != nullptr) {
    const float sheetScale = 0.98f + 0.02f * m_passwordSheetProgress;
    m_passwordCard->setScale(sheetScale);
  }
  layoutPasswordSheet();
  PanelManager::instance().requestRedraw();
}

void NetworkTab::layoutPasswordSheet() {
  if (m_passwordOverlay == nullptr) {
    return;
  }
  m_passwordOverlay->setPosition(0.0f, 0.0f);
  m_passwordOverlay->setSize(m_layoutWidth, m_layoutHeight);
  if (m_passwordScrim != nullptr) {
    m_passwordScrim->setPosition(0.0f, 0.0f);
    m_passwordScrim->setSize(m_layoutWidth, m_layoutHeight);
  }
  if (m_passwordCard == nullptr) {
    return;
  }
  const float inset = Style::spaceMd * contentScale();
  const float sheetHeight = std::min(250.0f * contentScale(), m_layoutHeight * 0.46f);
  m_passwordCard->setSize(std::max(1.0f, m_layoutWidth - inset * 2.0f), sheetHeight);
  m_passwordCard->setPosition(
      inset,
      m_layoutHeight - sheetHeight - inset + (1.0f - m_passwordSheetProgress) * sheetHeight * 0.24f
  );
}

std::unique_ptr<Flex> NetworkTab::create() {
  const float scale = contentScale();
  const bool referenceLayout = m_presentation == NetworkTabPresentation::ReferencePanel;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto currentCard = ui::column({
      .out = &m_currentCard,
      .align = FlexAlign::Stretch,
      .gap = referenceLayout ? Style::spaceSm * scale : 0.0f,
      .minHeight = 0.0f,
      .configure = [scale, referenceLayout, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        if (referenceLayout) {
          applySectionCardStyle(card, scale, opacity, borders);
        } else {
          applySeamlessSectionStyle(card, scale);
        }
      },
  });

  if (referenceLayout) {
    currentCard->addChild(ui::label({
        .text = i18n::tr("control-center.network.current-connection"),
        .fontSize = Style::fontSizeBody * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    }));
  } else {
    addTitle(*currentCard, i18n::tr("control-center.network.current-connection"), scale);
  }

  auto connRow = ui::row({
      .out = &m_currentRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });

  if (referenceLayout) {
    connRow->addChild(ui::column(
        {.out = &m_currentIconBubble,
         .align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.55f),
         .radius = 20.0f * scale,
         .width = 40.0f * scale,
         .height = 40.0f * scale},
        ui::glyph({
            .out = &m_currentGlyph,
            .glyph = "wifi-question",
            .glyphSize = 22.0f * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    ));
  }

  auto connectionDetails = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0f,
  });
  auto titleAndBadge = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  titleAndBadge->addChild(ui::label({
      .out = &m_currentTitle,
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .flexGrow = referenceLayout ? 1.0f : 0.0f,
  }));
  titleAndBadge->addChild(ui::row(
      {.out = &m_connectedBadge,
       .align = FlexAlign::Center,
       .paddingV = 2.0f * scale,
       .paddingH = 8.0f * scale,
       .fill = colorSpecFromRole(ColorRole::Primary, 0.12f),
       .radius = Style::scaledRadiusSm(scale),
       .border = colorSpecFromRole(ColorRole::Primary, 0.32f),
       .borderWidth = Style::borderWidth,
       .visible = false},
      ui::label({
          .text = i18n::tr("control-center.network.connected"),
          .fontSize = (Style::fontSizeCaption - 2.0f) * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  ));
  connectionDetails->addChild(std::move(titleAndBadge));
  connectionDetails->addChild(ui::label({
      .out = &m_currentDetail,
      .fontSize = Style::fontSizeCaption * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  connRow->addChild(std::move(connectionDetails));
  connRow->addChild(ui::button({
      .out = &m_disconnectButton,
      .glyph = "plug-off",
      .glyphSize = Style::baseGlyphSize * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .variant = ButtonVariant::Destructive,
      .tooltip = i18n::tr("control-center.network.disconnect"),
      .minWidth = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusSm(scale),
      .onClick = [this]() {
        if (m_network == nullptr || m_actionPending) {
          return;
        }
        const bool wasConnected = m_network->state().connected;
        if (wasConnected) {
          m_network->disconnect();
        } else if (!m_network->activateWiredConnection()) {
          return;
        }
        beginPendingAction(wasConnected);
        PanelManager::instance().refresh();
      },
  }));
  currentCard->addChild(std::move(connRow));

  tab->addChild(std::move(currentCard));

  auto passwordCard = ui::column({
      .out = &m_passwordCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .visible = referenceLayout,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
        card.setBorder(colorSpecFromRole(ColorRole::Primary, 0.5f), Style::borderWidth);
        card.setRadius(Style::scaledRadiusXl(scale));
      },
  });

  auto passwordHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  passwordHeader->addChild(ui::label({
          .out = &m_passwordTitle,
          .fontSize = (referenceLayout ? Style::fontSizeTitle : Style::fontSizeBody) * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }));
  if (referenceLayout) {
    passwordHeader->addChild(ui::button({
        .glyph = "x",
        .controlHeight = Style::controlHeightSm * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("common.actions.cancel"),
        .onClick = [this]() { cancelPasswordPrompt(); },
    }));
  }
  passwordCard->addChild(std::move(passwordHeader));

  auto inputRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });
  inputRow->addChild(ui::input({
          .out = &m_passwordInput,
          .placeholder = i18n::tr("control-center.network.password"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0f,
          .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
      }));
  inputRow->addChild(ui::button({
          .out = &m_passwordRevealButton,
          .glyph = "eye",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_passwordInput == nullptr) {
                  return;
                }
                m_passwordRevealed = !m_passwordRevealed;
                m_passwordInput->setPasswordMode(!m_passwordRevealed);
                if (m_passwordRevealButton != nullptr) {
                  m_passwordRevealButton->setGlyph(m_passwordRevealed ? "eye-off" : "eye");
                }
              },
      }));
  if (!referenceLayout) {
    inputRow->addChild(ui::button({
          .text = i18n::tr("control-center.network.connect"),
          .variant = ButtonVariant::Default,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() { submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{}); },
      }));
    inputRow->addChild(ui::button({
          .text = i18n::tr("common.actions.cancel"),
          .variant = ButtonVariant::Ghost,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { cancelPasswordPrompt(); },
      }));
  }

  passwordCard->addChild(std::move(inputRow));
  if (referenceLayout) {
    passwordCard->addChild(ui::row(
        {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm * scale, .fillWidth = true},
        ui::button({
            .text = i18n::tr("common.actions.cancel"),
            .variant = ButtonVariant::Ghost,
            .onClick = [this]() { cancelPasswordPrompt(); },
        }),
        ui::button({
            .text = i18n::tr("control-center.network.connect"),
            .variant = ButtonVariant::Primary,
            .onClick = [this]() {
              submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{});
            },
        })
    ));
  } else {
    tab->addChild(std::move(passwordCard));
  }

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);
  tab->addChild(std::move(listScroll));

  if (referenceLayout) {
    auto overlay = ui::column({
        .out = &m_passwordOverlay,
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::End,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::Shadow, 0.48f),
        .clipChildren = true,
        .visible = false,
        .participatesInLayout = false,
        .configure = [](Flex& node) { node.setZIndex(30); },
    });
    overlay->addChild(ui::inputArea({
        .out = &m_passwordScrim,
        .participatesInLayout = false,
        .onClick = [this](const InputArea::PointerData&) { cancelPasswordPrompt(); },
        .configure = [](InputArea& area) { area.setZIndex(0); },
    }));
    passwordCard->setZIndex(2);
    overlay->addChild(std::move(passwordCard));
    tab->addChild(std::move(overlay));
  }
  return tab;
}

std::unique_ptr<Flex> NetworkTab::createHeaderActions() { return nullptr; }

bool NetworkTab::dismissTransientUi() {
  if (m_contextMenuOpen) {
    closeContextMenu();
    return true;
  }
  if (m_hasPendingSecret || m_passwordSheetOpen) {
    cancelPasswordPrompt();
    return true;
  }
  return false;
}

void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (!m_active) {
    closeContextMenu();
    if (m_hasPendingSecret) {
      cancelPasswordPrompt();
    }
  }
  if (m_active && m_network != nullptr) {
    m_network->requestScan();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_layoutWidth = contentWidth;
  m_layoutHeight = bodyHeight;
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCurrentCard();
  m_rootLayout->layout(renderer);
  if (m_passwordOverlay != nullptr && m_passwordOverlay->visible()) {
    m_passwordOverlay->layout(renderer);
    layoutPasswordSheet();
  }
}

void NetworkTab::doPrepareIntrinsicLayout(Renderer& renderer, float contentWidth, float /*maxBodyHeight*/) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_layoutWidth = contentWidth;
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCurrentCard();
}

void NetworkTab::doUpdate(Renderer& renderer) {
  syncPasswordCard();
  rebuildApList(renderer);
  // A signal percent's text changes its width, so the list has to be laid out again.
  if (syncApRows() && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncCurrentCard();
}

void NetworkTab::onClose() {
  closeContextMenu();
  closePasswordSheet(false);
  m_rootLayout = nullptr;
  m_currentCard = nullptr;
  m_currentIconBubble = nullptr;
  m_currentGlyph = nullptr;
  m_currentTitle = nullptr;
  m_currentDetail = nullptr;
  m_connectedBadge = nullptr;
  m_passwordCard = nullptr;
  m_passwordOverlay = nullptr;
  m_passwordScrim = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_passwordRevealed = false;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_vpnMenuButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_currentRow = nullptr;
  m_disconnectButton = nullptr;
  m_apRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0f;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_actionPending = false;
  m_layoutWidth = 0.0f;
  m_layoutHeight = 0.0f;
}

void NetworkTab::syncPasswordCard() {
  if (m_passwordCard == nullptr) {
    return;
  }
  if (m_presentation == NetworkTabPresentation::ReferencePanel) {
    if (m_hasPendingSecret && !m_passwordSheetOpen) {
      openPasswordSheet();
    } else if (!m_hasPendingSecret && m_passwordSheetOpen) {
      closePasswordSheet(true);
    }
  } else {
    m_passwordCard->setVisible(m_hasPendingSecret);
  }
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    m_passwordTitle->setText(
        m_pendingSsid.empty() ? i18n::tr("control-center.network.password-prompt")
                              : i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid)
    );
  }
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  if (m_presentation == NetworkTabPresentation::ReferencePanel) {
    openPasswordSheet();
  }
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  if (m_presentation == NetworkTabPresentation::ReferencePanel) {
    openPasswordSheet();
  }
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
  if (m_presentation == NetworkTabPresentation::ReferencePanel && m_passwordSheetOpen) {
    closePasswordSheet(true);
  }
}

void NetworkTab::syncCurrentCard() {
  if (m_currentTitle == nullptr || m_currentDetail == nullptr) {
    return;
  }
  if (m_network == nullptr) {
    m_currentTitle->setText(i18n::tr("control-center.network.unavailable-title"));
    m_currentDetail->setText(i18n::tr("control-center.network.unavailable-detail"));
    if (m_currentRow != nullptr) {
      m_currentRow->setVisible(m_presentation == NetworkTabPresentation::ReferencePanel);
    }
    if (m_currentGlyph != nullptr) {
      m_currentGlyph->setGlyph("wifi-exclamation");
    }
    return;
  }
  const NetworkState& s = m_network->state();
  if (m_actionPending) {
    const bool flipped = s.connected != m_actionPendingConnected;
    const bool timedOut = std::chrono::steady_clock::now() - m_actionPendingSince > std::chrono::seconds(6);
    if (flipped || timedOut) {
      m_actionPending = false;
    }
  }
  m_currentTitle->setText(currentTitle(s));
  m_currentDetail->setText(currentDetail(s));
  if (m_currentRow != nullptr) {
    m_currentRow->setVisible(true);
  }

  if (m_currentGlyph != nullptr) {
    if (s.connected && s.kind == NetworkConnectivity::Wireless) {
      m_currentGlyph->setGlyph(network_glyphs::wifiGlyphForSignal(s.signalStrength));
    } else if (s.connected && s.kind == NetworkConnectivity::Wired) {
      m_currentGlyph->setGlyph("network");
    } else {
      m_currentGlyph->setGlyph(s.wirelessEnabled ? "wifi-question" : "wifi-exclamation");
    }
    m_currentGlyph->setColor(colorSpecFromRole(s.connected ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
  }
  if (m_currentIconBubble != nullptr) {
    m_currentIconBubble->setFill(colorSpecFromRole(
        s.connected ? ColorRole::Primary : ColorRole::SurfaceVariant, s.connected ? 0.10f : 0.45f
    ));
  }

  if (m_currentCard != nullptr) {
    if (s.connected) {
      m_currentCard->setFill(colorSpecFromRole(
          ColorRole::Primary, m_presentation == NetworkTabPresentation::ReferencePanel ? 0.04f : 0.08f
      ));
      m_currentCard->setBorder(colorSpecFromRole(ColorRole::Primary, 0.32f), Style::borderWidth);
      m_currentTitle->setColor(colorSpecFromRole(ColorRole::Primary));
    } else {
      if (m_presentation == NetworkTabPresentation::ReferencePanel) {
        applySectionCardStyle(
            *m_currentCard, contentScale(), panelCardOpacity(), panelBordersEnabled()
        );
      } else {
        applySeamlessSectionStyle(*m_currentCard, contentScale());
      }
      m_currentTitle->setColor(colorSpecFromRole(ColorRole::OnSurface));
    }
  }
  if (m_connectedBadge != nullptr) {
    m_connectedBadge->setVisible(s.connected);
  }

  if (m_disconnectButton != nullptr) {
    const bool canReconnectWired = !s.connected && m_network->canActivateWiredConnection();
    m_disconnectButton->setVisible(s.connected || canReconnectWired || m_actionPending);
    m_disconnectButton->setGlyph(s.connected ? "plug-off" : "plug");
    m_disconnectButton->setVariant(s.connected ? ButtonVariant::Destructive : ButtonVariant::Default);
    m_disconnectButton->setEnabled(!m_actionPending);
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(s.wirelessEnabled);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_rescanButton != nullptr) {
    m_rescanButton->setEnabled(s.wirelessEnabled && !s.scanning);
  }
  if (m_vpnMenuButton != nullptr) {
    m_vpnMenuButton->setSelected(s.vpnActive);
  }
}

void NetworkTab::beginPendingAction(bool wasConnected) {
  m_actionPending = true;
  m_actionPendingConnected = wasConnected;
  m_actionPendingSince = std::chrono::steady_clock::now();
  if (m_disconnectButton != nullptr) {
    m_disconnectButton->setEnabled(false);
  }
}

// Identity of the built list: which rows exist, in which order, and which controls
// each carries. The signal strength is absent by design — it refreshes in place
// through syncApRows(), so a scan update no longer tears the list down. Access points
// arrive sorted, so a change in row order changes the key.
std::string
NetworkTab::structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const {
  std::string key;
  for (const auto& ap : aps) {
    key += ap.ssid;
    key.push_back(':');
    key += ap.secured ? '1' : '0';
    key.push_back(':');
    key += ap.active ? '1' : '0';
    key.push_back(':');
    key += (m_network != nullptr && m_network->hasSavedConnection(ap.ssid)) ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& vpn : vpns) {
    key += vpn.path;
    key.push_back(':');
    key += vpn.name;
    key.push_back(':');
    key += vpn.active ? '1' : '0';
    key.push_back('\n');
  }
  const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
  const bool scanning = m_network != nullptr && m_network->state().scanning;
  key += "vis:";
  key += m_vpnVisible ? '1' : '0';
  key += "\nwifi:";
  key += wirelessEnabled ? '1' : '0';
  key += "\nscan:";
  key += scanning ? '1' : '0';
  return key;
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr) {
    return;
  }
  const float listWidth = (m_listScroll != nullptr && m_listScroll->contentViewportWidth() > 0.0f)
      ? m_listScroll->contentViewportWidth()
      : (m_list != nullptr && m_list->width() > 0.0f ? m_list->width() : m_layoutWidth);
  if (listWidth <= 0.0f) {
    return;
  }

  std::vector<AccessPointInfo> aps;
  if (m_network != nullptr) {
    aps = sortedAccessPoints(m_network->accessPoints());
  }
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : std::vector<VpnConnectionInfo>{};
  const std::string nextStructure = structureKey(aps, vpns);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;
  const float scale = contentScale();
  const bool referenceLayout = m_presentation == NetworkTabPresentation::ReferencePanel;

  auto buildApRows = [&]() {
    const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
    const std::string emptyText = wirelessEnabled ? i18n::tr("control-center.network.no-networks")
                                                  : i18n::tr("control-center.network.wifi-off");
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });
    if (aps.empty() || (referenceLayout && !wirelessEnabled)) {
      if (referenceLayout) {
        container->addChild(ui::column(
            {.align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .gap = Style::spaceSm * scale,
             .padding = Style::spaceLg * 2.0f * scale,
             .minHeight = 152.0f * scale},
            ui::glyph({
                .glyph = wirelessEnabled ? "wifi-question" : "wifi-exclamation",
                .glyphSize = 34.0f * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
              .text = emptyText,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        ));
      } else {
        container->addChild(ui::label({
            .text = emptyText,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }));
      }
    } else {
      for (std::size_t i = 0; i < aps.size(); ++i) {
        const auto& ap = aps[i];
        const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
        auto row = std::make_unique<AccessPointRow>(
            scale, ap, saved, referenceLayout,
            [this](const AccessPointInfo& clicked) {
              if (clicked.active || m_network == nullptr) {
                return;
              }
              if (clicked.secured && !m_network->hasSavedConnection(clicked.ssid)) {
                showPasswordPrompt(clicked);
                PanelManager::instance().refresh();
                return;
              }
              m_network->activateAccessPoint(clicked);
            },
            [this, referenceLayout](const AccessPointInfo& clicked, Button* anchor) {
              if (referenceLayout) {
                openAccessPointMenu(clicked, anchor);
              } else {
                if (m_network != nullptr) {
                  m_network->forgetSsid(clicked.ssid);
                }
                PanelManager::instance().refresh();
              }
            }
        );
        auto* rowPtr = row.get();
        container->addChild(std::move(row));
        m_apRows.emplace(rowPtr->ssid(), rowPtr);
        if (referenceLayout && i + 1 < aps.size()) {
          container->addChild(ui::box({
              .fill = colorSpecFromRole(ColorRole::Outline, 0.16f),
              .height = Style::borderWidth,
          }));
        }
      }
    }
    return container;
  };

  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_vpnMenuButton = nullptr;
  m_apRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_network == nullptr) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.network.unavailable-title"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  } else {
    const float opacity = panelCardOpacity();
    const bool borders = panelBordersEnabled();

    if (referenceLayout) {
      auto wifiHeader = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .fillWidth = true,
      });
      wifiHeader->addChild(ui::label({
          .text = i18n::tr("control-center.network.wifi"),
          .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }));
      wifiHeader->addChild(ui::button({
          .out = &m_vpnMenuButton,
          .glyph = "shield",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.network.vpns"),
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() { openVpnMenu(); },
      }));
      wifiHeader->addChild(ui::spinner({
          .out = &m_scanSpinner,
          .color = colorSpecFromRole(ColorRole::Primary),
          .spinnerSize = Style::baseGlyphSize * scale,
          .visible = false,
      }));
      wifiHeader->addChild(ui::button({
          .out = &m_rescanButton,
          .glyph = "refresh",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.network.scan"),
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_network != nullptr) {
              m_network->requestScan();
            }
          },
      }));
      wifiHeader->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Outline, 0.22f),
          .width = Style::borderWidth,
          .height = 28.0f * scale,
      }));
      wifiHeader->addChild(ui::toggle({
          .out = &m_wifiToggle,
          .checkedImmediate = m_network->state().wirelessEnabled,
          .toggleSize = ToggleSize::Medium,
          .scale = scale,
          .onChange = [this](bool checked) {
            if (m_network != nullptr) {
              m_network->setWirelessEnabled(checked);
            }
          },
      }));
      m_list->addChild(std::move(wifiHeader));

      auto wifiCard = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .configure = [scale, opacity, borders](Flex& card) {
            applySectionCardStyle(card, scale, opacity, borders);
            card.setPadding(Style::spaceXs * scale);
            card.setRadius(Style::scaledRadiusLg(scale));
          },
      });
      wifiCard->addChild(buildApRows());
      m_list->addChild(std::move(wifiCard));
      m_list->layout(renderer);
      return;
    }

    if (!vpns.empty()) {
      auto vpnCard = ui::column({
          .configure = [scale](Flex& card) { applySeamlessSectionStyle(card, scale); },
      });

      auto vpnHeader = makeCardHeaderRow(i18n::tr("control-center.network.vpns"), scale);
      vpnHeader->addChild(
          ui::toggle({
              .checkedImmediate = m_vpnVisible,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) {
                m_vpnVisible = checked;
                PanelManager::instance().refresh();
              },
          })
      );
      vpnCard->addChild(std::move(vpnHeader));

      if (m_vpnVisible) {
        for (const auto& vpn : vpns) {
          auto row = std::make_unique<VpnConnectionRow>(
              scale, vpn,
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->activateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              },
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->deactivateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              }
          );
          vpnCard->addChild(std::move(row));
        }
      }

      m_list->addChild(std::move(vpnCard));
    }

    {
      auto wifiCard = ui::column({
          .configure = [scale](Flex& card) { applySeamlessSectionStyle(card, scale); },
      });

      auto wifiHeader = makeCardHeaderRow(i18n::tr("control-center.network.wifi"), scale);
      wifiHeader->addChild(
          ui::spinner({
              .out = &m_scanSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
              .visible = false,
          })
      );

      wifiHeader->addChild(
          ui::button({
              .out = &m_rescanButton,
              .glyph = "refresh",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_network != nullptr) {
                  m_network->requestScan();
                }
              },
          })
      );

      wifiHeader->addChild(
          ui::toggle({
              .out = &m_wifiToggle,
              .checkedImmediate = m_network->state().wirelessEnabled,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) {
                if (m_network != nullptr) {
                  m_network->setWirelessEnabled(checked);
                }
              },
          })
      );
      wifiCard->addChild(std::move(wifiHeader));

      wifiCard->addChild(buildApRows());

      m_list->addChild(std::move(wifiCard));

      // Live state (spinner visibility/animation, toggle checked) is owned by
      // syncCurrentCard(), which runs every frame after the card is attached.
      // rebuildApList() builds structure only and must not drive animations.
    }
  }
  m_list->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it != m_apRows.end() && it->second->syncLiveMetrics(ap)) {
      changed = true;
    }
  }
  return changed;
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
}
