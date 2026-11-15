project('pdfjs-dist', version: '6.5.438', license: 'Apache-2.0',
        meson_version: '>=1.1')

pdfjs_version = meson.project_version()

pdfjs_embedded = {
  'pdfjs_lib': files('package/build/pdf.min.mjs'),
  'pdfjs_worker': files('package/build/pdf.worker.min.mjs'),
  'pdfjs_openjpeg': files('package/wasm/openjpeg.wasm'),
  'pdfjs_jbig2': files('package/wasm/jbig2.wasm'),
  'pdfjs_qcms': files('package/wasm/qcms_bg.wasm'),
}

# Glyph data for the standard PDF fonts a document may use unembedded:
# only the BSD-licensed Foxit fonts. Liberation Sans (GPL-2.0 with a font
# exception) is left out; Helvetica/Arial then use a system sans-serif.
pdfjs_standard_fonts = []
foreach name : ['FoxitDingbats.pfb', 'FoxitFixed.pfb', 'FoxitFixedBold.pfb',
                'FoxitFixedBoldItalic.pfb', 'FoxitFixedItalic.pfb',
                'FoxitSerif.pfb', 'FoxitSerifBold.pfb',
                'FoxitSerifBoldItalic.pfb', 'FoxitSerifItalic.pfb',
                'FoxitSymbol.pfb']
  pdfjs_standard_fonts += files('package/standard_fonts/' + name)
endforeach

fs = import('fs')
foreach name : ['pdfjs_lib', 'pdfjs_worker']
  if fs.read(pdfjs_embedded[name][0]).to_lower().contains('</script')
    error('pdfjs-dist: ' + name + ' contains "</script" and cannot be inlined in a script element')
  endif
endforeach
