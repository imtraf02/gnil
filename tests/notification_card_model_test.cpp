#include "shell/notification/notification_card_model.h"
#include "shell/sidebar/sidebar_edge_gesture.h"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      std::exit(1);
    }
  }

  Notification makeNotification(uint32_t id, std::string app, std::optional<std::string> desktop = std::nullopt) {
    Notification notification{};
    notification.id = id;
    notification.appName = std::move(app);
    notification.desktopEntry = std::move(desktop);
    notification.summary = "Summary";
    notification.body = "Body";
    notification.actions = {"inline-reply", "Reply", "open", "Open"};
    return notification;
  }
} // namespace

int main() {
  using notification_card::GestureAction;
  expect(
      notification_card::classifyGesture(119.0f, 0.0f, 400.0f, false, 0.3f, 20.0f)
          == GestureAction::None,
      "dismiss fired below the configured width fraction"
  );
  expect(
      notification_card::classifyGesture(120.0f, 0.0f, 400.0f, false, 0.3f, 20.0f)
          == GestureAction::Dismiss,
      "dismiss did not fire at the configured width fraction"
  );
  expect(
      notification_card::classifyGesture(0.0f, 20.0f, 400.0f, false, 0.3f, 20.0f)
          == GestureAction::Expand,
      "downward expand threshold failed"
  );
  expect(
      notification_card::classifyGesture(0.0f, -20.0f, 400.0f, true, 0.3f, 20.0f)
          == GestureAction::Collapse,
      "upward collapse threshold failed"
  );
  expect(!sidebar_edge_gesture::revealReached(8.0f, -71.0f, 80.0f), "edge reveal fired below threshold");
  expect(sidebar_edge_gesture::revealReached(8.0f, -72.0f, 80.0f), "edge reveal missed threshold");
  expect(!sidebar_edge_gesture::revealReached(0.0f, 80.0f, 80.0f), "outward edge drag opened sidebar");

  Notification imageNotification = makeNotification(1, "Chat", "chat.desktop");
  imageNotification.imageData = NotificationImageData{.width = 1, .height = 1, .rowStride = 4, .data = {0, 0, 0, 0}};
  const auto compact = notification_card::presentationFor(imageNotification, false);
  const auto expanded = notification_card::presentationFor(imageNotification, true);
  expect(compact.bodyMaxLines == 1 && !compact.hasLargeImage && !compact.hasActions, "compact model leaked details");
  expect(expanded.hasLargeImage && expanded.hasActions && expanded.hasInlineReply, "expanded model missed content");

  std::deque<Notification> active;
  active.push_back(makeNotification(1, "Chat", "chat.desktop"));
  active.push_back(makeNotification(2, "Mail", "mail.desktop"));
  active.push_back(makeNotification(3, "Chat", "chat.desktop"));
  active.push_back(makeNotification(4, "Chat", "chat.desktop"));
  active.push_back(makeNotification(5, "Chat", "chat.desktop"));

  auto groups = notification_card::groupActive(active, 3);
  expect(groups.size() == 2, "active notifications were not grouped by desktop entry");
  expect(groups[0].label == "Chat" && groups[0].items.front()->id == 5, "newest group/item ordering is wrong");
  expect(groups[0].items.size() == 4 && groups[0].visibleCount == 3, "collapsed preview count is wrong");

  std::unordered_set<std::string> open{groups[0].key};
  groups = notification_card::groupActive(active, 3, open);
  expect(groups[0].expanded && groups[0].visibleCount == 4, "expanded group did not reveal every active item");
  return 0;
}
