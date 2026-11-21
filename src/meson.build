python = import('python').find_installation()

qjs_tools_found = false
if js_engine == 'quickjs-ng' and not meson.is_cross_build()
  qjsc_prog = find_program('qjsc', required: false)
  qjs_prog  = find_program('qjs',  required: false)
  qjs_tools_found = qjsc_prog.found() and qjs_prog.found()
endif

if qjs_tools_found
  # Build-time check of data/js/polyfills.js: qjsc compiles it to catch syntax
  # errors, then qjs loads it and runs scripts/verify-polyfills.js to catch a
  # missing or broken API. Either failing fails the build; the outputs are
  # stamps so the check reruns only when an input changes.
  polyfills_syntax = custom_target(
    'polyfills-syntax',
    input: '../data/js/polyfills.js',
    output: 'polyfills.qjsc.c',
    command: [qjsc_prog, '-C', '-P', '-n', 'polyfills.js',
              '-o', '@OUTPUT@', '@INPUT@'],
    build_by_default: true,
  )
  polyfills_verify = custom_target(
    'polyfills-verify',
    input: ['../data/js/polyfills.js', '../scripts/verify-polyfills.js'],
    output: 'polyfills.verified',
    command: [qjs_prog, '-I', '@INPUT0@', '@INPUT1@'],
    capture: true,
    depends: polyfills_syntax,
    build_by_default: true,
  )
endif

# Files compiled into the binary, each as a NUL-terminated
# `static const char <symbol>[]` in its own generated header:
# [header, file, symbol].
ns_embeds = [
  ['polyfills.h', files('../data/js/polyfills.js'), 'ns_js_polyfills_src'],
  ['wpt_hook.h', files('../data/js/wpt-hook.js'), 'ns_wpt_hook_src'],
]

# The Google Safe Browsing API key, read from NS_SAFE_BROWSING_API_KEY at
# build time into a generated header so it never sits in the tree or on a
# compiler command line. Empty when unset: only the local list is used.
sb_key_h = custom_target(
  'ns_sb_key.h',
  input: '../scripts/gen-safebrowsing-key.py',
  output: 'ns_sb_key.h',
  command: [python, '@INPUT@', '@OUTPUT@'],
  build_always_stale: true,
)

# PDF.js and the viewer script, embedded as C strings for src/pdf.c.
pdf_viewer_embedded = {}
if have_pdfjs
  pdf_viewer_embedded = pdfjs_embedded + {
    'pdf_viewer_js': files('../data/js/pdf-viewer.js'),
    'pdf_viewer_css': files('../data/js/pdf-viewer.css'),
  }
endif
pdf_viewer_headers = []
foreach name, src : pdf_viewer_embedded
  ns_embeds += [[name + '.h', src, 'ns_' + name]]
endforeach
if have_pdfjs
  # Each standard font is embedded like any other file (with the trailing
  # NUL left out of the length), and pdfjs_fonts.h is a table over them,
  # written at configure time.
  pdfjs_font_includes = []
  pdfjs_font_rows = []
  foreach font : pdfjs_standard_fonts
    i = pdfjs_font_rows.length()
    ns_embeds += [['pdfjs_font_@0@.h'.format(i), [font],
                   'ns_pdfjs_font_@0@'.format(i)]]
    pdfjs_font_includes += '#include "pdfjs_font_@0@.h"'.format(i)
    pdfjs_font_rows += ('    { "@0@", (const unsigned char *)ns_pdfjs_font_@1@,' +
                        ' sizeof ns_pdfjs_font_@1@ - 1 },').format(
      fs.name(font), i)
  endforeach
  pdfjs_fonts_conf = configuration_data()
  pdfjs_fonts_conf.set('PDFJS_FONT_INCLUDES', '\n'.join(pdfjs_font_includes))
  pdfjs_fonts_conf.set('PDFJS_FONT_ROWS', '\n'.join(pdfjs_font_rows))
  pdf_viewer_headers += configure_file(
    input: 'pdfjs_fonts.h.in',
    output: 'pdfjs_fonts.h',
    configuration: pdfjs_fonts_conf,
  )
