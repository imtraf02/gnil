#pragma once

#include <cstddef>

// Keeps a logical selection in [0,count), while exposing a physical index in
// the middle of three identical strips. Moving across either logical edge
// therefore animates to an adjacent physical copy; callers rebase only after
// the animation has completed, when the replacement is visually identical.
class CyclicPicker {
public:
  void setCount(std::size_t count) noexcept {
    m_count = count;
    m_logical = count == 0 ? 0 : m_logical % count;
    m_physical = count == 0 ? 0 : count + m_logical;
  }

  [[nodiscard]] std::size_t count() const noexcept { return m_count; }
  [[nodiscard]] std::size_t logicalIndex() const noexcept { return m_logical; }
  [[nodiscard]] std::size_t physicalIndex() const noexcept { return m_physical; }

  bool select(std::size_t index) noexcept {
    if (m_count == 0) return false;
    const std::size_t next = index % m_count;
    if (next == m_logical) return false;
    m_logical = next;
    m_physical = m_count + next;
    return true;
  }

  // Adopt a clicked item from any of the three physical strips without
  // teleporting it back into the middle strip first.
  bool selectPhysical(std::size_t index) noexcept {
    if (m_count == 0 || index >= m_count * 3) return false;
    const bool changed = index != m_physical;
    m_physical = index;
    m_logical = index % m_count;
    return changed;
  }

  bool next() noexcept { return move(1); }
  bool previous() noexcept { return move(-1); }

  bool move(int delta) noexcept {
    if (m_count == 0 || delta == 0) return false;
    if (delta > 0) {
      m_logical = (m_logical + 1) % m_count;
      ++m_physical;
    } else {
      m_logical = (m_logical + m_count - 1) % m_count;
      --m_physical;
    }
    return true;
  }

  // Returns true if the caller must update physical positions without a
  // transition. It is safe only after the movement animation settles.
  bool rebase() noexcept {
    if (m_count == 0) return false;
    if (m_physical < m_count) {
      m_physical += m_count;
      return true;
    }
    if (m_physical >= m_count * 2) {
      m_physical -= m_count;
      return true;
    }
    return false;
  }

private:
  std::size_t m_count = 0;
  std::size_t m_logical = 0;
  std::size_t m_physical = 0;
};
