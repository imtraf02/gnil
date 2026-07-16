#include "shell/sidebar/sidebar_panel.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <ranges>

namespace {
  std::string relativeTime(const Notification& notification) {
    return notification.receivedWallClock.has_value() ? formatTimeAgo(*notification.receivedWallClock)
                                                      : formatElapsedSince(notification.receivedTime);
  }

  std::string actionLabel(std::string_view key, std::string_view label) {
    if (!StringUtils::isBlank(label)) {
      return std::string(label);
    }
    if (key == "inline-reply") {
      return i18n::tr("notifications.inline-reply.button");
    }
    return i18n::tr("notifications.actions.fallback");
  }
} // namespace

SidebarPanel::SidebarPanel(ConfigService* config, NotificationManager* notifications)
    : m_config(config), m_notifications(notifications) {}

void SidebarPanel::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::panelPadding * scale,
  });

  root->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceSm * scale},
      ui::label({
          .text = "Notifications",
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::button({
              .out = &m_dndButton,
              .glyph = m_notifications != nullptr && m_notifications->doNotDisturb() ? "bell-off" : "bell",
              .tooltip = i18n::tr("control-center.notifications.dnd-on"),
              .onClick = [this]() {
                if (m_notifications != nullptr) {
                  (void)m_notifications->toggleDoNotDisturb();
                  markContentDirty();
                }
              },
              .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
          }),
          ui::button({
              .out = &m_clearAllButton,
              .glyph = "trash",
              .tooltip = i18n::tr("control-center.notifications.clear-all"),
              .onClick = [this]() { clearAll(); },
              .configure = [scale](Button& button) {
                panel_button_style::configureHeaderIconButton(button, scale);
                button.setVariant(ButtonVariant::Destructive);
              },
          }),
          ui::button({
              .glyph = "close",
              .tooltip = i18n::tr("common.close"),
              .onClick = []() { PanelManager::instance().closePanel(); },
              .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
          })
      )
  ));

  auto scroll = ui::scrollView({
      .state = &m_scrollState,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
  });
  m_scroll = scroll.get();
  m_content = scroll->content();
  m_content->setDirection(FlexDirection::Vertical);
  m_content->setAlign(FlexAlign::Stretch);
  m_content->setGap(Style::spaceMd * scale);
  root->addChild(std::move(scroll));
  setRoot(std::move(root));
  m_contentDirty = true;
}

void SidebarPanel::onClose() {
  endReply(true);
  m_root = nullptr;
  m_content = nullptr;
  m_scroll = nullptr;
  m_dndButton = nullptr;
  m_clearAllButton = nullptr;
  m_expandedNotifications.clear();
  m_lastSerial = 0;
  m_contentDirty = true;
  clearReleasedRoot();
}

bool SidebarPanel::dismissTransientUi() {
  if (m_replyNotificationId.has_value()) {
    endReply(true);
    markContentDirty();
    return true;
  }
  if (!m_expandedNotifications.empty()) {
    m_expandedNotifications.erase(m_expandedNotifications.begin());
    markContentDirty();
    return true;
  }
  if (!m_expandedGroups.empty()) {
    m_expandedGroups.erase(m_expandedGroups.begin());
    markContentDirty();
    return true;
  }
  return false;
}

void SidebarPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr) {
    return;
  }
  rebuildContent(renderer);
  m_root->setSize(width, height);
  m_root->layout(renderer);
}

void SidebarPanel::doUpdate(Renderer& renderer) { rebuildContent(renderer); }

void SidebarPanel::markContentDirty() {
  m_contentDirty = true;
  PanelManager::instance().refresh();
}

