# Headers that compile files into the binary (see ns_embeds in
# src/meson.build), plus pdfjs_fonts.h, the table over the embedded fonts.
if have_pdfjs
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
  werror: true,
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
      input: ['../../scripts/embed-text.py', e[1]],
      output: e[0],
      command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@', e[2]],
    )
  endif
endforeach
