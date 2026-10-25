project(
  'northstar',
  ['c', 'cpp'],
  version: '1.0.17',
  license: 'GPL-3.0-or-later',
  default_options: [
    'c_std=gnu2x,c2x,gnu11,c11',
    'cpp_std=c++17',
    'warning_level=2',
    'quickjs-ng:warning_level=0',
    'quickjs-ng:default_library=static',
    'buildtype=release',
    'optimization=3',
    'b_lto=true',
    'b_ndebug=true',
    'b_pie=true',
  ],
  meson_version: '>=1.0',
)

cc = meson.get_compiler('c')

common_warning_flags = [
  '-Wshadow',
  '-Wpointer-arith',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wmissing-declarations',
  '-Wold-style-definition',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wformat-security',
  '-Wnull-dereference',
  '-Wlogical-op',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wredundant-decls',
]

suppressed_warnings = [
  '-Wno-constant-logical-operand',
]

fortify_level = '-D_FORTIFY_SOURCE=2'
if cc.has_argument('-D_FORTIFY_SOURCE=3')
  fortify_level = '-D_FORTIFY_SOURCE=3'
endif

hardening_c_flags = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-U_FORTIFY_SOURCE',
  fortify_level,
]

hardening_link_flags = [
  '-Wl,-z,relro',
  '-Wl,-z,now',
  '-Wl,-z,noexecstack',
  '-Wl,-z,separate-code',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags + suppressed_warnings +
                             hardening_c_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'c',
)

ns_buildtype = get_option('buildtype')
ns_is_release = (ns_buildtype == 'release' or ns_buildtype == 'minsize')
# Every build profile gets -O3 (via default_options optimization=3) plus
# dead-section pruning. Nordstjernen Web Browser leans on the optimizer to inline the
# lexbor parser walks, QuickJS interpreter dispatch, and our box-layout
# loops; -O2 left ~30% on the table for JS-heavy pages. The -O level is
# left to meson's optimization option so it isn't passed twice (which
# warns); we only add the section-GC flags here. Dev/non-release builds
# keep -g3 + frame pointers so step debugging still works under -O3.
add_project_arguments(
  cc.get_supported_arguments(['-ffunction-sections', '-fdata-sections']),
  language: 'c',
)
add_project_link_arguments(
  cc.get_supported_link_arguments(['-Wl,--gc-sections']),
  language: 'c',
)
if ns_is_release
  add_project_link_arguments(
    cc.get_supported_link_arguments(['-Wl,-s']),
    language: 'c',
  )
else
  add_project_arguments(
    cc.get_supported_arguments(['-g3', '-fno-omit-frame-pointer']),
    language: 'c',
  )
endif
# binutils ar on Windows has no LLVM plugin, so it archives clang's LTO
# bitcode without indexing a single symbol and every static library links as
# if it were empty. Meson picks llvm-ar for clang when it is installed.
if get_option('b_lto') and cc.get_id() == 'clang' and host_machine.system() == 'windows'
  llvm_ar = find_program('llvm-ar-' + cc.version().split('.')[0], 'llvm-ar',
                         required: false)
  if not llvm_ar.found()
    error('LTO with clang needs llvm-ar; install it (MSYS2: pacman -S ' +
          'mingw-w64-ucrt-x86_64-llvm-tools) or configure with -Db_lto=false')
  endif
endif

ns_release_profile = get_option('b_lto') ? 'release (LTO + stripped + hardened)' : 'release (stripped + hardened)'
message('northstar build profile: ' +
        (ns_is_release ? ns_release_profile
                       : 'development (debug symbols + frame pointer + hardened)'))