void SidebarPanel::rebuildContent(Renderer& renderer) {
  if (m_content == nullptr) {
    return;
  }
  const std::uint64_t serial = m_notifications != nullptr ? m_notifications->changeSerial() : 0;
  if (!m_contentDirty && serial == m_lastSerial) {
    return;
  }

  while (!m_content->children().empty()) {
    m_content->removeChild(m_content->children().back().get());
  }

  const std::size_t previewCount = m_config != nullptr
      ? static_cast<std::size_t>(std::max(1, m_config->config().notification.groupPreviewCount))
      : 3;
  const auto groups = m_notifications != nullptr
      ? notification_card::groupActive(m_notifications->all(), previewCount, m_expandedGroups)
      : std::vector<notification_card::Group>{};

  if (m_clearAllButton != nullptr) {
    m_clearAllButton->setVisible(!groups.empty());
  }
  if (m_dndButton != nullptr) {
    const bool dnd = m_notifications != nullptr && m_notifications->doNotDisturb();
    m_dndButton->setGlyph(dnd ? "bell-off" : "bell");
    m_dndButton->setSelected(dnd);
    m_dndButton->setTooltip(i18n::tr(
        dnd ? "control-center.notifications.dnd-off" : "control-center.notifications.dnd-on"
    ));
  }

  if (groups.empty()) {
    const float scale = contentScale();
    m_content->addChild(ui::column(
        {
            .align = FlexAlign::Center,
            .gap = Style::spaceSm * scale,
            .padding = Style::spaceLg * 2.0f * scale,
            .configure = [this, scale](Flex& empty) {
              empty.setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
            },
        },
        ui::glyph({
            .glyph = m_notifications != nullptr && m_notifications->doNotDisturb() ? "bell-off" : "notifications",
            .glyphSize = 32.0f * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .text = i18n::tr("control-center.notifications.empty-title"),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .text = i18n::tr("control-center.notifications.empty-body"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    ));
  } else {
    for (const auto& group : groups) {
      m_content->addChild(buildGroup(renderer, group));
    }
  }

  m_lastSerial = serial;
  m_contentDirty = false;
  m_content->markLayoutDirty();
}

std::unique_ptr<Flex> SidebarPanel::buildGroup(Renderer& renderer, const notification_card::Group& group) {
  const float scale = contentScale();
  auto column = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  const std::string key = group.key;

  column->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceSm * scale},
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::label({
              .text = group.label,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .text = std::to_string(group.items.size()),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      ),
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::button({
              .glyph = group.expanded ? "expand_less" : "expand_more",
              .visible = group.items.size() > group.visibleCount || group.expanded,
              .onClick = [this, key]() { toggleGroup(key); },
              .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
          }),
          ui::button({
              .glyph = "close",
              .tooltip = i18n::tr("control-center.notifications.clear-all"),
              .onClick = [this, key]() { clearGroup(key); },
              .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
          })
      )
  ));

  for (std::size_t i = 0; i < group.visibleCount; ++i) {
    if (group.items[i] != nullptr) {
      column->addChild(buildCard(renderer, *group.items[i]));
    }
  }
  return column;
}

