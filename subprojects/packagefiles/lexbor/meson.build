# Native replacement for lexbor's CMake build, as Northstar uses it: one
# static library with every module, plus the port for the host OS.
project('lexbor', 'c', version: '3.0.1', license: 'Apache-2.0',
        meson_version: '>=1.0')

port = host_machine.system() == 'windows' ? 'windows_nt' : 'posix'

# lexbor's CMake globs its sources; do the same so a revision bump needs no
# file list update. Every module except ports/, then the host OS port.
list_sources = '''
import pathlib, sys
src = pathlib.Path(sys.argv[1])
lex = src / "lexbor"
port = lex / "ports" / sys.argv[2] / "lexbor"
files = [p for p in sorted(lex.rglob("*.c"))
         if p.relative_to(lex).parts[0] != "ports"]
files += sorted(port.rglob("*.c"))
sys.stdout.write("\n".join(p.relative_to(src.parent).as_posix() for p in files))
'''
python = find_program('python3', 'python')
srcs = run_command(python, '-c', list_sources,
                   meson.current_source_dir() / 'source', port,
                   check: true).stdout().strip().split('\n')

c_args = ['-DLEXBOR_STATIC', '-w']
if host_machine.system() == 'linux'
  c_args += '-D_POSIX_C_SOURCE=199309L'
endif

inc = include_directories('source')
lexbor_lib = static_library('lexbor_static', srcs,
  c_args: c_args,
  include_directories: inc,
  pic: true,
  install: false)

lexbor_dep = declare_dependency(link_with: lexbor_lib,
  include_directories: include_directories('source', is_system: true),
  compile_args: '-DLEXBOR_STATIC')
meson.override_dependency('lexbor', lexbor_dep)
