#include "shell/notification/notification_card_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace notification_card {
  namespace {
    bool blank(std::string_view value) {
      return std::ranges::all_of(value, [](unsigned char c) { return std::isspace(c) != 0; });
    }

    bool hasInlineReply(const std::vector<std::string>& actions) {
      for (std::size_t i = 0; i + 1 < actions.size(); i += 2) {
        if (actions[i] == "inline-reply") {
          return true;
        }
      }
      return false;
    }
  } // namespace

  GestureAction classifyGesture(
      float deltaX, float deltaY, float cardWidth, bool expanded, float clearThreshold, float expandThreshold
  ) noexcept {
    const float dismissDistance = std::max(1.0f, cardWidth) * std::clamp(clearThreshold, 0.0f, 1.0f);
    // Decimal config fractions such as 0.3 are not exact in float. Keep the
    // configured boundary inclusive instead of making 120 / 400 miss by a few
    // millionths after multiplication.
    constexpr float thresholdEpsilon = 0.001f;
    if (std::abs(deltaX) + thresholdEpsilon >= dismissDistance && std::abs(deltaX) > std::abs(deltaY)) {
      return GestureAction::Dismiss;
    }
    const float verticalDistance = std::max(1.0f, expandThreshold);
    if (!expanded && deltaY >= verticalDistance) {
      return GestureAction::Expand;
    }
    if (expanded && deltaY <= -verticalDistance) {
      return GestureAction::Collapse;
    }
    return GestureAction::None;
  }

  Presentation presentationFor(const Notification& notification, bool expanded) noexcept {
    return Presentation{
        .expanded = expanded,
        .hasLargeImage = expanded && notification.imageData.has_value(),
        .hasActions = expanded && !notification.actions.empty(),
        .hasInlineReply = expanded && hasInlineReply(notification.actions),
        .bodyMaxLines = expanded ? 500 : 1,
    };
  }

  std::string groupKey(const Notification& notification) {
    if (notification.desktopEntry.has_value() && !blank(*notification.desktopEntry)) {
      return "desktop:" + *notification.desktopEntry;
    }
    if (!blank(notification.appName)) {
      return "app:" + notification.appName;
    }
    return "system";
  }

  std::vector<Group> groupActive(
      const std::deque<Notification>& active, std::size_t previewCount,
      const std::unordered_set<std::string>& expandedGroups
  ) {
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> indices;
    groups.reserve(active.size());

    for (auto it = active.rbegin(); it != active.rend(); ++it) {
      const Notification& notification = *it;
      const std::string key = groupKey(notification);
      auto found = indices.find(key);
      if (found == indices.end()) {
        const std::size_t index = groups.size();
        indices.emplace(key, index);
        std::string label = !blank(notification.appName) ? notification.appName : std::string("System");
        groups.push_back(Group{
            .key = key,
            .label = std::move(label),
            .items = {},
            .visibleCount = 0,
            .expanded = expandedGroups.contains(key),
        });
        found = indices.find(key);
      }
      groups[found->second].items.push_back(&notification);
    }

    const std::size_t collapsedLimit = std::max<std::size_t>(1, previewCount);
    for (auto& group : groups) {
      group.visibleCount = group.expanded ? group.items.size() : std::min(group.items.size(), collapsedLimit);
    }
    return groups;
  }

} // namespace notification_card