std::unique_ptr<Flex> SidebarPanel::buildCard(Renderer& renderer, const Notification& notification) {
  const float scale = contentScale();
  const bool expanded = m_expandedNotifications.contains(notification.id);
  const auto presentation = notification_card::presentationFor(notification, expanded);
  const uint32_t id = notification.id;
  const ColorSpec accent = notification.urgency == Urgency::Critical ? colorSpecFromRole(ColorRole::Error)
                                                                     : colorSpecFromRole(ColorRole::Primary);

  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = Style::spaceMd * scale,
      .configure = [this, scale, critical = notification.urgency == Urgency::Critical](Flex& flex) {
        flex.setCardStyle(scale, panelCardOpacity(), panelBordersEnabled());
        if (critical) {
          flex.setFill(colorSpecFromRole(ColorRole::Error, 0.12f));
          flex.setBorder(colorSpecFromRole(ColorRole::Error, 0.6f), Style::borderWidth);
        }
      },
  });

  card->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({.glyph = "notifications", .glyphSize = 24.0f * scale, .color = accent}),
      ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0f},
          ui::label({
              .text = StringUtils::trimLeadingBlankLines(notification.summary),
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
          }),
          ui::label({
              .text = relativeTime(notification),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      ),
      ui::button({
          .glyph = expanded ? "expand_less" : "expand_more",
          .onClick = [this, id]() { toggleNotification(id); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  ));

  if (presentation.hasLargeImage) {
    const auto& image = *notification.imageData;
    const bool valid = image.width > 0 && image.height > 0 && !image.data.empty() && image.bitsPerSample == 8
        && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
    if (valid) {
      const float previewWidth = 344.0f * scale;
      const float aspect = static_cast<float>(image.height) / static_cast<float>(image.width);
      const float previewHeight = std::clamp(previewWidth * aspect, 120.0f * scale, 220.0f * scale);
      auto preview = ui::image({
          .fit = ImageFit::Cover,
          .radius = Style::scaledRadiusLg(scale),
          .width = previewWidth,
          .height = previewHeight,
      });
      const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
      if (preview->setSourceRaw(
              renderer, image.data.data(), image.data.size(), image.width, image.height, image.rowStride, format, true
          )) {
        card->addChild(std::move(preview));
      }
    }
  }

  if (!StringUtils::isBlank(notification.body)) {
    card->addChild(ui::label({
        .text = StringUtils::trimLeadingBlankLines(notification.body),
        .fontSize = Style::fontSizeCaption * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = presentation.bodyMaxLines,
    }));
  }

  if (presentation.hasActions) {
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
    const std::size_t limit = std::min(notification.actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      const std::string key = notification.actions[i];
      if (key.empty() || key == "default") {
        continue;
      }
      actions->addChild(ui::button({
          .text = actionLabel(key, notification.actions[i + 1]),
          .fontSize = Style::fontSizeCaption * scale,
          .onClick = [this, id, key]() {
            if (key == "inline-reply") {
              beginReply(id);
            } else if (m_notifications != nullptr) {
              (void)m_notifications->invokeAction(id, key, true);
            }
          },
      }));
    }
    card->addChild(std::move(actions));
  }

  if (m_replyNotificationId == id) {
    card->addChild(ui::input({
        .placeholder = i18n::tr("notifications.inline-reply.placeholder"),
        .fontSize = Style::fontSizeCaption * scale,
        .frameVisible = true,
        .onSubmit = [this, id](const std::string& text) { submitReply(id, text); },
    }));
  }

  return card;
}

void SidebarPanel::clearAll() {
  if (m_notifications == nullptr) {
    return;
  }
  std::vector<uint32_t> ids;
  for (const auto& notification : m_notifications->all()) {
    ids.push_back(notification.id);
  }
  for (uint32_t id : ids) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_expandedGroups.clear();
  m_expandedNotifications.clear();
  m_replyNotificationId.reset();
  markContentDirty();
}

void SidebarPanel::clearGroup(std::string key) {
  if (m_notifications == nullptr) {
    return;
  }
  std::vector<uint32_t> ids;
  for (const auto& notification : m_notifications->all()) {
    if (notification_card::groupKey(notification) == key) {
      ids.push_back(notification.id);
    }
  }
  for (uint32_t id : ids) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
    m_expandedNotifications.erase(id);
  }
  if (m_replyNotificationId.has_value() && std::ranges::contains(ids, *m_replyNotificationId)) {
    m_replyNotificationId.reset();
  }
  m_expandedGroups.erase(key);
  markContentDirty();
}

void SidebarPanel::toggleGroup(std::string key) {
  if (!m_expandedGroups.erase(key)) {
    m_expandedGroups.insert(std::move(key));
  }
  markContentDirty();
}

void SidebarPanel::toggleNotification(uint32_t id) {
  if (!m_expandedNotifications.erase(id)) {
    m_expandedNotifications.insert(id);
  } else if (m_replyNotificationId == id) {
    endReply(true);
  }
  markContentDirty();
}

void SidebarPanel::beginReply(uint32_t id) {
  if (m_replyNotificationId.has_value() && m_replyNotificationId != id) {
    endReply(true);
  }
  m_expandedNotifications.insert(id);
  m_replyNotificationId = id;
  if (m_notifications != nullptr) {
    m_notifications->pauseExpiry(id);
  }
  markContentDirty();
}

void SidebarPanel::endReply(bool resumeTimeout) {
  if (!m_replyNotificationId.has_value()) {
    return;
  }
  const uint32_t id = *m_replyNotificationId;
  m_replyNotificationId.reset();
  if (!resumeTimeout || m_notifications == nullptr) {
    return;
  }
  const auto it = std::ranges::find(m_notifications->all(), id, &Notification::id);
  if (it != m_notifications->all().end() && it->timeout > 0) {
    m_notifications->resumeExpiry(id, it->timeout);
  }
}

void SidebarPanel::submitReply(uint32_t id, const std::string& text) {
  bool submitted = false;
  if (m_notifications != nullptr && !StringUtils::isBlank(text)) {
    submitted = m_notifications->invokeInlineReply(id, text, true);
  }
  endReply(!submitted);
  markContentDirty();
}
