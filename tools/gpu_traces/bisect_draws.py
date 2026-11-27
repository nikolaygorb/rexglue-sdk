#!/usr/bin/env python3
"""Finds the first draw of a GPU trace after which two renderers draw different frames.

Replays the trace with trace_replay_draw_limit = N on the reference renderer and on the
tested one, compares the two frames, and searches for the smallest N where they differ
(doubling, then halving). The tested replay of that N logs the draw: its shaders, render
targets and textures ("GPU trace: draw limit"). See docs/gpu-traces.md.

  python tools/gpu_traces/bisect_draws.py game.exe trace.xtr -- --gpu_plugin=native
"""
import argparse
import pathlib
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('executable', type=pathlib.Path, help='game executable built with ReXGlue')
    parser.add_argument('trace', type=pathlib.Path, help='the .xtr file')
    parser.add_argument('--reference', default='--gpu_plugin=xenos',
                        help='arguments of the reference renderer (default --gpu_plugin=xenos)')
    parser.add_argument('--tolerance', type=int, default=2,
                        help='trace_replay_tolerance (default 2)')
    parser.add_argument('--max-percent', type=float, default=0.05,
                        help='trace_replay_max_different_percent (default 0.05)')
    parser.add_argument('--timeout', type=float, default=120, help='seconds per replay')
    parser.add_argument('--scale', type=int, default=1,
                        help='resolution_scale of both replays (default 1, whatever the '
                             'config of the game has: at a scale the renderers save frames of '
                             'different sizes)')
    parser.add_argument('--at', type=int, metavar='N',
                        help='no search: replay both with the first N draws and compare')
    parser.add_argument('--keep-edram', action='store_true',
                        help='restore the EDRAM of the trace (by default both start with it '
                             'empty: a renderer without RestoreEdramSnapshot, such as native, '
                             'would differ from the first draw)')
    # The tested renderer's arguments after "--" (see compare.py).
    argv = sys.argv[1:]
    tested_args = []
    if '--' in argv:
        split = argv.index('--')
        tested_args = argv[split + 1:]
        argv = argv[:split]
    args, unknown = parser.parse_known_args(argv)
    tested_args = unknown + tested_args
    if not args.executable.is_file():
        print(f'no executable {args.executable}: give its path (relative to {pathlib.Path.cwd()})')
        return 2
    if not args.trace.is_file():
        print(f'no trace {args.trace}')
        return 2

    reference_png = args.trace.with_suffix('.bisect.reference.png')
    tested_png = args.trace.with_suffix('.bisect.tested.png')

    def replay(extra, limit, output, reference=None):
        # Before the renderer's own arguments, which may set another scale.
        extra = [f'--resolution_scale={args.scale}', *extra]
        if not args.keep_edram:
            extra = [*extra, '--trace_replay_edram=false']
        command = [str(args.executable), *extra, f'--trace_replay={args.trace}',
                   f'--trace_replay_output={output}', f'--trace_replay_draw_limit={limit}',
                   f'--trace_replay_tolerance={args.tolerance}',
                   f'--trace_replay_max_different_percent={args.max_percent}']
        if reference:
            command.append(f'--trace_replay_reference={reference}')
        try:
            return subprocess.run(command, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            return 2

    def frame_draws():
        """The frame's draw count from the newest game log ("GPU trace: N draws in the
        frame"), or None."""
        logs = sorted((args.executable.parent / 'logs').glob('*.log'),
                      key=lambda path: path.stat().st_mtime)
        if not logs:
            return None
        found = re.findall(r'GPU trace: (\d+) draws in the frame',
                           logs[-1].read_text(encoding='utf-8', errors='replace'))
        return int(found[-1]) if found else None

    tried = {}

    def differs(limit):
        """True if the frames of the first `limit` draws differ; None if not drawn."""
        if limit in tried:
            return tried[limit]
        code = replay(args.reference.split(), limit, reference_png)
        if code != 0:
            print(f'draws {limit}: the reference replay failed (exit code {code})')
            result = None
        else:
            code = replay(tested_args, limit, tested_png, reference_png)
            result = None if code == 2 else code == 1
            print(f'draws {limit}: {"DIFFERENT" if result else "same" if result is not None else "NOT DRAWN"}')
        tried[limit] = result
        return result

    def show_frames():
        print('the game log of the last replay names the last draw\'s shaders and textures '
              '("GPU trace: draw limit"); the frames are')
        print(f'  {reference_png}')
        print(f'  {tested_png} (and its .diff.png)')

    if args.at is not None:
        result = differs(args.at)
        if result is None:
            return 2
        show_frames()
        return 1 if result else 0

    if differs(0):
        print('the frames differ with no draw at all: copies, clears' +
              (' or the EDRAM restore (try without --keep-edram)' if args.keep_edram else ''))
        return 1
    # Doubling up to the first difference, not past the frame's draws (from the
    # game log of the replay above; without it, up to 2^20).
    total = frame_draws()
    if total is not None:
        print(f'the frame has {total} draws')
    last = total if total is not None else 1 << 20
    low, high = 0, min(1, last)
    while True:
        if high <= low:
            print('no difference with all draws: the frames match')
            return 0
        result = differs(high)
        if result is None:
            return 2
        if result:
            break
        low = high
        high = min(high * 2, last)
    while high - low > 1:
        middle = (low + high) // 2
        result = differs(middle)
        if result is None:
            return 2
        if result:
            high = middle
        else:
            low = middle
    # Both replays of `high` again, so the frames on disk and the last log are
    # of the differing draw (the search's last step may have been another N).
    if high != list(tried)[-1]:
        del tried[high]
        differs(high)
    print(f'first differing draw: #{high - 1} (frames match up to {low} draws, differ at {high})')
    show_frames()
    return 1


if __name__ == '__main__':
    sys.exit(main())
