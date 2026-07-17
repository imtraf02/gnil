#pragma once

#include "shell/control_center/shortcut_services.h"

class AccountsService;
class BluetoothAgent;
class BluetoothService;
class BrightnessService;
class CalendarService;
class ClipboardService;
class CompositorPlatform;
class ConfigService;
class DependencyService;
class EasyEffectsService;
class FileWatcher;
class HttpClient;
class IdleInhibitor;
class INetworkService;
class IpcService;
class MprisService;
class NetworkSecretAgent;
class NotificationManager;
class PipeWireService;
class PipeWireSpectrum;
class PowerProfilesService;
class ScreenTimeService;
class SystemMonitorService;
class ThumbnailService;
class UPowerService;
class Wallpaper;
class WeatherService;
class GammaService;

namespace gnil::theme {
  class ThemeService;
}

struct ControlCenterServices {
  NotificationManager* notifications = nullptr;
  PipeWireService* audio = nullptr;
  EasyEffectsService* easyEffects = nullptr;
  MprisService* mpris = nullptr;
  ConfigService* config = nullptr;
  HttpClient* httpClient = nullptr;
  WeatherService* weather = nullptr;
  PipeWireSpectrum* spectrum = nullptr;
  UPowerService* upower = nullptr;
  PowerProfilesService* powerProfiles = nullptr;
  INetworkService* network = nullptr;
  NetworkSecretAgent* networkSecrets = nullptr;
  BluetoothService* bluetooth = nullptr;
  BluetoothAgent* bluetoothAgent = nullptr;
  BrightnessService* brightness = nullptr;
  SystemMonitorService* sysmon = nullptr;
  ScreenTimeService* screenTime = nullptr;
  GammaService* nightLight = nullptr;
  gnil::theme::ThemeService* theme = nullptr;
  IdleInhibitor* idleInhibitor = nullptr;
  DependencyService* dependencies = nullptr;
  CompositorPlatform* platform = nullptr;
  IpcService* ipc = nullptr;
  Wallpaper* wallpaper = nullptr;
  CalendarService* calendar = nullptr;
  FileWatcher* fileWatcher = nullptr;
  ClipboardService* clipboard = nullptr;
  AccountsService* accounts = nullptr;
  ThumbnailService* thumbnails = nullptr;

  [[nodiscard]] ShortcutServices shortcutServices() const {
    return {
        .network = network,
        .bluetooth = bluetooth,
        .nightLight = nightLight,
        .theme = theme,
        .notifications = notifications,
        .idleInhibitor = idleInhibitor,
        .audio = audio,
        .powerProfiles = powerProfiles,
        .mpris = mpris,
        .weather = weather,
        .config = config,
        .dependencies = dependencies,
        .platform = platform,
        .ipc = ipc,
        .fileWatcher = fileWatcher,
        .httpClient = httpClient,
        .clipboard = clipboard,
    };
  }
};
