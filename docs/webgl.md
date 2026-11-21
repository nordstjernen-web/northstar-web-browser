# WebGL

Northstar has a **minimalist WebGL 1 and WebGL 2 implementation**, ported
from [Nordstjernen Sol](https://github.com/nordstjernen-web/nordstjernen-sol-browser).
It maps the WebGL API more or less directly onto OpenGL through an
**offscreen GL context** (`src/glctx.c`) and **libepoxy** for GL dispatch.
WebGL renders into a framebuffer object, and the result is read back and
composited by the page painter like any other canvas, so it needs no
window and no display server: it works in the GUI and in `--headless`
runs alike. There is no ANGLE layer and no GL command-stream validator
between the page and the driver: WebGL calls become GL calls, after the
checks listed under [Security](#security).

Northstar is single-process, so the GPU driver runs **inside the browser
process**, on the engine thread. Read [Security](#security) before
relying on it for hostile content.

## Availability and control

`canvas.getContext("webgl")` (or `"experimental-webgl"`, `"webgl2"`)
creates a context when WebGL is built in and enabled. There is no
permission prompt and no per-site setting.

- **Settings.** *Enable WebGL* on `about:settings` (`webgl_enabled` in the
  config file, default `true`). Turning it **off** takes effect at once:
  later `getContext("webgl")` calls return `null`. Turning it **on** takes
  effect at the next start, because the sandbox is set up for a GPU
  driver, or not, when the browser starts.
- **Environment.** `NS_NO_WEBGL=1` turns it off for one run.
- **Build.** `-Dwebgl=disabled` leaves it out; `-Dwebgl=enabled` makes
  libepoxy and glslang required. With the default `auto`, WebGL is built
  when both are found. A build without WebGL defines no
  `WebGLRenderingContext`.

A canvas has one context type for life: after `getContext("2d")`,
`getContext("webgl")` returns `null`, and the reverse. WebGL is not
available in workers.

## How it works

`src/webgl.c` is the binding, `src/glctx.c` the context, and
`src/glsl_validate.c` the shader validator.

1. **Context.** `ns_gl_context_create()` creates the offscreen context.
   On Linux and the BSDs it is a surfaceless **EGL** context bound to the
   OpenGL ES API, preferring `EGL_MESA_platform_surfaceless`, with a 1×1
   pbuffer fallback; GLES is requested so GLSL ES shaders (`#version 100`,
   `#version 300 es`) compile natively. It is always created with
   `EGL_EXT_create_context_robustness`; a driver without that extension
   gets no WebGL context. On Windows it is a desktop OpenGL context
   created through **WGL** on a hidden window, with
   `WGL_ARB_create_context_robustness` when the driver has it and an
   ordinary context when it does not (AMD's driver, for one). On macOS it is a **CGL** context
   on the GL 4.1 core profile, which gives WebGL 1 only:
   `getContext("webgl2")` returns `null` there, because Apple's OpenGL
   cannot compile GLSL ES 3.00.
2. **Render target.** The context renders into a framebuffer object sized
   to the canvas, with a colour texture and, when requested, a
   depth/stencil renderbuffer. With `antialias` (the default) and a driver
   that supports it, drawing goes to a 4× multisample buffer that is
   resolved before readback.
3. **Compositing.** After the page draws, the buffer is read back with
   `glReadPixels`, flipped, premultiplied and written into a Cairo image
   surface, which the canvas paint path (`src/paint.c`) blits. Readback
   happens only after a draw, so a one-shot render stays on screen and a
   `requestAnimationFrame` loop updates every frame. `toDataURL()`,
   `toBlob()` and a 2D context's `drawImage()` read the same surface.

WebGL objects (`WebGLBuffer`, `WebGLTexture`, `WebGLProgram`, …) are thin
JS wrappers around the GL integer name.

## Supported API

This is a pragmatic core, not a conformance-complete implementation.

### WebGL 1

- Context attributes: `alpha`, `depth`, `stencil`, `antialias`,
  `premultipliedAlpha`, `preserveDrawingBuffer` (parsed and honoured),
  plus `getContextAttributes()`.
- State: `enable`/`disable`/`isEnabled`, `clearColor`/`clearDepth`/
  `clearStencil`/`clear`, `viewport`, `scissor`, `colorMask`, `depthFunc`/
  `depthMask`/`depthRange`, `blend*`, `stencil*`, `cullFace`/`frontFace`,
  `lineWidth`, `polygonOffset`, `hint`, `pixelStorei` (including
  `UNPACK_FLIP_Y_WEBGL` / `UNPACK_PREMULTIPLY_ALPHA_WEBGL`), `finish`,
  `flush`, `getParameter`, `getError`.
- Shaders & programs: `createShader`, `shaderSource`, `compileShader`,
  `getShaderParameter`/`getShaderInfoLog`/`getShaderSource`,
  `createProgram`, `attachShader`/`detachShader`, `linkProgram`,
  `useProgram`, `validateProgram`, `getProgramParameter`/
  `getProgramInfoLog`, `bindAttribLocation`, `getAttribLocation`,
  `getUniformLocation`, `getActiveAttrib`/`getActiveUniform`.
- Buffers: `createBuffer`, `bindBuffer`, `bufferData`, `bufferSubData`,
  `deleteBuffer`, `getBufferParameter`, `isBuffer`.
- Vertex attributes: `enableVertexAttribArray`,
  `disableVertexAttribArray`, `vertexAttribPointer`, `vertexAttrib[1-4]f`,
  `vertexAttrib[1-4]fv`, `getVertexAttrib`, `getVertexAttribOffset`.
- Uniforms: `uniform[1-4]f`, `uniform[1-4]i`, `uniform[1-4]fv`,
  `uniform[1-4]iv`, `uniformMatrix[2-4]fv`.
- Drawing: `drawArrays`, `drawElements`.
- Textures: `createTexture`, `bindTexture`, `activeTexture`,
  `texParameteri`/`texParameterf`, `generateMipmap`, `texImage2D`,
  `texSubImage2D` — from a typed array, from `ImageData`, and from an
  `<img>` / `<canvas>` / `ImageBitmap` source.
- Framebuffers / renderbuffers: full `*Framebuffer*` and `*Renderbuffer*`
  set, `checkFramebufferStatus`, `readPixels`. Binding framebuffer `null`
  binds the canvas drawing buffer (our FBO), not GL's default framebuffer.
- Queries: `isProgram`, `isShader`, `isTexture`, `isFramebuffer`,
  `isRenderbuffer`, `getTexParameter`, `getRenderbufferParameter`,
  `getFramebufferAttachmentParameter`.

### WebGL 2 (when `getContext("webgl2")`)

Everything above, plus:

- Vertex array objects (`createVertexArray` …), instanced drawing
  (`drawArraysInstanced`, `drawElementsInstanced`, `vertexAttribDivisor`),
  `drawBuffers`, `drawRangeElements`, `vertexAttribIPointer`,
  `vertexAttribI4{i,ui,iv,uiv}`.
- Unsigned-integer uniforms (`uniform[1-4]ui` / `uniform[1-4]uiv`) and
  non-square matrices (`uniformMatrix{2x3,3x2,2x4,4x2,3x4,4x3}fv`).
- 3D / 2D-array textures: `texImage3D`, `texSubImage3D`, `texStorage2D`,
  `texStorage3D`, `copyTexSubImage3D`; plus `copyTexImage2D` /
  `copyTexSubImage2D`.
- Framebuffers: `renderbufferStorageMultisample`, `blitFramebuffer`,
  `framebufferTextureLayer`, `invalidateFramebuffer`, `readBuffer`.
- Buffers: `copyBufferSubData`, `getBufferSubData` (via `glMapBufferRange`),
  `clearBuffer{fv,iv,uiv,fi}`, `bindBufferBase`, `bindBufferRange`.
- Sampler objects, uniform-block introspection / binding
  (`getUniformBlockIndex`, `uniformBlockBinding`, `getActiveUniforms`,
  `getActiveUniformBlockParameter`, `getActiveUniformBlockName`),
  `getFragDataLocation`, `getInternalformatParameter`.
- Query objects (`createQuery`, `beginQuery`/`endQuery`,
  `getQueryParameter`, …), transform feedback (`createTransformFeedback`,
  `transformFeedbackVaryings`, `begin`/`end`/`pause`/`resume`), and sync
  objects (`fenceSync`, `clientWaitSync`, `waitSync`, `getSyncParameter`).
- The WebGL 2 enum constants.

### Extensions

`getExtension()` returns one cached object per extension and context;
extensions that depend on the driver are advertised only when the GL context
can honour them (desktop GL always can).

- WebGL 1 only: `ANGLE_instanced_arrays`, `OES_vertex_array_object` (not on
  macOS), `OES_element_index_uint`, `OES_standard_derivatives`,
  `OES_texture_float`, `OES_texture_half_float`,
  `OES_texture_half_float_linear`, `WEBGL_depth_texture`, `EXT_blend_minmax`,
  `WEBGL_color_buffer_float`. Because the context underneath is GLES 3 or
  desktop GL, WebGL 1 float, half-float and depth uploads with an unsized
  internal format are mapped to the matching sized format (`RGBA32F`,
  `RGBA16F`, `DEPTH_COMPONENT16/24`, `DEPTH24_STENCIL8`) and `HALF_FLOAT_OES`
  to `HALF_FLOAT`.
- WebGL 2 only: `EXT_color_buffer_float`.
- Compressed textures, each offered when the driver can decode the format:
  `WEBGL_compressed_texture_s3tc`, `WEBGL_compressed_texture_s3tc_srgb`,
  `EXT_texture_compression_rgtc`, `EXT_texture_compression_bptc`,
  `WEBGL_compressed_texture_etc` and `WEBGL_compressed_texture_astc`.
  `compressedTex(Sub)Image2D/3D` check that the data is exactly the size the
  format and dimensions require (or read from a bound unpack buffer in
  WebGL 2), and `COMPRESSED_TEXTURE_FORMATS` lists the formats of the enabled
  extensions.
- Both: `EXT_color_buffer_half_float`, `EXT_float_blend`,
  `OES_texture_float_linear`, `EXT_texture_filter_anisotropic`,
  `KHR_parallel_shader_compile` (compilation is synchronous, so
  `COMPLETION_STATUS_KHR` is always true), `WEBGL_debug_renderer_info` and
  `WEBGL_lose_context` (`loseContext()` / `restoreContext()` flip
  `isContextLost()` and fire `webglcontextlost` / `webglcontextrestored` on
  the canvas).

## Security

WebGL is a thin bridge to the GPU driver, so the mitigations focus on the
parts a hostile page can reach. Two things are specific to Northstar:

- **The driver runs in the browser process.** Nordstjernen Sol runs WebGL
  in a sandboxed renderer process; Northstar has none. A bug in the GPU
  driver that a page can trigger is a bug in the whole browser.
- **A weaker content-thread filter.** GPU drivers compile shaders to
  machine code and share buffers through memfds. When WebGL is enabled at
  startup, the Linux content-thread seccomp filter therefore no longer
  refuses executable mappings, `PROT_EXEC` through `mprotect`, or
  `memfd_create` on the engine thread (and, in headless runs, in the whole
  process). It still refuses new `AF_UNIX` sockets. Landlock also grants
  the GPU device nodes in headless runs, as it already does in the GUI.
  With WebGL off (`webgl_enabled = false`, `NS_NO_WEBGL=1` or
  `-Dwebgl=disabled`) the filter is as strict as before.

The rest is shared with Nordstjernen Sol:

- **Global control.** The Settings switch can prevent all new context
  creation.
- **Shader validation ahead of the driver.** `shaderSource()` never hands
  page GLSL to the GPU driver unchecked. `src/glsl_validate.c` first applies
  the WebGL source rules — the GLSL ES source character set (other bytes
  only inside comments), identifiers of at most 256 characters, no
  `webgl_`/`_webgl_` names, a nesting cap, only the standard preprocessor
  directives, `#version 100` (or `300 es` on a WebGL 2 context) and only the
  `#extension` names WebGL exposes — then parses and links the shader with
  the Khronos [glslang](https://github.com/KhronosGroup/glslang) front end
  as GLSL ES 1.00 or 3.00. For ES 1.00 the Appendix A limits apply (no
  `while` loops, inductive `for` loops only, constant-index-expressions for
  uniform, varying, sampler and attribute arrays). A rejected shader never
  reaches the driver: it is given a one-line `#error` source instead, so
  `COMPILE_STATUS` is false and `getShaderInfoLog()` returns the validator's
  message; `getShaderSource()` returns the page's original text. glslang is
  therefore a build requirement for WebGL; without it WebGL is not built
  rather than built unvalidated.
- **Buffer ranges are checked by the browser.** Every draw call is checked
  before it reaches the driver: each vertex attribute array the program
  reads must cover the vertices and instances drawn (instance divisors
  included, in WebGL 1 through `ANGLE_instanced_arrays` as well), the
  largest index in an element range is read from a copy of the index
  buffer, and in WebGL 2 every uniform block must have a bound buffer range
  at least its data size. A draw that would read past a buffer is an
  `INVALID_OPERATION` and is not issued.
- **Robust buffer access contexts.** In addition, the GL context is created
  with `EGL_EXT_create_context_robustness` (Linux and the BSDs) or
  `WGL_ARB_create_context_robustness` (Windows) and lose-context-on-reset,
  so out-of-range reads return defined values and a reset takes down only
  that context. On Linux and the BSDs a driver without it gets no WebGL
  context. On Windows, where AMD's OpenGL driver lacks the extension, an
  ordinary context is used and the checks above are what bound buffer
  reads; a GPU reset is then not confined to the WebGL context. macOS's CGL
  has no robustness extension.
- **No JS-heap out-of-bounds.** Every pixel-transfer entry point validates
  the caller-supplied `ArrayBufferView` against the exact number of bytes
  GL will touch — computed from width/height/depth, format, type and the
  live `PACK`/`UNPACK` pixel-store state. That byte count is computed with
  overflow-checked arithmetic, so hostile width/height/depth or `pixelStorei`
  (`UNPACK_ROW_LENGTH`, `UNPACK_IMAGE_HEIGHT`, `SKIP_*`) values can't wrap it
  to a small number that slips past the bounds check. `readPixels` into an
  undersized buffer is skipped (no write past the view); `texImage2D` /
  `texImage3D` / `texSubImage*` from an undersized array are skipped (no read
  past it).
- **No uninitialized GPU memory disclosure.** Every allocation a page can
  read back starts as zeros, so it cannot see stale VRAM: `bufferData(size)`
  and `texImage2D/3D(…, null)` upload zero-filled storage; `texStorage2D/3D`
  uploads zeros to every level, face and layer (in bands of at most 4 MiB)
  and accepts only the sized and compressed formats it knows how to fill;
  `renderbufferStorage[Multisample]` clears the new storage through a
  scratch framebuffer; and the canvas drawing buffer has its colour, depth
  and stencil cleared whenever it is allocated or resized.
- **Buffer-range checks on raw copies.** `bufferSubData` (JS → buffer) and
  `getBufferSubData` (buffer → JS, via `glMapBufferRange`) validate that
  `srcByteOffset + length` lands inside the bound buffer — offset
  non-negative, the add overflow-checked in 64-bit, the sum within
  `GL_BUFFER_SIZE` — before touching GPU memory. This doesn't trust the
  driver to reject an out-of-range map/copy, so a hostile offset can't be
  used to write past or read stale bytes beyond the buffer.
- **Index-buffer bounds on indexed draws.** `drawElements`,
  `drawElementsInstanced` and `drawRangeElements` validate that the index
  range (`offset + count × index-size`) fits inside the currently bound
  `ELEMENT_ARRAY_BUFFER`, with the multiply/add done in overflow-checked
  64-bit. A draw with no index buffer bound, a bad index type, or a range
  that runs off the end of the buffer is dropped instead of handed to the
  driver, so a hostile `count`/`offset` can't make the GPU read indices
  past the buffer. Negative `first`/`count`/`instanceCount` on any draw
  call are likewise rejected.
- **Texture sources.** Texture uploads from a DOM source (`<img>`,
  `<canvas>`, `ImageBitmap`) go through the surface path the 2D canvas
  `drawImage` uses, so decoding is the engine's usual one (Wuffs, libavif)
  and a source that is not origin-clean is refused with a `SecurityError`.
  A WebGL canvas is therefore never tainted. Raw typed-array uploads stay
  bounds-checked as above.
- **Shader source size cap.** `shaderSource` is bounded (4 MiB) and the
  source is handed to the driver with an explicit length, so an embedded
  NUL can't truncate the shader and a pathologically large string can't be
  used to exhaust memory in the driver's compiler.
- **Allocation + context caps.** A single buffer/texture upload is capped
  (1 GiB) and the number of simultaneous live contexts is capped (32) to
  bound memory and FD/context exhaustion. Canvas dimensions are clamped.
- **Reduced fingerprinting.** `getParameter(VENDOR)` and `RENDERER` return
  fixed compatibility strings, while `VERSION` and
  `SHADING_LANGUAGE_VERSION` return WebGL-style versions rather than the raw
  driver strings. `WEBGL_debug_renderer_info` is present for compatibility,
  but its unmasked vendor and renderer values are fixed strings rather than
  the host GPU's identity. The numeric capability limits that fingerprinting
  libraries probe — `MAX_TEXTURE_SIZE`,
  `MAX_VIEWPORT_DIMS`, `MAX_VERTEX_ATTRIBS`, the uniform/varying/texture-unit
  maxima, the WebGL 2 block and component limits, and the aliased
  line/point-size ranges — are clamped to fixed common values
  (`wgl_param_cap` in `src/webgl.c`), so high-end and exotic GPUs report the
  same numbers as the mainstream. `COMPRESSED_TEXTURE_FORMATS` lists only the
  formats of the compressed-texture extensions the page has enabled. Clamping
  only ever lowers a reported limit, so it never makes the
  driver promise capacity it does not have.
- **No JS-controlled stack write in `getParameter`.** An unrecognised
  `pname` falls through to `glGetIntegerv` into a fixed stack buffer; that
  buffer is sized with generous headroom (32 ints) so a caller-supplied
  enum that returns a multi-valued result can't write past it.
- **No raw GPU handles in JS.** Sync objects (`GLsync` pointers) are handed
  out as opaque session ids via an indirection table, not pointer values.

What is **not** done: there is no full GL command-stream validator (the
ANGLE model). Index-buffer bounds are now checked and shaders are
validated before compilation, and vertex, index and uniform buffer ranges
are checked at every draw (see above). Resources are initialised eagerly,
when they are allocated, rather than lazily on first use as ANGLE does.
Users who do not want pages to reach the native GL stack can disable WebGL
globally in Settings.

## Limitations

- `texImage2D` from a DOM source decodes through the same surface path the
  2D canvas `drawImage` uses, so cross-origin restrictions and decode
  support match the rest of the engine.
- Context loss is only ever simulated: `webglcontextlost` /
  `webglcontextrestored` fire through `WEBGL_lose_context`, never because the
  driver lost the context.
- Antialiasing depends on the driver advertising `GL_MAX_SAMPLES > 1`.
- On Linux and the BSDs WebGL needs a driver that can create a
  robust-access context (`EGL_EXT_create_context_robustness`); Mesa's
  drivers, llvmpipe included, do. Windows uses an ordinary context when the
  driver lacks `WGL_ARB_create_context_robustness`, and macOS's CGL has no
  such extension; buffer ranges are checked by the browser on all three.
- WebGL 2 is not available on macOS (see [How it works](#how-it-works)).

These are deliberate: the goal is a small, readable bridge that runs the
common WebGL content, not a full conformance suite.

## Trying it

```sh
meson compile -C builddir
./builddir/src/gtk/northstar --headless --dump=png:out.png your-webgl-page.html
```

A minimal smoke test is a hello-triangle: compile a vertex and a fragment
shader, link a program, upload three vertices and call
`drawArrays(gl.TRIANGLES, 0, 3)`.
