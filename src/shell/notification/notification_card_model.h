#pragma once

#include "notification/notification.h"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace notification_card {

  enum class GestureAction {
    None,
    Dismiss,
    Expand,
    Collapse,
  };

  struct Presentation {
    bool expanded = false;
    bool hasLargeImage = false;
    bool hasActions = false;
    bool hasInlineReply = false;
    int bodyMaxLines = 1;
  };

  struct Group {
    std::string key;
    std::string label;
    std::vector<const Notification*> items;
    std::size_t visibleCount = 0;
    bool expanded = false;
  };

  [[nodiscard]] GestureAction classifyGesture(
      float deltaX, float deltaY, float cardWidth, bool expanded, float clearThreshold, float expandThreshold
  ) noexcept;

  [[nodiscard]] Presentation presentationFor(const Notification& notification, bool expanded) noexcept;
  [[nodiscard]] std::string groupKey(const Notification& notification);
  [[nodiscard]] std::vector<Group> groupActive(
      const std::deque<Notification>& active, std::size_t previewCount,
      const std::unordered_set<std::string>& expandedGroups = {}
  );

} // namespace notification_card
