project(
  'northstar',
  'c',
  version: '1.0.27-dev',
  license: 'GPL-3.0-or-later',
  license_files: ['LICENSE', 'THIRD-PARTY-LICENSES.md'],
  default_options: {
    'c_std': 'gnu2x,c2x,gnu11,c11',
    'warning_level': '2',
    'quickjs-ng:warning_level': '0',
    'quickjs-ng:default_library': 'static',
    'buildtype': 'release',
    'optimization': '3',
    'b_lto': true,
    'b_ndebug': 'true',
    'b_pie': true,
  },
  meson_version: '>=1.3',
)

cc = meson.get_compiler('c')
fs = import('fs')
host_system = host_machine.system()

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

# A C library without level 3 (glibc before 2.34) treats it as level 2.
hardening_c_flags = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-U_FORTIFY_SOURCE',
  '-D_FORTIFY_SOURCE=3',
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
# dead-section pruning. Northstar Web Browser leans on the optimizer to inline the
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
if get_option('b_lto') and cc.get_id() == 'clang' and host_system == 'windows'
  llvm_ar = find_program('llvm-ar-' + cc.version().split('.')[0], 'llvm-ar',
                         required: false)
  if not llvm_ar.found()
    error('LTO with clang needs llvm-ar; install it (MSYS2: pacman -S ' +
          'mingw-w64-ucrt-x86_64-llvm-tools) or configure with -Db_lto=false')
  endif
endif

# muon hands GCC a bare -flto, under which GCC runs the link-time
# partitions of the browser one after another ("using serial compilation of
# N LTRANS jobs"); meson compiles with -flto=auto, which runs them in
# parallel. Naming it on the link line gives muon builds the parallel link
# too.
if get_option('b_lto') and cc.get_id() == 'gcc'
  add_project_link_arguments(
    cc.get_supported_link_arguments(['-flto=auto']),
    language: 'c',
  )
endif

ns_release_profile = get_option('b_lto') ? 'release (LTO + stripped + hardened)' : 'release (stripped + hardened)'
message('northstar build profile: ' +
        (ns_is_release ? ns_release_profile
                       : 'development (debug symbols + frame pointer + hardened)'))

# web-text-lib enforces the text-stack floors from inside its own build, where
# a too-old library surfaces as a bare version mismatch and only the first of
# several is reported. Checking every floor here first names them all at once.
# The floors are named once here and reused by every dependency() call on
# these libraries, here and in src/web-text-lib/.
ns_floors = {
  'glib-2.0': '>=2.80',
  'gobject-2.0': '>=2.80',
  'gio-2.0': '>=2.80',
  'cairo': '>=1.18',
  'harfbuzz': '>=8.3',
  'fribidi': '>=1.0.6',
  'fontconfig': '>=2.15',
  'gtk4': host_system == 'windows' ? '>=4.22.1' : '>=4.14',
  'libcurl': '>=8.5',
}
ns_unmet_floors = []
foreach name, floor : ns_floors
  if not dependency(name, version: floor, required: false).found()
    installed = dependency(name, required: false)
    ns_unmet_floors += '  @0@ @1@ (@2@)'.format(name, floor,
      installed.found() ? 'found ' + installed.version() : 'not found')
  endif
endforeach
if ns_unmet_floors.length() > 0
  error('Northstar Web Browser needs newer system libraries than this machine has:\n' +
        '\n'.join(ns_unmet_floors) + '\n' +
        'Ubuntu 24.04, Debian 13 and Fedora 40 are the oldest releases of ' +
        'their distributions that ship all of them; see docs/building.md.')
endif

glib    = dependency('glib-2.0', version: ns_floors['glib-2.0'])
gobject = dependency('gobject-2.0', version: ns_floors['gobject-2.0'])
gio     = dependency('gio-2.0', version: ns_floors['gio-2.0'])
cairo   = dependency('cairo', version: ns_floors['cairo'])

gtk4 = dependency('gtk4', version: ns_floors['gtk4'])
if host_system == 'windows'
  message('GTK @0@ — Windows requires the MSYS2 stock GTK (>= 4.22.1).'.format(gtk4.version()))