# web-text-lib enforces the text-stack floors from inside its own build, where
# a too-old library surfaces as a bare version mismatch and only the first of
# several is reported. Checking every floor here first names them all at once.
gtk4_min = host_machine.system() == 'windows' ? '>=4.22.1' : '>=4.14'
ns_dependency_floors = [
  ['glib-2.0', '>=2.80'],
  ['gobject-2.0', '>=2.80'],
  ['gio-2.0', '>=2.80'],
  ['cairo', '>=1.18'],
  ['harfbuzz', '>=8.3'],
  ['fribidi', '>=1.0.6'],
  ['fontconfig', '>=2.15'],
  ['gtk4', gtk4_min],
  ['libcurl', '>=8.5'],
]
ns_unmet_floors = []
foreach floor : ns_dependency_floors
  if not dependency(floor[0], version: floor[1], required: false).found()
    installed = dependency(floor[0], required: false)
    ns_unmet_floors += '  @0@ @1@ (@2@)'.format(floor[0], floor[1],
      installed.found() ? 'found ' + installed.version() : 'not found')
  endif
endforeach
if ns_unmet_floors.length() > 0
  error('Nordstjernen Web Browser needs newer system libraries than this machine has:\n' +
        '\n'.join(ns_unmet_floors) + '\n' +
        'Ubuntu 24.04, Debian 13 and Fedora 40 are the oldest releases of ' +
        'their distributions that ship all of them; see docs/building.md.')
endif

glib       = dependency('glib-2.0')
gobject    = dependency('gobject-2.0')
gio        = dependency('gio-2.0')
cairo      = dependency('cairo')

gtk4 = dependency('gtk4', version: gtk4_min)
if host_machine.system() == 'windows'
  message('GTK @0@ — Windows requires the MSYS2 stock GTK (>= 4.22.1).'.format(gtk4.version()))
elif gtk4.version().version_compare('>=4.22')
  message('GTK @0@ — preferred (>= 4.22).'.format(gtk4.version()))
else
  warning('GTK @0@ meets the 4.14 minimum, but Nordstjernen Web Browser prefers >= 4.22 for the newer GSK renderers.'.format(gtk4.version()))
endif
gmodule = dependency('gmodule-2.0')
libcurl = dependency('libcurl', version: '>=8.5')
if libcurl.version().version_compare('<8.11')
  warning('libcurl @0@ predates stable WebSocket support (curl 8.11): WebSocket will be unavailable at runtime unless this libcurl was built with the ws/wss protocols.'.format(libcurl.version()))
endif
libcrypto = dependency('libcrypto')
libm = cc.find_library('m', required: false)

libuchardet = dependency('uchardet')

libpsl = dependency('libpsl')

sqlite3 = dependency('sqlite3')

libfontconfig = dependency('fontconfig', required: false)
have_fontconfig = libfontconfig.found()
if have_fontconfig
  add_project_arguments('-DNS_HAVE_FONTCONFIG=1', language: 'c')
endif

libfreetype = dependency('freetype2', required: false)
have_freetype = libfreetype.found()
if have_freetype
  add_project_arguments('-DNS_HAVE_FREETYPE=1', language: 'c')
endif


libdav1d = dependency('dav1d', version: '>= 1.0.0', required: get_option('av1'))
have_av1 = libdav1d.found()
if have_av1
  add_project_arguments('-DNS_HAVE_AV1', language: 'c')
endif
libvpx = dependency('vpx', version: '>= 1.8.0', required: get_option('vp9'))
have_vp9 = libvpx.found()
if have_vp9
  add_project_arguments('-DNS_HAVE_VP9', language: 'c')
endif
libopenh264 = dependency('openh264', version: '>= 2.0.0', required: get_option('h264'))
have_h264 = libopenh264.found()
if have_h264
  add_project_arguments('-DNS_HAVE_H264', language: 'c')
endif
libopus = dependency('opus', required: false)
if libopus.found()
  add_project_arguments('-DNS_HAVE_OPUS', language: 'c')
endif
libavif = dependency('libavif', required: get_option('avif'))
have_avif = libavif.found()
if have_avif
  add_project_arguments('-DNS_HAVE_AVIF=1', language: 'c')
endif

# vorbisfile is the ov_* high-level decode API over libvorbis, used by the
# in-process audio mixer to decode Ogg/Vorbis natively.
libvorbisfile = dependency('vorbisfile', required: false)
if libvorbisfile.found()
  add_project_arguments('-DNS_HAVE_VORBISFILE=1', language: 'c')
