#!/usr/bin/env python3
"""Merge V8 builtins profiles from many processes into one PGO profile.

A Node built with `configure --v8-enable-builtins-profiling` counts how often
each basic block of V8's builtins runs. Run a workload with

  NODE_V8_FLAGS=--turbo-profiling-output=/tmp/pgo/v8.%p.log

and every process (and every isolate in it) writes its counts to its own file
when it exits. This script sums the counts of all those files per builtin and
block, checks that they all come from the same build (V8 records a hash of
each builtin's graph), and turns the sum into the branch hints and block counts
that mksnapshot reads, using V8's own deps/v8/tools/builtins-pgo/get_hints.py.
Build with `configure --v8-builtins-profile=<output>` to use it.

Usage:
  tools/v8_builtins_pgo.py [--min N] [--ratio R] [--merged FILE]
                           -o OUTPUT INPUT...

Each INPUT is a log file or a directory of them (every file in it is read).
"""

import argparse
import collections
import os
import subprocess
import sys
import tempfile

BLOCK_COUNT_MARKER = 'block'
BRANCH_MARKER = 'block_hint'
BUILTIN_HASH_MARKER = 'builtin_hash'

GET_HINTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                         'deps', 'v8', 'tools', 'builtins-pgo', 'get_hints.py')


def input_files(paths):
  for path in paths:
    if os.path.isdir(path):
      for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isfile(full):
          yield full
    else:
      yield path


def merge(files):
  counts = collections.defaultdict(int)
  branches = {}
  hashes = {}
  per_file_builtins = []
  for path in files:
    builtins = set()
    with open(path) as f:
      for line_no, line in enumerate(f, 1):
        fields = line.rstrip('\n').split('\t')
        marker = fields[0]
        if marker == BLOCK_COUNT_MARKER:
          name, block, count = fields[1], int(fields[2]), int(fields[3])
          counts[(name, block)] += count
          builtins.add(name)
        elif marker == BRANCH_MARKER:
          branches[(fields[1], int(fields[2]), int(fields[3]))] = None
        elif marker == BUILTIN_HASH_MARKER:
          name, value = fields[1], int(fields[2])
          old = hashes.setdefault(name, (value, path))
          if old[0] != value:
            sys.exit(f'{path}:{line_no}: builtin {name} has hash {value}, '
                     f'but {old[1]} has {old[0]}: the files come from '
                     'different builds')
        elif line.strip():
          sys.exit(f'{path}:{line_no}: unexpected line: {line.strip()[:80]}')
    per_file_builtins.append(len(builtins))
  return counts, branches, hashes, per_file_builtins


def write_merged(path, counts, branches, hashes):
  with open(path, 'w') as f:
    for (name, block), count in sorted(counts.items()):
      f.write(f'{BLOCK_COUNT_MARKER}\t{name}\t{block}\t{count}\n')
    for (name, true_id, false_id) in branches:
      f.write(f'{BRANCH_MARKER}\t{name}\t{true_id}\t{false_id}\n')
    for name, (value, _) in sorted(hashes.items()):
      f.write(f'{BUILTIN_HASH_MARKER}\t{name}\t{value}\n')


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('-o', '--output', required=True,
                      help='profile to write, for --v8-builtins-profile')
  parser.add_argument('--merged',
                      help='also keep the summed raw counts in this file')
  parser.add_argument('--min', type=int, default=1000,
                      help='get_hints.py: minimum count of a hinted branch '
                      'destination (default 1000)')
  parser.add_argument('--ratio', type=int, default=40,
                      help='get_hints.py: how many times more often one '
                      'destination must run than the other (default 40)')
  parser.add_argument('inputs', nargs='+', help='log files or directories')
  args = parser.parse_args()

  files = list(input_files(args.inputs))
  if not files:
    sys.exit('no input files')
  counts, branches, hashes, per_file = merge(files)
  builtins = {name for name, _ in counts}
  per_file.sort()
  print(f'merged {len(files)} files: {len(builtins)} builtins with counts '
        f'({per_file[0]} to {per_file[-1]} per file, median '
        f'{per_file[len(per_file) // 2]}), {len(counts)} blocks, '
        f'{len(branches)} branches, '
        f'{sum(counts.values()) / 1e9:.2f} G block executions')

  if args.merged:
    merged = args.merged
    write_merged(merged, counts, branches, hashes)
    run_hints(args, merged)
  else:
    with tempfile.TemporaryDirectory() as tmp:
      merged = os.path.join(tmp, 'merged.log')
      write_merged(merged, counts, branches, hashes)
      run_hints(args, merged)

  hints = 0
  with open(args.output) as f:
    for line in f:
      if line.startswith(BRANCH_MARKER + ','):
        hints += 1
  print(f'wrote {args.output}: {hints} branch hints for {len(hashes)} builtins')


def run_hints(args, merged):
  subprocess.run([sys.executable, GET_HINTS, '--min', str(args.min),
                  '--ratio', str(args.ratio), merged, args.output], check=True)


if __name__ == '__main__':
  main()
