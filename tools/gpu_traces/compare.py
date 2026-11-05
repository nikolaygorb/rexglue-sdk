#!/usr/bin/env python3
"""Replays every GPU frame trace in a folder and compares the frames with references.

See docs/gpu-traces.md.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

RESULTS = {0: 'ok', 1: 'DIFFERENT', 2: 'NOT DRAWN'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=pathlib.Path, help='game executable built with ReXGlue')
    parser.add_argument('traces', type=pathlib.Path, help='folder of .xtr files')
    parser.add_argument('--label', default='reference',
                        help='reference name, <trace>.<label>.png (one per backend and GPU)')
    parser.add_argument('--update', action='store_true',
                        help='save the frames as the references instead of comparing')
    parser.add_argument('--timeout', type=float, default=120, help='seconds per trace')
    parser.add_argument('game_args', nargs=argparse.REMAINDER,
                        help='after --: arguments for the game')
    args = parser.parse_args()
    game_args = args.game_args[1:] if args.game_args[:1] == ['--'] else args.game_args

    traces = sorted(args.traces.glob('*.xtr'))
    if not traces:
        print(f'no .xtr files in {args.traces}')
        return 2
    failed = 0
    for trace in traces:
        reference = trace.with_suffix(f'.{args.label}.png')
        output = trace.with_suffix(f'.{args.label}.last.png')
        command = [str(args.executable), *game_args, f'--trace_replay={trace}',
                   f'--trace_replay_output={output}']
        if not args.update:
            if not reference.exists():
                print(f'{trace.name}: no reference {reference.name}, run with --update')
                failed += 1
                continue
            command.append(f'--trace_replay_reference={reference}')
        try:
            code = subprocess.run(command, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            code = 2
            print(f'{trace.name}: timed out')
        if args.update and code == 0:
            shutil.copyfile(output, reference)
        print(f'{trace.name}: {RESULTS.get(code, f"exit code {code}")}')
        failed += code != 0
    print(f'{len(traces) - failed} of {len(traces)} passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
