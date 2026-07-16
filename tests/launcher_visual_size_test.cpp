#include "shell/launcher/launcher_visual_size.h"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {
  bool check(bool value, std::string_view message) {
    if (!value) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return value;
  }

  bool near(float a, float b) { return std::abs(a - b) < 0.001f; }
} // namespace

int main() {
  bool ok = true;
  constexpr float chrome = 84.0f;
  constexpr float row = 56.0f;
  constexpr float gap = 4.0f;

  const float empty = launcher_visual::dynamicListHeight(chrome, row, gap, 0, 7);
  const float one = launcher_visual::dynamicListHeight(chrome, row, gap, 1, 7);
  const float three = launcher_visual::dynamicListHeight(chrome, row, gap, 3, 7);
  const float seven = launcher_visual::dynamicListHeight(chrome, row, gap, 7, 7);
  const float overflow = launcher_visual::dynamicListHeight(chrome, row, gap, 20, 7);

  ok &= check(near(empty, 112.0f), "empty provider overview stays compact");
  ok &= check(one > empty, "one provider grows beyond the empty state");
  ok &= check(three > one, "provider overview grows with result rows");
  ok &= check(near(overflow, seven), "provider overview caps at seven rows");
  ok &= check(overflow <= 540.0f, "provider overview respects launcher height cap");
  return ok ? 0 : 1;
}
