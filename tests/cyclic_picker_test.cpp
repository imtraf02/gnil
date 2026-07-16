#include "shell/wallpaper/cyclic_picker.h"

#include <iostream>

namespace {
bool check(bool value, const char* message) {
  if (!value) std::cerr << "FAIL: " << message << '\n';
  return value;
}
}

int main() {
  bool ok = true;
  CyclicPicker picker;
  ok &= check(!picker.next() && picker.count() == 0, "empty picker is inert");
  picker.setCount(1);
  ok &= check(picker.logicalIndex() == 0 && picker.physicalIndex() == 1, "one item starts in middle strip");
  ok &= check(picker.next() && picker.logicalIndex() == 0 && picker.physicalIndex() == 2, "one item moves into copy");
  ok &= check(picker.rebase() && picker.physicalIndex() == 1, "one item rebases after copy");
  picker.setCount(3);
  ok &= check(picker.logicalIndex() == 0 && picker.physicalIndex() == 3, "multi item starts middle");
  picker.previous();
  ok &= check(picker.logicalIndex() == 2 && picker.physicalIndex() == 2, "previous crosses first edge continuously");
  ok &= check(picker.rebase() && picker.physicalIndex() == 5, "previous edge rebases into middle strip");
  picker.next();
  ok &= check(picker.logicalIndex() == 0 && picker.physicalIndex() == 6, "next crosses last edge continuously");
  ok &= check(picker.rebase() && picker.physicalIndex() == 3, "next edge rebases into middle strip");
  ok &= check(picker.selectPhysical(8), "physical copy can be selected directly");
  ok &= check(picker.logicalIndex() == 2 && picker.physicalIndex() == 8, "physical selection preserves its strip");
  ok &= check(picker.rebase() && picker.physicalIndex() == 5, "clicked outer copy rebases after settling");
  return ok ? 0 : 1;
}
