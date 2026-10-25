# Building and running in Visual Studio Code

The repository carries a VS Code workspace in `.vscode/` that builds, runs
and debugs Northstar Web Browser on Linux, macOS and Windows. It drives the
same Meson build described in [building.md](building.md); VS Code adds
buttons and a debugger on top, not a second build system.

## 1. Install the build dependencies

VS Code does not install the compiler or libraries. Follow the section for
your platform in [building.md](building.md) first:

- **Linux:** [Linux dependencies](building.md#linux-dependencies), plus a
  debugger: `sudo apt install gdb` (Fedora `sudo dnf install gdb`).
- **macOS:** [macOS dependencies](building.md#macos-dependencies) and the
  Xcode command-line tools (`xcode-select --install`), which provide lldb.
  With Homebrew, export the `PKG_CONFIG_PATH` shown there in the shell you
  start VS Code from (`code .`), so the build tasks inherit it.
- **Windows:** [Windows dependencies](building.md#windows-dependencies).
  MSYS2 must be installed at `C:\msys64`. Add gdb to the same MSYS2
  environment the build uses, from an MSYS2 shell:

  ```sh
  pacman -S mingw-w64-ucrt-x86_64-gdb     # UCRT64 (the usual setup)
  pacman -S mingw-w64-x86_64-gdb          # MINGW64, where only that one has GTK 4
  ```

  The Windows tasks run in PowerShell and call MSYS2 themselves, so VS Code
  does not need to be started from an MSYS2 shell.

Check that the plain command-line build works once before going further;
the VS Code tasks fail the same way it does.

## 2. Open the folder and install the extensions

1. Open the repository folder (*File → Open Folder…*, or `code .` from the
   checkout).
2. Accept the prompt to install the recommended extensions, or install them
   from the Extensions view (`@recommended`):
   - **C/C++** (`ms-vscode.cpptools`) — IntelliSense and the gdb/lldb
     debugger. Required for debugging.
   - **Meson** (`mesonbuild.mesonbuild`) — syntax highlighting for
     `meson.build` files. Optional.

The workspace settings stop the Meson and CMake extensions from configuring
a build of their own or rewriting the settings. If VS Code offers to
configure a CMake project, decline: the `CMakeLists.txt` files under
`subprojects/` belong to dependencies. CMake Tools is listed as an unwanted
recommendation for that reason.

## 3. Build

Press `Ctrl+Shift+B` (`Cmd+Shift+B` on macOS) to run the default **Build**
task. The first build configures `builddir` and fetches the pinned
subprojects, so it takes a few minutes; later builds only recompile what
changed. Compiler errors and warnings appear in the *Problems* panel, and a
click jumps to the line.

All tasks are under *Terminal → Run Task…*:

| Task | What it does |
| --- | --- |
| **Build** | Configures `builddir` if needed and compiles it (the release build). |
| **Build (debug)** | Configures and compiles `builddir-debug` with `-Dbuildtype=debug -Db_lto=false`. |
| **Run** | Builds, then opens the browser on `about:start`. |
| **Stop** | Closes every running browser instance. |
| **Clean** | Deletes the compiled objects in `builddir`, keeping its configuration. |
| **Smoke test** | Builds, then runs the smoke check: the headless fixture comparison on Linux and macOS (`scripts/dev.sh smoke`), a launch-and-close check on Windows (`scripts/smoke-windows.ps1`). |

On Windows, **Build** and **Build (debug)** close a running `northstar.exe`
first, since a running copy locks the file the linker writes.

## 4. Run

Run the **Run** task. On Linux and macOS the browser runs in the task's
terminal, and its console output appears there; close the window or run
**Stop** to end it. On Windows it starts detached with the MSYS2 libraries
on its `PATH`, and the task finishes at once.

## 5. Debug

Open *Run and Debug* (`Ctrl+Shift+D`), pick a configuration and press `F5`:

- **Debug Northstar** builds `builddir-debug`, then starts the browser on
  `about:start` under gdb (lldb on macOS).
- **Debug Northstar (headless dump)** asks for a URL or file, default
  `data/fixtures/basics.html`, and runs `--headless --dump=text` on it,
  with no window. This is the quickest way to step through parsing, style
  and layout for one page.

Breakpoints, stepping, the call stack and variable inspection work as
usual. The debug configurations always use `builddir-debug`: the release
build in `builddir` is optimised and carries no debug information. The
debug build is a separate configuration, so its first build takes as long
as the first release build.

On Windows, each debug session first asks which MSYS2 environment the
browser was built in. Pick the one whose `bin` holds GTK 4 and the gdb
installed in step 1 — UCRT64 normally, MINGW64 on a machine where only that
environment has GTK 4. It supplies both the debugger and the DLLs the
browser loads.

## IntelliSense

IntelliSense reads `builddir/compile_commands.json`, which Meson writes when
it configures `builddir`. Until the first build, includes such as
`<gtk/gtk.h>` and `<quickjs.h>` show as unresolved; after it, go-to-
definition, hover and completion follow the exact flags of the real build.
After changing a `meson.build` or a Meson option, build once more so the
file is refreshed.

On Windows, `.vscode/c_cpp_properties.json` names UCRT64's `clang.exe` for
the compiler's own headers. If you build in MINGW64, change `compilerPath`
in the `Win32` configuration to `C:/msys64/mingw64/bin/clang.exe`.

## Troubleshooting

- **The build task fails at once** — run the same build from a terminal
  (`./scripts/dev.sh build`, or on Windows
  `powershell -File scripts\vs-build.ps1 Build`) to see the full output;
  the cause is almost always a missing dependency from step 1.
- **"MSYS2 was not found at C:\msys64"** — MSYS2 is installed elsewhere.
  Change `$msysRoot` in `scripts/vs-build.ps1` and the paths in
  `.vscode/launch.json` and `.vscode/c_cpp_properties.json`.
- **Windows link fails with "Permission denied"** — a browser started
  outside VS Code is still running. Run **Stop**, then build again.
- **Windows debug build fails loading `LLVMgold.dll`** — a `builddir-debug`
  configured by hand with LTO on. Delete `builddir-debug` and run
  **Build (debug)** again; the task turns LTO off.
- **gdb cannot be found on Windows** — the environment picked at the
  start of the debug session has no gdb; install it there (step 1) or pick
  the other environment.
- **Breakpoints stay hollow** — the session is running a release binary.
  Use the debug configurations, which build and run `builddir-debug`.

Edits to the shared files in `.vscode/` (`tasks.json`, `launch.json`,
`settings.json`, `extensions.json`, `c_cpp_properties.json`) are committed
like any other change; anything else VS Code writes there is ignored.