endif
# opusfile is the high-level op_* decode API over libopus + libogg, used by the
# in-process audio mixer to decode Ogg/Opus natively.
libopusfile = dependency('opusfile', required: false)
if libopusfile.found()
  add_project_arguments('-DNS_HAVE_OPUSFILE=1', language: 'c')
endif
# src/media_types.c may advertise an audio type only where the decoder that
# plays it is built: the SDL2 in-process mixer.
if get_option('audio').allowed()
  sdl2_probe = dependency('sdl2', required: false)
  if sdl2_probe.found()
    add_project_arguments('-DNS_AUDIO_PLAYBACK=1', language: 'c')
  endif
  if sdl2_probe.found() and libvorbisfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_VORBIS=1', language: 'c')
  endif
  if sdl2_probe.found() and libopusfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_OPUS=1', language: 'c')
  endif
endif
libenchant = dependency('enchant-2', required: false)
have_enchant = libenchant.found()
if have_enchant
  add_project_arguments('-DNS_HAVE_ENCHANT=1', language: 'c')
endif

libseccomp = dependency(
  'libseccomp',
  required: host_machine.system() == 'linux',
)
have_seccomp = libseccomp.found()
if have_seccomp
  add_project_arguments('-DNS_HAVE_SECCOMP=1', language: 'c')
endif

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])
libplmpeg = dependency('plmpeg', fallback: ['plmpeg', 'plmpeg_dep'])
zlib = dependency('zlib')

liblexbor = dependency('lexbor', version: '>=3.0.0', required: false)
if liblexbor.found()
  lexbor_version = liblexbor.version()
else
  lexbor_version = '3.0.1'
endif
add_project_arguments('-DNS_LEXBOR_VERSION="' + lexbor_version + '"',
                      language: 'c')

build_date = get_option('build_date')
if build_date == ''
  date_cmd = run_command('date', '-u', '+%Y-%m-%d', check: false)
  if date_cmd.returncode() == 0
    build_date = date_cmd.stdout().strip()
  endif
endif
add_project_arguments('-DNS_BUILD_DATE="' + build_date + '"', language: 'c')
add_project_arguments('-DNS_VERSION="' + meson.project_version() + '"', language: 'c')

if not liblexbor.found()
  cmake = import('cmake')
  lexbor_opts = cmake.subproject_options()
  lexbor_opts.add_cmake_defines({
    'LEXBOR_BUILD_SHARED': false,
    'LEXBOR_BUILD_STATIC': true,
    'LEXBOR_BUILD_TESTS': false,
    'LEXBOR_BUILD_EXAMPLES': false,
    'LEXBOR_BUILD_UTILS': false,
    'LEXBOR_BUILD_SEPARATELY': false,
    'CMAKE_POSITION_INDEPENDENT_CODE': true,
  })
  lexbor_opts.append_compile_args('c', '-w')
  lexbor_subproject = cmake.subproject('lexbor', options: lexbor_opts)
  liblexbor = lexbor_subproject.dependency('lexbor_static').as_system('system')
endif

js_engine = get_option('js_engine')
if js_engine == 'quickjs'
  libquickjs = subproject('quickjs').get_variable('quickjs_dep')
  add_project_arguments('-DNS_QUICKJS_ORIGINAL=1',
                        '-DNS_QUICKJS_VERSION="' + libquickjs.version() + '"',
                        language: 'c')
else
  libquickjs = dependency('quickjs-ng', fallback: ['quickjs-ng', 'qjs_dep'])
endif

wasm_unsupported_cpu = not (host_machine.cpu_family() == 'x86_64' or
  (host_machine.cpu_family() == 'aarch64' and host_machine.system() != 'windows'))
if get_option('wasm').enabled() and wasm_unsupported_cpu
  error('wasm (WAMR) is built only for x86-64 and 64-bit Arm (not Arm Windows); build with -Dwasm=disabled')
endif
wasm_feature = get_option('wasm').disable_auto_if(wasm_unsupported_cpu)
have_wasm = false
if wasm_feature.allowed()
  subdir('src/wamr')
  have_wasm = true
else
  libwamr = declare_dependency()
endif

