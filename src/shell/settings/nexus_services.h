#pragma once

#include <functional>

class ConfigService;
class INetworkService;
class BluetoothService;
class BluetoothAgent;
class PipeWireService;
class NotificationManager;
class CompositorPlatform;
class DependencyService;
class Wallpaper;
class ClipboardService;

struct NexusServices {
  ConfigService* config = nullptr;
  INetworkService* network = nullptr;
  BluetoothService* bluetooth = nullptr;
  BluetoothAgent* bluetoothAgent = nullptr;
  PipeWireService* audio = nullptr;
  NotificationManager* notifications = nullptr;
  CompositorPlatform* platform = nullptr;
  DependencyService* dependencies = nullptr;
  Wallpaper* wallpaper = nullptr;
  ClipboardService* clipboard = nullptr;
  std::function<void()> openWallpaperPanel;
  std::function<void()> resetLauncherUsage;
};
