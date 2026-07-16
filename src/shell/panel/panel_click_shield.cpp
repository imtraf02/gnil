#include "shell/panel/panel_click_shield.h"

#include "core/log.h"
#include "viewporter-client-protocol.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace {

  constexpr Logger kLog("panel-click-shield");

  int createAnonFd(std::size_t size) {
    const int fd = memfd_create("gnil-click-shield", MFD_CLOEXEC);
    if (fd < 0) {
      return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  const zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = &PanelClickShield::handleConfigure,
      .closed = &PanelClickShield::handleClosed,
  };

} // namespace

PanelClickShield::~PanelClickShield() {
  deactivate();
  if (m_buffer != nullptr) {
    wl_buffer_destroy(m_buffer);
    m_buffer = nullptr;
  }
}

void PanelClickShield::initialize(WaylandConnection& wayland) { m_wayland = &wayland; }

bool PanelClickShield::ensureSharedBuffer() {
  if (m_buffer != nullptr) {
    return true;
  }
  if (m_wayland == nullptr || m_wayland->shm() == nullptr) {
    return false;
  }

  constexpr std::int32_t kWidth = 1;
  constexpr std::int32_t kHeight = 1;
  constexpr std::int32_t kStride = kWidth * 4;
  constexpr auto kSize = static_cast<std::size_t>(kStride * kHeight);

  const int fd = createAnonFd(kSize);
  if (fd < 0) {
    kLog.warn("failed to create shared buffer: {}", std::strerror(errno));
    return false;
  }

  wl_shm_pool* pool = wl_shm_create_pool(m_wayland->shm(), fd, static_cast<std::int32_t>(kSize));
  close(fd);
  if (pool == nullptr) {
    return false;
  }

  m_buffer = wl_shm_pool_create_buffer(pool, 0, kWidth, kHeight, kStride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  return m_buffer != nullptr;
}

void PanelClickShield::activate(
    const std::vector<wl_output*>& outputs, LayerShellLayer layer, ExcludeProvider excludeProvider
) {
  deactivate();
  m_layer = layer;
  m_excludeProvider = std::move(excludeProvider);
  syncOutputs(outputs);
}

void PanelClickShield::syncOutputs(const std::vector<wl_output*>& outputs) {
  if (m_wayland == nullptr || !m_layer.has_value()) {
    return;
  }
  if (m_wayland->layerShell() == nullptr || m_wayland->compositor() == nullptr) {
    return;
  }
  if (m_wayland->viewporter() == nullptr) {
    kLog.warn("disabled: wp_viewporter is unavailable");
    return;
  }
  if (!ensureSharedBuffer()) {
    kLog.warn("disabled: transparent SHM buffer is unavailable");
    return;
  }

  std::unordered_set<wl_output*> liveOutputs;
  liveOutputs.reserve(outputs.size());
  for (wl_output* output : outputs) {
    if (output != nullptr) {
      liveOutputs.insert(output);
    }
  }

  for (auto it = m_shields.begin(); it != m_shields.end();) {
    if (liveOutputs.contains(it->first)) {
      ++it;
      continue;
    }
    if (it->second != nullptr) {
      destroyShield(*it->second);
    }
    it = m_shields.erase(it);
  }

  for (wl_output* output : liveOutputs) {
    std::vector<InputRect> excludeRects;
    if (m_excludeProvider) {
      excludeRects = m_excludeProvider(output);
    }

    if (const auto existing = m_shields.find(output); existing != m_shields.end()) {
      existing->second->excludeRects = std::move(excludeRects);
      if (existing->second->configured) {
        applyInputRegion(*existing->second);
        wl_surface_commit(existing->second->surface);
      }
      continue;
    }

    auto shield = createShield(output, std::move(excludeRects));
    if (shield != nullptr) {
      m_shields.emplace(output, std::move(shield));
    }
  }
}

std::unique_ptr<PanelClickShield::Shield>
PanelClickShield::createShield(wl_output* output, std::vector<InputRect> excludeRects) {
  auto shield = std::make_unique<Shield>();
  shield->owner = this;
  shield->output = output;
  shield->excludeRects = std::move(excludeRects);

  shield->surface = wl_compositor_create_surface(m_wayland->compositor());
  if (shield->surface == nullptr) {
    return nullptr;
  }
  shield->viewport = wp_viewporter_get_viewport(m_wayland->viewporter(), shield->surface);
  shield->layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      m_wayland->layerShell(), shield->surface, output, static_cast<std::uint32_t>(*m_layer), "gnil-panel-click-shield"
  );
  if (shield->layerSurface == nullptr || shield->viewport == nullptr) {
    if (shield->viewport != nullptr) {
      wp_viewport_destroy(shield->viewport);
    }
    wl_surface_destroy(shield->surface);
    return nullptr;
  }

  zwlr_layer_surface_v1_add_listener(shield->layerSurface, &kLayerSurfaceListener, shield.get());
  zwlr_layer_surface_v1_set_anchor(
      shield->layerSurface,
      LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right
  );
  zwlr_layer_surface_v1_set_size(shield->layerSurface, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(shield->layerSurface, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      shield->layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
  );

  // An empty region keeps the unconfigured 1x1 surface inert until Niri gives
  // us the output's logical size.
  if (wl_region* emptyRegion = wl_compositor_create_region(m_wayland->compositor()); emptyRegion != nullptr) {
    wl_surface_set_input_region(shield->surface, emptyRegion);
    wl_region_destroy(emptyRegion);
  }
  wl_surface_commit(shield->surface);
  return shield;
}

void PanelClickShield::deactivate() {
  for (auto& shield : m_shields | std::views::values) {
    if (shield != nullptr) {
      destroyShield(*shield);
    }
  }
  m_shields.clear();
  m_layer.reset();
  m_excludeProvider = {};
}

void PanelClickShield::destroyShield(Shield& shield) {
  if (shield.viewport != nullptr) {
    wp_viewport_destroy(shield.viewport);
    shield.viewport = nullptr;
  }
  if (shield.layerSurface != nullptr) {
    zwlr_layer_surface_v1_destroy(shield.layerSurface);
    shield.layerSurface = nullptr;
  }
  if (shield.surface != nullptr) {
    if (m_wayland != nullptr) {
      m_wayland->unregisterSurface(shield.surface);
    }
    wl_surface_destroy(shield.surface);
    shield.surface = nullptr;
  }
}

bool PanelClickShield::ownsSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return false;
  }
  return std::ranges::any_of(m_shields | std::views::values, [surface](const auto& shield) {
    return shield != nullptr && shield->surface == surface;
  });
}