elif gtk4.version().version_compare('>=4.22')
  message('GTK @0@ — preferred (>= 4.22).'.format(gtk4.version()))
else
  warning('GTK @0@ meets the 4.14 minimum, but Northstar Web Browser prefers >= 4.22 for the newer GSK renderers.'.format(gtk4.version()))
endif
libcurl = dependency('libcurl', version: ns_floors['libcurl'])
if libcurl.version().version_compare('<8.11')
  warning('libcurl @0@ predates stable WebSocket support (curl 8.11): WebSocket will be unavailable at runtime unless this libcurl was built with the ws/wss protocols.'.format(libcurl.version()))
endif
libcrypto = dependency('libcrypto')
libm = cc.find_library('m', required: false)

libpsl = dependency('libpsl')

sqlite3 = dependency('sqlite3')

libfontconfig = dependency('fontconfig', version: ns_floors['fontconfig'],
                           required: false)
libfreetype = dependency('freetype2', required: false)

libdav1d = dependency('dav1d', version: '>= 1.0.0', required: get_option('av1'))
have_av1 = libdav1d.found()
libvpx = dependency('vpx', version: '>= 1.8.0', required: get_option('vp9'))
have_vp9 = libvpx.found()
libopenh264 = dependency('openh264', version: '>= 2.0.0', required: get_option('h264'))
have_h264 = libopenh264.found()
# GStreamer is the default <video> decoder on Linux; elsewhere it is used only
# when asked for explicitly with -Dgstreamer=enabled.
gst_wanted = get_option('gstreamer').disable_auto_if(host_system != 'linux')
libgstreamer = dependency('gstreamer-1.0', version: '>= 1.18', required: gst_wanted)
libgstapp = dependency('gstreamer-app-1.0', version: '>= 1.18', required: gst_wanted)
libgstvideo = dependency('gstreamer-video-1.0', version: '>= 1.18', required: gst_wanted)
have_gstreamer = libgstreamer.found() and libgstapp.found() and libgstvideo.found()
# Media Foundation is the default <video> decoder on Windows: the H.264
# decoder that ships with the system, plus the AV1 and VP9 Video Extensions
# where installed. mfplat.dll itself is loaded at runtime.
mf_wanted = get_option('mediafoundation').disable_auto_if(host_system != 'windows')
libmfuuid = cc.find_library('mfuuid', required: mf_wanted)
libole32 = cc.find_library('ole32', required: mf_wanted)
have_media_foundation = libmfuuid.found() and libole32.found() and \
                        cc.has_header('mftransform.h', required: mf_wanted)
libopus = dependency('opus', required: false)
libavif = dependency('libavif', required: get_option('avif'))
have_avif = libavif.found()

