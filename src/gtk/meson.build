northstar_gtk_sources = files(
  '../appmain.c',
  'icons.c',
  'procview.c',
  'procwindow.c',
  'uiscale.c',
  '../libnorthstar.c',
  '../page_session.c',
  '../threaddump.c',
  'enginethread.c',
)

northstar_gtk_deps = northstar_deps + [gtk4]

northstar_gtk_platform_sources = []
northstar_gtk_platform_link_args = []

gnome = import('gnome')
northstar_icon_resources = gnome.compile_resources(
  'northstar-icon-resources',
  meson.project_source_root() / 'data' / 'icons' / 'northstar-icons.gresource.xml',
  source_dir: meson.project_source_root() / 'data' / 'icons',
  c_name: 'northstar_icons',
)

executable(
  'northstar',
  northstar_engine_sources + northstar_app_sources +
    northstar_gtk_sources + northstar_audio_sources +
    northstar_gtk_platform_sources +
    northstar_icon_resources +
    northstar_win_resources + [polyfills_h, wpt_hook_h, sb_key_h],
  dependencies: northstar_gtk_deps + northstar_audio_deps,
  link_with: northstar_audio_libs,
  include_directories: include_directories('..', '.'),
  pie: host_machine.system() != 'windows',
  win_subsystem: northstar_win_subsystem,
  link_args: northstar_link_args + northstar_gtk_platform_link_args,
  install: true,
)

if host_machine.system() != 'windows'
  install_symlink(
    'open-internet-browser',
    pointing_to: 'northstar',
    install_dir: get_option('bindir'),
  )
endif