void PanelClickShield::handleConfigure(
    void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
) {
  zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
  auto* shield = static_cast<Shield*>(data);
  if (shield != nullptr && shield->owner != nullptr) {
    shield->owner->applyConfigured(*shield, width, height);
  }
}

void PanelClickShield::handleClosed(void* data, zwlr_layer_surface_v1* /*layerSurface*/) {
  auto* shield = static_cast<Shield*>(data);
  if (shield == nullptr || shield->owner == nullptr) {
    return;
  }
  PanelClickShield* owner = shield->owner;
  const auto it = owner->m_shields.find(shield->output);
  if (it != owner->m_shields.end() && it->second.get() == shield) {
    owner->destroyShield(*it->second);
    owner->m_shields.erase(it);
  }
}

void PanelClickShield::applyConfigured(Shield& shield, std::uint32_t width, std::uint32_t height) {
  if (shield.surface == nullptr) {
    return;
  }
  shield.width = static_cast<std::int32_t>(width);
  shield.height = static_cast<std::int32_t>(height);

  if (!shield.bufferAttached && m_buffer != nullptr) {
    wl_surface_attach(shield.surface, m_buffer, 0, 0);
    wl_surface_set_buffer_scale(shield.surface, 1);
    wl_surface_damage_buffer(shield.surface, 0, 0, 1, 1);
    shield.bufferAttached = true;
  }
  if (shield.viewport != nullptr && width > 0 && height > 0) {
    wp_viewport_set_destination(shield.viewport, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height));
  }

  shield.configured = true;
  applyInputRegion(shield);
  wl_surface_commit(shield.surface);
}

void PanelClickShield::applyInputRegion(Shield& shield) {
  if (m_wayland == nullptr || shield.surface == nullptr || shield.width <= 0 || shield.height <= 0) {
    return;
  }
  wl_region* region = wl_compositor_create_region(m_wayland->compositor());
  if (region == nullptr) {
    return;
  }
  wl_region_add(region, 0, 0, shield.width, shield.height);
  for (const auto& rect : shield.excludeRects) {
    if (rect.width > 0 && rect.height > 0) {
      wl_region_subtract(region, rect.x, rect.y, rect.width, rect.height);
    }
  }
  wl_surface_set_input_region(shield.surface, region);
  wl_region_destroy(region);
}