install_data(
  'data/icons/hicolor/scalable/apps/northstar.svg',
  'data/icons/hicolor/scalable/apps/northstar.gif',
  'data/icons/hicolor/scalable/apps/northstar-back.svg',
  'data/icons/hicolor/scalable/apps/northstar-forward.svg',
  'data/icons/hicolor/scalable/apps/northstar-home.svg',
  'data/icons/hicolor/scalable/apps/northstar-reload.svg',
  'data/icons/hicolor/scalable/apps/northstar-stop.svg',
  'data/icons/hicolor/scalable/apps/northstar-print.svg',
  'data/icons/hicolor/scalable/apps/northstar-downloads.svg',
  'data/icons/hicolor/scalable/apps/northstar-bookmarks.svg',
  'data/icons/hicolor/scalable/apps/northstar-bookmarks-saved.svg',
  'data/icons/hicolor/scalable/apps/northstar-console.svg',
  'data/icons/hicolor/scalable/apps/northstar-settings.svg',
  'data/icons/hicolor/scalable/apps/northstar-about.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-connect.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-fetch.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-parse.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-script.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-render.svg',
  'data/icons/hicolor/scalable/apps/northstar-stage-done.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_data(
  'LICENSE',
  'THIRD-PARTY-LICENSES.md',
  install_dir: get_option('datadir') / 'northstar',
)

install_data(
  'data/safebrowsing.list',
  install_dir: get_option('datadir') / 'northstar',
)

install_data(
  'data/i18n/am.lang',
  'data/i18n/ar.lang',
  'data/i18n/bn.lang',
  'data/i18n/de.lang',
  'data/i18n/el.lang',
  'data/i18n/en.lang',
  'data/i18n/es.lang',
  'data/i18n/fa.lang',
  'data/i18n/fr.lang',
  'data/i18n/gu.lang',
  'data/i18n/ha.lang',
  'data/i18n/hi.lang',
  'data/i18n/hu.lang',
  'data/i18n/id.lang',
  'data/i18n/it.lang',
  'data/i18n/ja.lang',
  'data/i18n/jv.lang',
  'data/i18n/kn.lang',
  'data/i18n/ko.lang',
  'data/i18n/ml.lang',
  'data/i18n/mr.lang',
  'data/i18n/my.lang',
  'data/i18n/nl.lang',
  'data/i18n/pa.lang',
  'data/i18n/pl.lang',
  'data/i18n/pt.lang',
  'data/i18n/ro.lang',
  'data/i18n/ru.lang',
  'data/i18n/su.lang',
  'data/i18n/sw.lang',
  'data/i18n/ta.lang',
  'data/i18n/te.lang',
  'data/i18n/th.lang',
  'data/i18n/tl.lang',
  'data/i18n/tr.lang',
  'data/i18n/uk.lang',
  'data/i18n/ur.lang',
  'data/i18n/vi.lang',
  'data/i18n/yo.lang',
  'data/i18n/zh.lang',
  install_dir: get_option('datadir') / 'northstar' / 'i18n',
)

install_data(
  'data/northstar.desktop',
  install_dir: get_option('datadir') / 'applications',
  rename: 'org.northstar.WebBrowser.desktop',
)

install_data(
  'data/org.northstar.WebBrowser.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

summary({'WebAssembly (WAMR)': have_wasm}, section: 'Features')
summary({'AV1 video (dav1d)': have_av1}, section: 'Features')
summary({'VP9 video (libvpx)': have_vp9}, section: 'Features')
summary({'H.264 video (OpenH264)': have_h264}, section: 'Features')
summary({'Opus in WebM (libopus)': libopus.found()}, section: 'Features')
summary({'AVIF images (libavif)': have_avif}, section: 'Features')
summary({'JavaScript engine': js_engine + ' ' + libquickjs.version()}, section: 'Features')

# Text is shaped through web-text-lib (src/web-text-lib/), the in-tree Pango
# fork that caches finished glyph strings across layouts; every symbol in it
# is renamed, because GTK loads the system Pango into the same process. It
# declares targets, so it comes after the last project argument.
subdir('src/web-text-lib')
pango       = web_text_dep
pangocairo  = web_text_cairo_dep
libpangoft2 = web_text_fc_dep

subdir('src')