endif

# With a C23 #embed compiler (GCC 15, Clang 19 and later) each header is a
# two-line #embed written at configure time: no generator runs, the compiler
# reads the file itself and records it in the dependency file, so editing it
# rebuilds only what includes it. Older compilers get the same array spelled
# out byte by byte by scripts/embed-text.py at build time.
ns_have_embed = cc.compiles('''
static const char ns_embed_probe[] = {
#embed "@0@" suffix(, 0) if_empty(0)
};
int ns_embed_probe_len(void);
int ns_embed_probe_len(void) { return (int)sizeof ns_embed_probe; }
'''.format((meson.project_source_root() / 'LICENSE').replace('\\', '/')),
  args: ['-Werror'],
  name: 'C23 #embed')
ns_embedded_headers = []
foreach e : ns_embeds
  if ns_have_embed
    # fs.parent() gives a File's directory relative to the source root
    # (or absolute, for a -Dpdfjs_dir file); #embed needs an absolute path.
    embed_path = (meson.project_source_root() / fs.parent(e[1][0]) /
                  fs.name(e[1][0])).replace('\\', '/')
    embed_conf = configuration_data()
    embed_conf.set('SYMBOL', e[2])
    embed_conf.set('PATH', embed_path)
    ns_embedded_headers += configure_file(
      input: 'embed.h.in',
      output: e[0],
      configuration: embed_conf,
    )
  else
    ns_embedded_headers += custom_target(
      e[0],
      input: ['../scripts/embed-text.py', e[1]],
      output: e[0],
      command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@', e[2]],
    )
  endif
endforeach

northstar_engine_sources = files(
  'anim.c',
  'bytecode_cache.c',
  'bookmarks.c',
  'cache.c',
  'config.c',
  'csp.c',
  'css.c',
  'css_calc.c',
  'css_media.c',
  'css_prop_syntax.c',
  'css_syntax.c',
  'datetime.c',
  'debuglog.c',
  'damage.c',
  'dom.c',
  'download_name.c',
  'encoding.c',
  'engine.c',
  'mainctx.c',
  'eventsource.c',
  'ext.c',
  'fetch_policy.c',
  'forms.c',
  'camera.c',
  'font.c',
  'history.c',
  'html.c',
  'i18n.c',
  'image.c',
  'smil.c',
  'svg.c',
  'video.c',
  'av1.c',
  'mse.c',
  'progressive.c',
  'mp4.c',
  'webm.c',
  'yuv.c',
  'vp9.c',
  'avc.c',
  'h264.c',
  'videodec.c',
  'gstvideo.c',
  'mfvideo.c',
  'videoworker.c',
  'videolayer.c',
  'audiodec.c',
  'audio/aac.c',
  'js_mse.c',
  'image_ico.c',
  'image_wuffs.c',
  'idb.c',
  'js.c',
  'js_canvas.c',
  'js_intl.c',
  'js_date.c',
  'js_encoding.c',
  'js_media.c',
  'js_perf.c',
  'js_realm.c',
  'js_receiver.c',
  'js_webcrypto.c',
  'quickjs_compat.c',
  'layout.c',
  'mathml.c',
  'media_types.c',
  'net.c',
  'netutil.c',
  'content_decode.c',
  'paint.c',
  'pdf.c',
  'print.c',
  'render.c',
  'safebrowsing.c',
  'safebrowsing_google.c',
  'security.c',
  'shared_buffer.c',
  'selection.c',
  'spellcheck.c',
  'sqlite_db.c',
  'texture.c',
  'trace.c',
  'wasm.c',
  'webaudio.c',
  'webcrypto.c',
  'ws.c',
  'html_lexbor.c',
  'xml.c',
)

# GTK-free helpers used by the desktop executable but not the engine UI:
# the headless render driver and the crash watchdog. Kept in src/ as common
# code; the GTK frontend itself lives in src/gtk/.
northstar_app_sources = files(
  'headless.c',
  'watchdog.c',
)

if have_avif
  northstar_engine_sources += files('image_avif.c')
endif

