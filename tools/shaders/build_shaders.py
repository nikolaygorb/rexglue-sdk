#!/usr/bin/env python3
"""Regenerates the built-in shader bytecode headers from their sources.

The headers are checked in, so building the SDK never needs a shader toolchain.
Run this (or the rexglue_shaders CMake target, REXGLUE_BUILD_SHADERS=ON) after
changing a shader source, then commit the sources and headers together.

Usage:
  python build_shaders.py [--tools-dir DIR] [--fxc PATH] [--clang-format PATH]
                          [--only dxbc|spirv] [--check] [shader dirs...]

Sources and outputs (rexglue layout):
  <dir>/<name>.<stage>.xesl|.hlsl  -> <dir>/bytecode/d3d12_5_1/<name>_<stage>.h (FXC)
  <dir>/<name>.<stage>.xesl|.glsl  -> <dir>/vulkan_spirv/<name>_<stage>.h
  <dir>/<name>.<stage>.spvasm      -> <dir>/vulkan_spirv/<name>_<stage>.h (spirv-as)
  where <stage> is vs, hs, ds, gs, ps or cs.

The compiler flags match xenia-canary's `xb buildshaders` (canary 0b2ffa314),
which the checked-in headers came from:
  FXC:   /D SHADING_LANGUAGE_HLSL_XE=1 /T <stage>_5_1 /Vn <identifier>
  SPIR-V: glslangValidator -V, spirv-opt -O, spirv-remap --do-everything.
  Without spirv-remap (newer Vulkan SDKs), spirv-opt -O --canonicalize-ids is
  used instead, as canary does now.

SPIR-V tools are taken from --tools-dir, else from $VULKAN_SDK/bin (preferred,
same as canary), else from PATH. FXC comes from --fxc, $FXC_PATH or the newest
Windows 10/11 SDK. Generated headers are formatted with clang-format using the
repository's .clang-format, like the checked-in ones.

--check writes nothing into the tree: it generates into a temporary directory
and lists the headers whose array contents differ from the checked-in ones.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEFAULT_SHADER_DIRS = ['src/graphics/shaders', 'src/ui/shaders']

DXBC_DIR = os.path.join('bytecode', 'd3d12_5_1')
SPIRV_DIR = 'vulkan_spirv'
STAGES = ('vs', 'hs', 'ds', 'gs', 'ps', 'cs')
SPIRV_STAGES = {'vs': 'vert', 'hs': 'tesc', 'ds': 'tese', 'gs': 'geom', 'ps': 'frag',
                'cs': 'comp'}

# Sources that must not get a generated header.
SKIP_SOURCES = set()

# #version and extensions must come before everything else in a GLSL file, so
# XeSL sources are compiled through this wrapper from stdin.
SPIRV_XESL_WRAPPER = ('#version 460\n'
                      '#extension GL_EXT_control_flow_attributes : require\n'
                      '#extension GL_EXT_samplerless_texture_functions : require\n'
                      '#extension GL_GOOGLE_include_directive : require\n'
                      '#include "%s"\n')

NUMBER = re.compile(r'0x[0-9A-Fa-f]+|[0-9]+')


def identifier_and_stage(src_name):
    base, ext = os.path.splitext(src_name)
    identifier = base.replace('.', '_')
    stage = identifier[-2:]
    if len(identifier) < 3 or identifier[-3] != '_' or stage not in STAGES:
        return None, None, ext
    return identifier, stage, ext


def find_fxc(explicit):
    if explicit:
        return explicit
    if os.environ.get('FXC_PATH'):
        return os.environ['FXC_PATH']
    program_files = os.environ.get('ProgramFiles(x86)') or os.environ.get('ProgramFiles')
    if not program_files:
        return None
    arch = 'arm64' if os.environ.get('PROCESSOR_ARCHITECTURE') == 'ARM64' else 'x64'
    paths = glob.glob(os.path.join(program_files, 'Windows Kits', '10', 'bin', '*', arch,
                                   'fxc.exe'))

    def version_key(path):
        version = os.path.basename(os.path.dirname(os.path.dirname(path)))
        return [int(p) if p.isdigit() else 0 for p in version.split('.')]

    paths.sort(key=version_key)
    return paths[-1] if paths else None


def find_tool(name, tools_dir):
    exe = name + ('.exe' if sys.platform == 'win32' else '')
    dirs = []
    if tools_dir:
        dirs.append(tools_dir)
    if os.environ.get('VULKAN_SDK'):
        dirs.append(os.path.join(os.environ['VULKAN_SDK'], 'bin'))
        dirs.append(os.path.join(os.environ['VULKAN_SDK'], 'Bin'))
    for d in dirs:
        path = os.path.join(d, exe)
        if os.path.isfile(path):
            return path
    return shutil.which(name)


def run(args, input_text=None):
    result = subprocess.run(args, input=input_text, text=input_text is not None,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        err = result.stderr if isinstance(result.stderr, str) else result.stderr.decode(
            'utf-8', 'replace')
        raise RuntimeError('%s failed:\n%s' % (os.path.basename(args[0]), err))


def write_spirv_header(out_path, identifier, comment_first_line, comment_body, spv_path):
    with open(spv_path, 'rb') as f:
        data = f.read()
    if len(data) % 4:
        raise RuntimeError('SPIR-V binary is misaligned: ' + spv_path)
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    with open(out_path, 'w', newline='\n') as out:
        out.write(comment_first_line + '\n#if 0\n')
        out.write(comment_body)
        if comment_body and not comment_body.endswith('\n'):
            out.write('\n')
        out.write('#endif\n\nconst uint32_t %s[] = {' % identifier)
        for i, word in enumerate(words):
            out.write('\n    ' if i % 6 == 0 else ' ')
            out.write('0x%08X,' % word)
        out.write('\n};\n')


def build_dxbc(src_path, identifier, stage, out_path, fxc):
    args = [fxc] if sys.platform == 'win32' else ['wine', fxc]
    args += ['/D', 'SHADING_LANGUAGE_HLSL_XE=1', '/Fh', out_path, '/T', stage + '_5_1',
             '/Vn', identifier, '/nologo', src_path]
    run(args)


def build_spirv(src_path, identifier, stage, out_path, tools, tmp_dir):
    src_name = os.path.basename(src_path)
    src_dir = os.path.dirname(src_path)
    base = os.path.join(tmp_dir, identifier)
    if src_name.endswith('.spvasm'):
        spv = base + '.spv'
        run([tools['spirv-as'], '--target-env', 'spv1.0', '-o', spv, src_path])
        with open(src_path, encoding='utf-8') as f:
            source_text = f.read()
        write_spirv_header(out_path, identifier,
                           '// Generated with `spirv-as` from %s.' % src_name, source_text, spv)
        return
    is_xesl = src_name.endswith('.xesl')
    glslang_spv = base + '.glslang.spv'
    args = [tools['glslangValidator'], '--stdin' if is_xesl else src_path,
            '-DSHADING_LANGUAGE_GLSL_XE=1', '-S', SPIRV_STAGES[stage], '-o', glslang_spv, '-V']
    if is_xesl:
        args.append('-I' + src_dir)
    run(args, (SPIRV_XESL_WRAPPER % src_name) if is_xesl else None)
    spv = base + '.spv'
    if tools.get('spirv-remap'):
        run([tools['spirv-opt'], '-O', glslang_spv, '-o', spv])
        # spirv-remap writes <output dir>/<input file name>.
        remap_dir = os.path.join(tmp_dir, 'remap')
        os.makedirs(remap_dir, exist_ok=True)
        run([tools['spirv-remap'], '--do-everything', '-i', spv, '-o', remap_dir])
        spv = os.path.join(remap_dir, os.path.basename(spv))
    else:
        # Newer Vulkan SDKs no longer ship spirv-remap; canary itself now uses
        # spirv-opt's ID canonicalization instead (since f2ac39cfd).
        run([tools['spirv-opt'], '-O', '--canonicalize-ids', glslang_spv, '-o', spv])
    dis = base + '.txt'
    run([tools['spirv-dis'], '-o', dis, spv])
    with open(dis, encoding='utf-8') as f:
        disassembly = f.read()
    write_spirv_header(out_path, identifier, '// Generated with `xb buildshaders`.',
                       disassembly, spv)


def array_contents(path):
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8', 'replace')
    i = text.rfind('#endif')
    body = text[i:] if i >= 0 else text
    return [int(n, 0) for n in NUMBER.findall(body[body.find('{'):])]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('dirs', nargs='*', help='shader source directories')
    parser.add_argument('--tools-dir', help='directory with the SPIR-V tools')
    parser.add_argument('--fxc', help='path to fxc.exe')
    parser.add_argument('--clang-format', help='path to clang-format')
    parser.add_argument('--only', choices=('dxbc', 'spirv'))
    parser.add_argument('--check', action='store_true',
                        help='compare with the checked-in headers instead of writing')
    args = parser.parse_args()

    dirs = [os.path.join(REPO_ROOT, d) for d in (args.dirs or DEFAULT_SHADER_DIRS)]
    do_dxbc = args.only != 'spirv'
    do_spirv = args.only != 'dxbc'

    fxc = find_fxc(args.fxc) if do_dxbc else None
    if do_dxbc and not fxc:
        if args.only == 'dxbc' or sys.platform == 'win32':
            print('ERROR: fxc.exe not found (set FXC_PATH or install the Windows SDK)')
            return 1
        print('Skipping DXBC: fxc is only available on Windows')
        do_dxbc = False
    tools = {}
    if do_spirv:
        for name in ('glslangValidator', 'spirv-opt', 'spirv-dis', 'spirv-as'):
            tools[name] = find_tool(name, args.tools_dir)
            if not tools[name]:
                print('ERROR: %s not found (--tools-dir, VULKAN_SDK or PATH)' % name)
                return 1
        tools['spirv-remap'] = find_tool('spirv-remap', args.tools_dir)
        if not tools['spirv-remap']:
            print('NOTE: spirv-remap not found, using spirv-opt --canonicalize-ids instead '
                  '(SPIR-V will differ from the headers built with spirv-remap)')
    clang_format = args.clang_format or shutil.which('clang-format')
    if not clang_format:
        print('WARNING: clang-format not found, headers are left unformatted')

    tmp_root = tempfile.mkdtemp(prefix='rexglue_shaders_')
    failures = []
    differences = []
    count = 0
    try:
        for shader_dir in dirs:
            for src_name in sorted(os.listdir(shader_dir)):
                if src_name in SKIP_SOURCES:
                    continue
                identifier, stage, ext = identifier_and_stage(src_name)
                if identifier is None:
                    continue
                src_path = os.path.join(shader_dir, src_name)
                jobs = []
                if do_dxbc and ext in ('.xesl', '.hlsl'):
                    jobs.append((DXBC_DIR, 'dxbc'))
                if do_spirv and ext in ('.xesl', '.glsl', '.spvasm'):
                    jobs.append((SPIRV_DIR, 'spirv'))
                for out_subdir, kind in jobs:
                    final_path = os.path.join(shader_dir, out_subdir, identifier + '.h')
                    work_dir = os.path.join(tmp_root, kind)
                    os.makedirs(work_dir, exist_ok=True)
                    out_path = os.path.join(work_dir, identifier + '.h')
                    print('- %s > %s' % (os.path.relpath(src_path, REPO_ROOT), kind), flush=True)
                    try:
                        if kind == 'dxbc':
                            build_dxbc(src_path, identifier, stage, out_path, fxc)
                        else:
                            build_spirv(src_path, identifier, stage, out_path, tools, work_dir)
                        if clang_format:
                            run([clang_format, '-i',
                                 '--style=file:' + os.path.join(REPO_ROOT, '.clang-format'),
                                 out_path])
                    except RuntimeError as e:
                        failures.append(src_name)
                        print('ERROR: %s' % e)
                        continue
                    count += 1
                    if args.check:
                        if (not os.path.exists(final_path) or
                                array_contents(final_path) != array_contents(out_path)):
                            differences.append(os.path.relpath(final_path, REPO_ROOT))
                    else:
                        os.makedirs(os.path.dirname(final_path), exist_ok=True)
                        with open(out_path, 'rb') as f:
                            data = f.read().replace(b'\r\n', b'\n')
                        with open(final_path, 'wb') as f:
                            f.write(data)
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)

    print('%d headers %s, %d failures' % (count, 'checked' if args.check else 'written',
                                          len(failures)))
    if args.check:
        print('%d differ from the checked-in headers:' % len(differences))
        for path in differences:
            print('  ' + path)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