# WebGL 1 / 2 (src/webgl.c) maps canvas.getContext('webgl') onto an offscreen
# GL context (src/glctx.c: EGL, CGL on macOS, WGL on Windows) through
# libepoxy. Every shader is validated by the Khronos glslang front end before
# it reaches the GPU driver (src/glsl_validate.c), so WebGL is built only when
# glslang is present. glslang's pkg-config file names libraries that no longer
# exist (MachineIndependent, GenericCodeGen), so it is consulted only for the
# library and include directories, which Homebrew and the BSD package prefixes
# keep off the default search paths; the two real libraries are looked up
# directly.
webgl_feature = get_option('webgl')
have_webgl = false
webgl_deps = []
if webgl_feature.allowed()
  epoxy = dependency('epoxy', required: webgl_feature)
  have_egl = epoxy.found() and cc.has_header('epoxy/egl.h', dependencies: epoxy)
  have_gl_backend = host_system in ['darwin', 'windows'] or have_egl
  glslang_pc = dependency('glslang', method: 'pkg-config', required: false)
  glslang_lib_dirs = []
  glslang_cflags = []
  if glslang_pc.found()
    glslang_libdir = glslang_pc.get_variable(pkgconfig: 'libdir', default_value: '')
    glslang_incdir = glslang_pc.get_variable(pkgconfig: 'includedir', default_value: '')
    if glslang_libdir != ''
      glslang_lib_dirs = [glslang_libdir]
    endif
    if glslang_incdir != ''
      glslang_cflags = ['-I' + glslang_incdir]
    endif
  else
    glslang_prefixes = {
      'darwin': ['/opt/homebrew', '/usr/local'],
      'freebsd': ['/usr/local'],
      'dragonfly': ['/usr/local'],
      'openbsd': ['/usr/local'],
      'netbsd': ['/usr/pkg'],
    }.get(host_system, [])
    foreach glslang_prefix : glslang_prefixes
      glslang_lib_dirs += glslang_prefix / 'lib'
      glslang_cflags += '-I' + glslang_prefix / 'include'
    endforeach
  endif
  glslang_lib = cc.find_library('glslang', dirs: glslang_lib_dirs,
                                required: false)
  glslang_limits = cc.find_library('glslang-default-resource-limits',
                                   dirs: glslang_lib_dirs, required: false)
  have_glslang = glslang_lib.found() and glslang_limits.found() and \
                 cc.has_header('glslang/Include/glslang_c_interface.h',
                               args: glslang_cflags) and \
                 cc.has_header('glslang/Public/resource_limits_c.h',
                               args: glslang_cflags)
  have_webgl = epoxy.found() and have_gl_backend and have_glslang
  if have_webgl
    webgl_deps = [epoxy, declare_dependency(compile_args: glslang_cflags),
                  glslang_lib, glslang_limits]
    # glslang uses threads. Not on Windows: the threads dependency puts
    # -pthread on the link line, which clang there does not use and, under
    # --werror with LTO, rejects (-Wunused-command-line-argument).
    if host_system != 'windows'
      webgl_deps += dependency('threads')
    endif
    # glslang is C++ behind a C interface; a static glslang needs the C++
    # runtime named on the link line of this C program.
    cxx_runtime_name = host_system == 'darwin' ? 'c++' : 'stdc++'
    cxx_runtime = cc.find_library(cxx_runtime_name, required: false)
    if cxx_runtime.found()
      webgl_deps += cxx_runtime
    elif host_system != 'darwin' and cc.has_link_argument('-l:libstdc++.so.6')
      # No C++ compiler installed, so no libstdc++.so development link:
      # name the runtime library itself.
      webgl_deps += declare_dependency(link_args: ['-l:libstdc++.so.6'])
    endif
    if host_system == 'darwin'
      webgl_deps += dependency('appleframeworks', modules: ['OpenGL'])
    endif
  elif webgl_feature.enabled()
    error('-Dwebgl=enabled needs libepoxy (with EGL on Linux and the BSDs) and the glslang shader validator: install libepoxy-dev and glslang-dev (Debian/Ubuntu), libepoxy-devel and glslang-devel (Fedora/openSUSE) or libepoxy and glslang (Arch, Alpine, FreeBSD, MSYS2, Homebrew)')
  else
    warning('libepoxy or the glslang shader validator was not found: building WITHOUT WebGL. Install libepoxy-dev and glslang-dev (or your distribution\'s equivalents), or pass -Dwebgl=disabled to silence this warning.')
  endif
endif