# Always compiled: without NS_ENABLE_WEBGL they reduce to stubs.
northstar_engine_sources += files('webgl.c', 'glctx.c', 'glsl_validate.c')

northstar_win_resources = []
if host_machine.system() == 'windows'
  windows = import('windows')
  northstar_win_resources = windows.compile_resources(
    'northstar.rc',
    depend_files: files('../data/icons/northstar.ico'),
  )
endif

northstar_deps = [
  glib,
  gobject,
  gio,
  cairo,
  pango,
  pangocairo,
  libcurl,
  libcrypto,
  libm,
  libquickjs,
  libwuffs,
  libplmpeg,
  liblexbor,
  libwamr,
  sqlite3,
  zlib,
  libbrotlidec,
  libzstd,
]

if have_seccomp
  northstar_deps += [libseccomp]
endif

if have_av1
  northstar_deps += [libdav1d]
endif
if have_vp9
  northstar_deps += [libvpx]
endif
if have_h264
  northstar_deps += [libopenh264]
endif
if have_gstreamer
  northstar_deps += [libgstreamer, libgstapp, libgstvideo]
endif
if have_media_foundation
  northstar_deps += [libmfuuid, libole32]
endif
if libopus.found()
  northstar_deps += [libopus]
endif
if have_avif
  northstar_deps += [libavif]
endif
if have_webgl
  northstar_deps += webgl_deps
endif

if have_enchant
  northstar_deps += [libenchant]
endif

if have_fontconfig
  northstar_deps += [libfontconfig]
endif

northstar_deps += [libpangoft2]

if have_freetype
  northstar_deps += [libfreetype]
endif

northstar_deps += [libpsl]

northstar_win_subsystem = (
  host_machine.system() == 'windows' ? 'windows' : 'console'
)

northstar_link_args = []
if host_machine.system() == 'windows'
  northstar_link_args += cc.get_supported_link_arguments(
    ['-Wl,--stack,8388608'],
  )
  northstar_link_args += ['-lbcrypt', '-ladvapi32', '-lshell32', '-luser32']
endif
if host_machine.system() == 'linux'
  northstar_link_args += cc.get_supported_link_arguments(
    ['-Wl,-z,stack-size=2097152'],
  )
endif

# NetBSD pkgsrc gnutls (pulled in transitively) carries an undefined
# reference to libc's mbrtoc32, which the hardened --no-undefined link
# rejects even though the dynamic linker resolves it at runtime. Allow
# undefined symbols originating from shared libraries on NetBSD only; our
# own objects are still checked.
if host_machine.system() == 'netbsd'
  northstar_link_args += cc.get_supported_link_arguments(
    ['-Wl,--allow-shlib-undefined'],
  )
endif

if host_machine.system() == 'windows'
  executable(
    'northstar-launcher',
    files('win_launcher.c') + northstar_win_resources,
    pie: false,
    win_subsystem: 'windows',
    link_args: ['-luser32', '-lshell32'],
  )
endif

# Optional in-process audio playback.
northstar_audio_sources = files('audio/stub.c')
northstar_audio_deps = []
northstar_audio_libs = []
sdl3_dep = dependency('sdl3', version: '>=3.2.0', required: get_option('audio'))
if sdl3_dep.found()
  libminimp3 = static_library(
    'minimp3',
    files('audio/minimp3_impl.c'),
    include_directories: include_directories('audio'),
    c_args: cc.get_supported_arguments(['-w']),
    install: false,
  )

  northstar_audio_sources = files('audio/audio.c')
  northstar_audio_deps = [sdl3_dep, libplmpeg]
  northstar_audio_libs = [libminimp3]
  if libvorbisfile.found()
    northstar_audio_deps += [libvorbisfile]
  endif
  if libopusfile.found()
    northstar_audio_deps += [libopusfile]
  endif
  # libm is needed by pl_mpeg / minimp3 on Linux and the BSDs; it is part of
  # the C runtime on Windows, where find_library('m') is a harmless no-op.
  northstar_audio_deps += [cc.find_library('m', required: false)]
else
  message('SDL3 not found — in-process audio playback disabled')
endif

subdir('gtk')
