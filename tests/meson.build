notification_history_utf8_test = executable('notification_history_utf8_test',
  sources: files(
    'notification_history_utf8_test.cpp',
    '../src/core/log.cpp',
    '../src/notification/notification_history_store.cpp',
    '../src/render/core/image_decoder.cpp',
  ),
  dependencies: [
    libwebp_dep,
    nlohmann_dep,
  ],
  include_directories: _gnil_inc,
)

test('notification_history_utf8', notification_history_utf8_test)

notification_filter_test = executable('notification_filter_test',
  sources: files(
    'notification_filter_test.cpp',
    '../src/notification/notification_filter.cpp',
  ),
  include_directories: _gnil_inc,
)

test('notification_filter', notification_filter_test)

workspace_alert_service_test = executable('workspace_alert_service_test',
  sources: files(
    'workspace_alert_service_test.cpp',
    '../src/compositors/workspace_alert_service.cpp',
  ),
  include_directories: _gnil_inc,
)

test('workspace_alert_service', workspace_alert_service_test)

notification_card_model_test = executable('notification_card_model_test',
  sources: files(
    'notification_card_model_test.cpp',
    '../src/shell/notification/notification_card_model.cpp',
  ),
  include_directories: _gnil_inc,
)

test('notification_card_model', notification_card_model_test)

workspace_glyph_test = executable('workspace_glyph_test',
  sources: files(
    'workspace_glyph_test.cpp',
    '../src/shell/bar/widgets/workspace_glyph.cpp',
  ),
  include_directories: _gnil_inc,
  cpp_args: ['-DGNIL_SOURCE_ROOT="' + meson.project_source_root() + '"'],
)

test('workspace_glyph', workspace_glyph_test)

animation_manager_test = executable('animation_manager_test',
  sources: files(
    'animation_manager_test.cpp',
    '../src/render/animation/animation.cpp',
    '../src/render/animation/animation_manager.cpp',
    '../src/render/animation/motion_service.cpp',
  ),
  include_directories: _gnil_inc,
)

test('animation_manager', animation_manager_test)

chrome_geometry_test = executable('chrome_geometry_test',
  sources: files(
    'chrome_geometry_test.cpp',
    '../src/shell/chrome/chrome_geometry.cpp',
  ),
  include_directories: _gnil_inc,
)

test('chrome_geometry', chrome_geometry_test)

lockscreen_layout_test = executable('lockscreen_layout_test',
  sources: files(
    'lockscreen_layout_test.cpp',
    '../src/shell/lockscreen/lockscreen_layout.cpp',
  ),
  include_directories: _gnil_inc,
)

test('lockscreen_layout', lockscreen_layout_test)

cyclic_picker_test = executable('cyclic_picker_test',
  sources: files('cyclic_picker_test.cpp'),
  include_directories: _gnil_inc,
)
test('cyclic_picker', cyclic_picker_test)

panel_keyboard_focus_test = executable('panel_keyboard_focus_test',
  sources: files(
    'panel_keyboard_focus_test.cpp',
  ),
  include_directories: _gnil_inc,
)

test('panel_keyboard_focus', panel_keyboard_focus_test)

panel_click_shield_policy_test = executable('panel_click_shield_policy_test',
  sources: files(
    'panel_click_shield_policy_test.cpp',
  ),
  include_directories: _gnil_inc,
)

test('panel_click_shield_policy', panel_click_shield_policy_test)

launcher_visual_size_test = executable('launcher_visual_size_test',
  sources: files(
    'launcher_visual_size_test.cpp',
  ),
  include_directories: _gnil_inc,
)

test('launcher_visual_size', launcher_visual_size_test)