# PDF.js, embedded into the PDF viewer page at build time; see src/pdf.c.
# -Dpdfjs is optional (auto) or required (enabled). It comes from an
# installed copy named by -Dpdfjs_dir, which must then be a supported
# version, or else from the release pinned in subprojects/pdfjs-dist.wrap.
pdfjs_embedded = {}
pdfjs_standard_fonts = []
pdfjs_source = 'not built'
if not get_option('pdfjs').disabled()
  if get_option('pdfjs_dir') != ''
    # Both the pdfjs-dist npm layout (build/pdf.min.mjs, wasm/,
    # standard_fonts/) and the generic viewer layout distributions install
    # (build/pdf.mjs, web/wasm/, web/standard_fonts/) are accepted, minified
    # or not. The viewer (data/js/pdf-viewer.js) is written against 6.5 to 6.x.
    pdfjs_root = get_option('pdfjs_dir')
    if not fs.is_dir(pdfjs_root)
      error('-Dpdfjs_dir: @0@ is not a directory'.format(pdfjs_root))
    endif
    pdfjs_candidates = {
      'pdfjs_lib': ['build/pdf.min.mjs', 'build/pdf.mjs'],
      'pdfjs_worker': ['build/pdf.worker.min.mjs', 'build/pdf.worker.mjs'],
      'pdfjs_openjpeg': ['wasm/openjpeg.wasm', 'web/wasm/openjpeg.wasm'],
      'pdfjs_jbig2': ['wasm/jbig2.wasm', 'web/wasm/jbig2.wasm'],
      'pdfjs_qcms': ['wasm/qcms_bg.wasm', 'web/wasm/qcms_bg.wasm'],
    }
    foreach name, rels : pdfjs_candidates
      pdfjs_found = ''
      foreach rel : rels
        if pdfjs_found == '' and fs.is_file(pdfjs_root / rel)
          pdfjs_found = pdfjs_root / rel
        endif
      endforeach
      if pdfjs_found == ''
        error('-Dpdfjs_dir: @0@ has none of @1@'.format(pdfjs_root,
                                                        ', '.join(rels)))
      endif
      pdfjs_embedded += {name: files(pdfjs_found)}
    endforeach

    # Glyph data for PDF's standard fonts when a document does not embed
    # them. Only the BSD-licensed Foxit fonts (PDFium) are embedded; PDF.js's
    # Liberation Sans, under GPL-2.0 with a font exception, is left out, and
    # Helvetica/Arial then draw with a system sans-serif font.
    pdfjs_font_dir = ''
    foreach rel : ['standard_fonts', 'web/standard_fonts']
      if pdfjs_font_dir == '' and fs.is_dir(pdfjs_root / rel)
        pdfjs_font_dir = pdfjs_root / rel
      endif
    endforeach
    if pdfjs_font_dir == ''
      error('-Dpdfjs_dir: @0@ has neither standard_fonts nor web/standard_fonts'.format(pdfjs_root))
    endif
    foreach name : ['FoxitDingbats.pfb', 'FoxitFixed.pfb', 'FoxitFixedBold.pfb',
                    'FoxitFixedBoldItalic.pfb', 'FoxitFixedItalic.pfb',
                    'FoxitSerif.pfb', 'FoxitSerifBold.pfb',
                    'FoxitSerifBoldItalic.pfb', 'FoxitSerifItalic.pfb',
                    'FoxitSymbol.pfb']
      if not fs.is_file(pdfjs_font_dir / name)
        error('-Dpdfjs_dir: @0@ lacks the standard font @1@'.format(
          pdfjs_font_dir, name))
      endif
      pdfjs_standard_fonts += files(pdfjs_font_dir / name)
    endforeach

    # The library and worker each state "pdfjsVersion = X.Y.Z" in their
    # header comment; both must agree and fall in the supported range.
    pdfjs_versions = []
    foreach name : ['pdfjs_lib', 'pdfjs_worker']
      pdfjs_text = fs.read(pdfjs_embedded[name][0])
      if pdfjs_text.to_lower().contains('</script')
        error('-Dpdfjs_dir: @0@ contains "</script" and cannot be inlined in a script element'.format(name))
      endif
      pdfjs_parts = pdfjs_text.split('pdfjsVersion')
      if pdfjs_parts.length() < 2
        error('-Dpdfjs_dir: @0@ does not state a pdfjsVersion'.format(name))
      endif
      pdfjs_v = pdfjs_parts[1].split('\n')[0].split(',')[0].split(';')[0]
      pdfjs_v = pdfjs_v.strip().strip('=').strip().strip('"').strip()
      pdfjs_digits = pdfjs_v
      foreach c : ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9']
        pdfjs_digits = pdfjs_digits.replace(c, '')
      endforeach
      if pdfjs_v.split('.').length() != 3 or pdfjs_digits != '..'
        error('-Dpdfjs_dir: @0@ states an unreadable pdfjsVersion "@1@"'.format(
          name, pdfjs_v))
      endif
      pdfjs_versions += pdfjs_v
    endforeach
    pdfjs_version = pdfjs_versions[0]
    if pdfjs_versions[1] != pdfjs_version
      error('-Dpdfjs_dir: PDF.js library @0@ and worker @1@ differ in version'.format(
        pdfjs_version, pdfjs_versions[1]))
    endif
    if not (pdfjs_version.version_compare('>=6.5.0') and
            pdfjs_version.version_compare('<7'))
      error('-Dpdfjs_dir: PDF.js @0@ in @1@ is outside the supported range 6.5.0 to 6.x'.format(
        pdfjs_version, pdfjs_root))
    endif
    pdfjs_source = pdfjs_version + ' from ' + pdfjs_root
  else
    pdfjs_sub = subproject('pdfjs-dist', required: get_option('pdfjs'))
    if pdfjs_sub.found()
      pdfjs_embedded = pdfjs_sub.get_variable('pdfjs_embedded')
      pdfjs_standard_fonts = pdfjs_sub.get_variable('pdfjs_standard_fonts', [])
      pdfjs_version = pdfjs_sub.get_variable('pdfjs_version', 'pinned')
      pdfjs_source = pdfjs_version + ' (subprojects/pdfjs-dist.wrap)'
    endif
  endif
