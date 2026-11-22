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

# muon compiles a pie: true executable with -fPIE but, unlike meson, does
# not link it with -pie, so with a compiler that does not default to PIE
# the browser would link position-dependent. Naming -pie here keeps muon
# builds PIE; meson then passes it twice, which changes nothing.
northstar_pie = host_system != 'windows'
if northstar_pie and host_system != 'darwin'
  northstar_gtk_platform_link_args += cc.get_supported_link_arguments(['-pie'])
endif

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
    northstar_win_resources + ns_embedded_headers + [sb_key_h] +
    pdf_viewer_headers,
  dependencies: northstar_gtk_deps + northstar_audio_deps,
  link_with: northstar_audio_libs,
  include_directories: [include_directories('..', '.'), ns_embedded_inc],
  pie: northstar_pie,
  win_subsystem: northstar_win_subsystem,
  link_args: northstar_link_args + northstar_gtk_platform_link_args,
  install: true,
)

if host_system != 'windows'
  install_symlink(
    'open-internet-browser',
    pointing_to: 'northstar',
    install_dir: get_option('bindir'),
  )
endif
