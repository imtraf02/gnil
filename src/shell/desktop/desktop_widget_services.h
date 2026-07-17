#pragma once

class ConfigService;
class HttpClient;
class MprisService;
class PipeWireSpectrum;
class RenderContext;
class SharedTextureCache;
class SystemMonitorService;
class WaylandConnection;
class WeatherService;

struct DesktopWidgetRuntimeServices {
  PipeWireSpectrum* pipewireSpectrum = nullptr;
  const WeatherService* weather = nullptr;
  MprisService* mpris = nullptr;
  HttpClient* httpClient = nullptr;
  SystemMonitorService* sysmon = nullptr;
  ConfigService* config = nullptr;
};

struct DesktopWidgetServices {
  WaylandConnection& wayland;
  ConfigService* config = nullptr;
  RenderContext* renderContext = nullptr;
  DesktopWidgetRuntimeServices runtime;
  SharedTextureCache* textureCache = nullptr;
};