endif
have_pdfjs = pdfjs_embedded.keys().length() > 0
if have_pdfjs and pdfjs_standard_fonts.length() == 0
  error('pdfjs-dist: no standard fonts; the extracted subproject predates ' +
        'them, delete subprojects/pdfjs-dist-*/ and set up again')
endif

# vorbisfile and opusfile are the high-level decode APIs over libvorbis and
# libopus + libogg, used by the in-process SDL3 audio mixer to decode
# Ogg/Vorbis and Ogg/Opus natively.
sdl3_dep = dependency('sdl3', version: '>=3.2.0', required: get_option('audio'))
libvorbisfile = dependency('vorbisfile', required: false)
libopusfile = dependency('opusfile', required: false)
libenchant = dependency('enchant-2', required: false)
have_enchant = libenchant.found()

libseccomp = dependency('libseccomp', required: host_system == 'linux')
have_seccomp = libseccomp.found()

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])
libplmpeg = dependency('plmpeg', fallback: ['plmpeg', 'plmpeg_dep'])
zlib = dependency('zlib')

# Brotli and zstd Content-Encoding; without them they are not advertised.
libbrotlidec = dependency('libbrotlidec', required: false)
libzstd = dependency('libzstd', required: false)

# A system lexbor 3 is used when there is one, else the pinned subproject.
liblexbor = dependency('lexbor', version: '>=3.0.0',
                       fallback: ['lexbor', 'lexbor_dep'])

# Feature macros for the sources, each defined when its condition holds.
# src/media_types.c may advertise an audio type only where the decoder that
# plays it is built: the SDL3 in-process mixer.
ns_feature_macros = {
  'NS_HAVE_FONTCONFIG=1': libfontconfig.found(),
  'NS_HAVE_FREETYPE=1': libfreetype.found(),
  'NS_HAVE_AV1': have_av1,
  'NS_HAVE_VP9': have_vp9,
  'NS_HAVE_H264': have_h264,
  'NS_HAVE_GSTREAMER': have_gstreamer,
  'NS_HAVE_MEDIA_FOUNDATION': have_media_foundation,
  'NS_HAVE_OPUS': libopus.found(),
  'NS_HAVE_AVIF=1': have_avif,
  'NS_ENABLE_WEBGL=1': have_webgl,
  'NS_HAVE_CGL=1': have_webgl and host_system == 'darwin',
  'NS_HAVE_EGL=1': have_webgl and host_system not in ['darwin', 'windows'],
  'NS_HAVE_PDFJS=1': have_pdfjs,
  'NS_HAVE_VORBISFILE=1': libvorbisfile.found(),
  'NS_HAVE_OPUSFILE=1': libopusfile.found(),
  'NS_AUDIO_PLAYBACK=1': sdl3_dep.found(),
  'NS_AUDIO_NATIVE_VORBIS=1': sdl3_dep.found() and libvorbisfile.found(),
  'NS_AUDIO_NATIVE_OPUS=1': sdl3_dep.found() and libopusfile.found(),
  'NS_HAVE_ENCHANT=1': have_enchant,
  'NS_HAVE_SECCOMP=1': have_seccomp,
  'NS_HAVE_BROTLI_DEC=1': libbrotlidec.found(),
  'NS_HAVE_ZSTD=1': libzstd.found(),
}
foreach macro, enabled : ns_feature_macros
  if enabled
    add_project_arguments('-D' + macro, language: 'c')
  endif