glyph_registry_test = executable('glyph_registry_test',
  sources: files(
    'glyph_registry_test.cpp',
    '../src/render/text/glyph_registry.cpp',
    '../src/core/files/resource_paths.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
  cpp_args: [
    '-DGNIL_SOURCE_ASSETS_DIR="' + meson.project_source_root() / 'assets' + '"',
    '-DGNIL_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DGNIL_INSTALL_DATADIR="' + get_option('datadir') + '"',
  ],
)

test('glyph_registry', glyph_registry_test)

input_area_test = executable('input_area_test',
  sources: files(
    'input_area_test.cpp',
    '../src/render/scene/input_area.cpp',
    '../src/render/scene/node.cpp',
    '../src/render/animation/animation.cpp',
    '../src/render/animation/animation_manager.cpp',
    '../src/render/animation/motion_service.cpp',
    '../src/core/ui_phase.cpp',
    '../src/core/log.cpp',
  ) + _protocol_headers,
  dependencies: [
    wayland_client_dep,
  ],
  include_directories: _gnil_inc,
)

test('input_area', input_area_test)

log_test = executable('log_test',
  sources: files(
    'log_test.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
)

test('log', log_test)

ipc_service_test = executable('ipc_service_test',
  sources: files(
    'ipc_service_test.cpp',
    '../src/ipc/ipc_service.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
)

test('ipc_service', ipc_service_test)

process_test = executable('process_test',
  sources: files(
    'process_test.cpp',
    '../src/core/process/process.cpp',
    '../src/core/log.cpp'
  ),
  include_directories: _gnil_inc,
)

test('process', process_test)

battery_hook_state_test = executable('battery_hook_state_test',
  sources: files(
    'battery_hook_state_test.cpp',
    '../src/hooks/battery_hook_state.cpp',
  ),
  include_directories: _gnil_inc,
)

test('battery_hook_state', battery_hook_state_test)

audio_glyphs_test = executable('audio_glyphs_test',
  sources: files(
    'audio_glyphs_test.cpp',
    '../src/pipewire/audio_glyphs.cpp',
  ),
  include_directories: _gnil_inc,
)

test('audio_glyphs', audio_glyphs_test)

hook_manager_test = executable('hook_manager_test',
  sources: files(
    'hook_manager_test.cpp',
    '../src/hooks/hook_manager.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
)

test('hook_manager', hook_manager_test)

desktop_entry_launch_test = executable('desktop_entry_launch_test',
  sources: files(
    'desktop_entry_launch_test.cpp',
    '../src/system/desktop_entry_launch.cpp',
    '../src/system/terminal_launch.cpp',
    '../src/core/process/process.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
)

test('desktop_entry_launch', desktop_entry_launch_test)

string_utils_test = executable('string_utils_test',
  sources: files(
    'string_utils_test.cpp',
  ),
  include_directories: _gnil_inc,
)

test('string_utils', string_utils_test)

ical_recurrence_test = executable('ical_recurrence_test',
  sources: files(
    'ical_recurrence_test.cpp',
    '../src/calendar/ical_parser.cpp',
  ),
  include_directories: _gnil_inc,
)

test('ical_recurrence', ical_recurrence_test)

time_format_test = executable('time_format_test',
  sources: files(
    'time_format_test.cpp',
    '../src/time/time_format.cpp',
    '../src/i18n/i18n.cpp',
    '../src/i18n/i18n_service.cpp',
    '../src/core/files/resource_paths.cpp',
    '../src/core/log.cpp',
  ),
  dependencies: [
    nlohmann_dep,
  ],
  include_directories: _gnil_inc,
  cpp_args: [
    '-DGNIL_SOURCE_ASSETS_DIR="' + meson.project_source_root() / 'assets' + '"',
    '-DGNIL_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DGNIL_INSTALL_DATADIR="' + get_option('datadir') + '"',
  ],
)

test('time_format', time_format_test)

day_night_schedule_test = executable('day_night_schedule_test',
  sources: files(
    'day_night_schedule_test.cpp',
    '../src/system/day_night_schedule.cpp',
  ),
  include_directories: _gnil_inc,
)

test('day_night_schedule', day_night_schedule_test)

cpu_temp_sensor_test = executable('cpu_temp_sensor_test',
  sources: files(
    'cpu_temp_sensor_test.cpp',
    '../src/system/cpu_temp_sensor.cpp',
  ),
  include_directories: _gnil_inc,
)

test('cpu_temp_sensor', cpu_temp_sensor_test)

scheme_test = executable('scheme_test',
  sources: files(
    'scheme_test.cpp',
    '../src/theme/scheme.cpp',
    '../src/theme/color.cpp',
    '../src/theme/contrast.cpp',
    '../src/theme/custom_schemes.cpp',
    '../src/theme/m3_schemes.cpp',
    '../src/theme/fixed_palette.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    mcu_dep,
  ],
)

test('scheme', scheme_test)

launcher_command_router_test = executable('launcher_command_router_test',
  sources: files(
    'launcher_command_router_test.cpp',
    '../src/launcher/command_router.cpp',
  ),
  include_directories: _gnil_inc,
)

test('launcher_command_router', launcher_command_router_test)

nexus_route_test = executable('nexus_route_test',
  sources: files(
    'nexus_route_test.cpp',
    '../src/shell/settings/nexus_route.cpp',
  ),
  include_directories: _gnil_inc,
)

test('nexus_route', nexus_route_test)

config_schema_roundtrip_test = executable('config_schema_roundtrip_test',
  sources: files(
    'config_schema_roundtrip_test.cpp',
    '../src/config/schema/config_schema.cpp',
    '../src/config/schema/config_sections.cpp',
    '../src/config/config_export.cpp',
    '../src/config/config_types.cpp',
    '../src/notification/notification_filter.cpp',
    '../src/render/core/color.cpp',
    '../src/ui/palette.cpp',
    '../src/theme/builtin_palettes.cpp',
    '../src/theme/fixed_palette.cpp',
    '../src/theme/color.cpp',
    '../src/theme/contrast.cpp',
    '../src/core/input/key_chord.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    xkbcommon_dep,
    tomlplusplus_dep,
  ],
)

test('config_schema_roundtrip', config_schema_roundtrip_test)

config_migration_test = executable('config_migration_test',
  sources: files(
    'config_migration_test.cpp',
    '../src/config/config_migrations.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    tomlplusplus_dep,
  ],
)

test('config_migration', config_migration_test)

settings_document_test = executable('settings_document_test',
  sources: files(
    'settings_document_test.cpp',
    '../src/config/settings_document.cpp',
    '../src/config/config_migrations.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    tomlplusplus_dep,
  ],
)

test('settings_document', settings_document_test)

config_widget_test = executable('config_widget_test',
  sources: files(
    'config_widget_test.cpp',
    '../src/config/widget_config.cpp',
    '../src/config/config_types.cpp',
    '../src/render/core/color.cpp',
    '../src/ui/palette.cpp',
    '../src/theme/builtin_palettes.cpp',
    '../src/theme/fixed_palette.cpp',
    '../src/theme/color.cpp',
    '../src/theme/contrast.cpp',
    '../src/core/input/key_chord.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    xkbcommon_dep,
    tomlplusplus_dep,
  ],
)

test('config_widget', config_widget_test)

config_path_resolution_test = executable('config_path_resolution_test',
  sources: files(
    'config_path_resolution_test.cpp',
    '../src/config/schema/config_schema.cpp',
    '../src/config/schema/config_sections.cpp',
    '../src/config/config_types.cpp',
    '../src/notification/notification_filter.cpp',
    '../src/render/core/color.cpp',
    '../src/ui/palette.cpp',
    '../src/theme/builtin_palettes.cpp',
    '../src/theme/fixed_palette.cpp',
    '../src/theme/color.cpp',
    '../src/theme/contrast.cpp',
    '../src/core/input/key_chord.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    xkbcommon_dep,
    tomlplusplus_dep,
  ],
)

test('config_path_resolution', config_path_resolution_test)

config_validate_env = environment()

sh_program = find_program('sh')
test('config_validate_cli',
  sh_program,
  args: ['config_validate_cli_test.sh', gnil_exe.full_path()],
  workdir: meson.current_source_dir(),
  env: config_validate_env,
)

test('branding_guard',
  sh_program,
  args: ['branding_guard_test.sh'],
  workdir: meson.current_source_dir(),
)

state_store_test = executable('state_store_test',
  sources: files(
    'state_store_test.cpp',
    '../src/config/atomic_file.cpp',
    '../src/config/state_store.cpp',
    '../src/core/log.cpp',
  ),
  include_directories: _gnil_inc,
  dependencies: [
    tomlplusplus_dep,
  ],
)

test('state_store', state_store_test)

image_source_log_test = executable('image_source_log_test',
  sources: files(
    'image_source_log_test.cpp',
  ),
  include_directories: _gnil_inc,
)

test('image_source_log', image_source_log_test)

image_file_loader_data_uri_test = executable('image_file_loader_data_uri_test',
  sources: files(
    'image_file_loader_data_uri_test.cpp',
    '../src/render/core/image_file_loader.cpp',
    '../src/render/core/image_decoder.cpp',
    '../src/render/core/stb_image_resize_impl.cpp',
  ),
  dependencies: [
    cairo_dep,
    librsvg_dep,
    libwebp_dep,
  ],
  include_directories: _gnil_inc,
)

test('image_file_loader_data_uri', image_file_loader_data_uri_test)

ico_decoder_test = executable('ico_decoder_test',
  sources: files(
    'ico_decoder_test.cpp',
    '../src/render/core/image_decoder.cpp',
  ),
  dependencies: [
    libwebp_dep,
  ],
  include_directories: _gnil_inc,
)

test('ico_decoder', ico_decoder_test)

app_identity_test = executable('app_identity_test',
  sources: files(
    'app_identity_test.cpp',
    '../src/system/app_identity.cpp',
  ),
  include_directories: _gnil_inc,
)

test('app_identity', app_identity_test)

wayland_toplevels_identity_test = executable('wayland_toplevels_identity_test',
  sources: files(
    'wayland_toplevels_identity_test.cpp',
    '../src/system/app_identity.cpp',
    '../src/wayland/wayland_toplevels.cpp',
  ) + _protocol_sources + _protocol_headers,
  dependencies: [
    wayland_client_dep,
  ],
  include_directories: _gnil_inc,
)

test('wayland_toplevels_identity', wayland_toplevels_identity_test)