endforeach
add_project_arguments('-DNS_LEXBOR_VERSION="' + liblexbor.version() + '"',
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

js_engine = get_option('js_engine')
if js_engine == 'quickjs'
  libquickjs = subproject('quickjs').get_variable('quickjs_dep')
  add_project_arguments('-DNS_QUICKJS_ORIGINAL=1',
                        '-DNS_QUICKJS_VERSION="' + libquickjs.version() + '"',
                        language: 'c')
else
  libquickjs = dependency('quickjs-ng', fallback: ['quickjs-ng', 'qjs_dep'])
endif

# WAMR's call stubs cover x86-64 and 64-bit Arm (not Arm Windows).
wasm_feature = get_option('wasm').require(
  host_machine.cpu_family() == 'x86_64' or
    (host_machine.cpu_family() == 'aarch64' and host_system != 'windows'),
  error_message: 'wasm (WAMR) is built only for x86-64 and 64-bit Arm ' +
                 '(not Arm Windows); build with -Dwasm=disabled')
have_wasm = wasm_feature.allowed()
if have_wasm
  # WAMR is the upstream release pinned in subprojects/wamr.wrap. A subproject
  # does not inherit this project's arguments, so the hardening and
  # code-generation flags are handed to it.
  wamr_parent_c_args = cc.get_supported_arguments(
    hardening_c_flags + ['-ffunction-sections', '-fdata-sections'])
  if not ns_is_release
    wamr_parent_c_args += cc.get_supported_arguments(
      ['-g3', '-fno-omit-frame-pointer'])
  endif
  libwamr = subproject('wamr',
    default_options: {'parent_c_args': wamr_parent_c_args},
  ).get_variable('libwamr')
else
  libwamr = declare_dependency()
endif

# Every file in these two directories is installed, so a new icon or
# translation needs no change here.
install_subdir(
  'data/icons/hicolor/scalable/apps',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable',
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

install_subdir(
  'data/i18n',
  install_dir: get_option('datadir') / 'northstar',
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

summary({
  'WebAssembly (WAMR)': have_wasm,
  'PDF viewer (PDF.js)': pdfjs_source,
  'AV1 video (dav1d)': have_av1,
  'VP9 video (libvpx)': have_vp9,
  'H.264 video (OpenH264)': have_h264,
  'Video via GStreamer': have_gstreamer,
  'Video via Media Foundation': have_media_foundation,
  'Opus in WebM (libopus)': libopus.found(),
  'AVIF images (libavif)': have_avif,
  'WebGL 1 / 2 (libepoxy, glslang)': have_webgl,
  'Audio playback (SDL3)': sdl3_dep.found(),
  'JavaScript engine': js_engine + ' ' + libquickjs.version(),
}, section: 'Features')

# Text is shaped through web-text-lib (src/web-text-lib/), the in-tree Pango
# fork that caches finished glyph strings across layouts; every symbol in it
# is renamed, because GTK loads the system Pango into the same process. It
# declares targets, so it comes after the last project argument.
subdir('src/web-text-lib')
pango       = web_text_dep
pangocairo  = web_text_cairo_dep
libpangoft2 = web_text_fc_dep

subdir('src')
