#!/usr/bin/env python3
"""Build selected kernels, run cfg_bench, compare results, summarize timings.

Default: Java points-to, baseline and P1 full, source sizes 1/10/100.
Each chunk gets one warm-up and five consecutive measurements in cfg_bench.
No profiling, outer repetition loop, automatic cfg_bench rebuild or thread overrides.
"""
import argparse
from collections import defaultdict
import csv
from datetime import datetime
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
from zoneinfo import ZoneInfo

import labstate

# Edit build paths/flags here. Benchmark flags are in benchmark_command() below.
LAB = Path(__file__).resolve().parent
ROOT = LAB.parents[2]
CFG_DIR = Path('/home/hollow/repo/CFG_bench')
GRAPHBLAS = Path('/home/hollow/repo/GraphBLAS/build')
COMPILER = 'cc'
CFLAGS = ['-O2', '-std=c11', '-fPIC', '-shared']
LIB_DIRS = [ROOT/'build-exp-release/experimental', ROOT/'build/src', GRAPHBLAS]


def arguments():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', type=Path, default=LAB/'configs/java_points_to.csv')
    p.add_argument('--input-manifest', type=Path, default=labstate.LOCK,
                   help='pinned binary/data lock (default: one-shot timing, construction included)')
    p.add_argument('--variants', nargs='+', default=['baseline', 'p1_full'],
                   help='registered source names, e.g. baseline p1_full')
    p.add_argument('--allow-archived', action='store_true',
                   help='allow explicit selection of archived variants for reproduction')
    p.add_argument('--reference',
                   help='selected variant used for correctness and timing ratios; defaults to baseline if selected, otherwise the first variant')
    p.add_argument('--source-sizes', default='1,10,100', help='comma-separated sizes')
    p.add_argument('--chunks', type=int, default=30)
    p.add_argument('--rounds', type=int, default=5, help='consecutive measurements per chunk')
    p.add_argument('--hot', action=argparse.BooleanOptionalAction, default=True,
                   help='one unmeasured warm-up per chunk (default: enabled)')
    p.add_argument('--seed', type=int, default=2026)
    p.add_argument('--out', type=Path, help='new output directory; never overwritten')
    p.add_argument('--dry-run', action='store_true', help='print commands; create/run nothing')
    p.add_argument('--build-only', action='store_true', help='compile kernels without benchmarking')
    p.add_argument('--analyze', type=Path, help='check/summarize a saved run without rebuilding')
    return p.parse_args()


def command(cmd, cwd, out, name, env, dry_run):
    print(f'cwd: {cwd}\n{shlex.join(map(str, cmd))}', flush=True)
    if dry_run:
        return
    with (out/'commands.txt').open('a') as stream:
        stream.write(f'cwd: {cwd}\n{shlex.join(map(str, cmd))}\n\n')
    with (out/f'{name}.log').open('w') as log:
        subprocess.run(list(map(str, cmd)), cwd=cwd, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)


def build_command(source, library):
    return [COMPILER, *CFLAGS,
            '-I'+str(ROOT/'include'), '-I'+str(LAB/'include'), '-I'+str(ROOT/'src/utility'),
            '-I/usr/local/include/suitesparse', source, '-o', library,
            '-L'+str(ROOT/'build/src'), '-L'+str(GRAPHBLAS),
            '-Wl,-rpath,'+str(ROOT/'build/src'), '-Wl,-rpath,'+str(GRAPHBLAS),
            '-llagraph', '-lgraphblas']


def benchmark_command(binary, config, library, results, args):
    # ONE process per variant. cfg_bench owns the chunk and repetition loops.
    cmd = [binary,
           '-c', config,
           '-a', 'CFL_CFPQ_RSM',
           '--rsm-lib', library,
           '--experiment',
           '--source-sizes', args.source_sizes,
           '--chunks', str(args.chunks),
           '--seed', str(args.seed),
           '-r', str(args.rounds),
           '--results-file', results]
    if args.hot:
        cmd.append('--hot')
    return cmd


def config_rows(config):
    with config.open() as stream:
        rows = [r for r in csv.reader(stream) if r and not r[0].startswith('#')]
    if not rows or any(len(r) != 4 for r in rows):
        raise ValueError('config needs graph,grammar,expected-result,source-pool on each line')
    if len({(r[0], r[1]) for r in rows}) != len(rows):
        raise ValueError('duplicate graph/grammar in config')
    return rows


def read_results(path):
    groups = defaultdict(list)
    with path.open() as stream:
        for row in csv.DictReader(stream):
            key = (row['graph'], row['cnf'], int(row['source_size']), int(row['chunk']))
            groups[key].append(row)
    return groups


def reference_variant(variants, reference=None):
    if not variants or len(set(variants)) != len(variants):
        raise ValueError('--variants must contain distinct names')
    selected = reference or ('baseline' if 'baseline' in variants else variants[0])
    if selected not in variants:
        raise ValueError('--reference must be one of the selected variants')
    return selected


def check_correctness(results, expected, rounds, reference='baseline'):
    """Require complete samples and stable count/hash equality with the reference."""
    reference_variant(list(results), reference)
    for name, cases in results.items():
        if set(cases) != expected:
            raise ValueError(f'{name}: missing or unexpected cases')
        for key, rows in cases.items():
            if len(rows) != rounds:
                raise ValueError(f'{name}: {key}: expected {rounds} measurements, got {len(rows)}')
    for name, cases in results.items():
        for key, rows in cases.items():
            reference_row = results[reference][key][0]
            for row in rows:
                if row['algo_name'] != 'CFL_CFPQ_RSM' or int(row['result_hash']) == 0:
                    raise ValueError(f'{name}: {key}: wrong algorithm or missing result hash')
                if (row['result'], row['result_hash']) != (reference_row['result'], reference_row['result_hash']):
                    raise ValueError(f'{name}: {key}: result count/hash differs from {reference}')
                if not math.isfinite(float(row['time_s'])) or float(row['time_s']) <= 0:
                    raise ValueError(f'{name}: {key}: invalid time')
    print(f'PASS: every sample count/hash matches {reference} (not a general correctness proof).')


def analyze(out):
    settings = json.loads((out/'settings.json').read_text())
    rows = config_rows(out/'config.csv')
    expected = {(r[0], r[1], size, chunk) for r in rows
                for size in settings['source_sizes'] for chunk in range(settings['chunks'])}
    results = {name: read_results(out/f'{name}.csv') for name in settings['variants']}
    reference = reference_variant(settings['variants'], settings.get('reference'))
    check_correctness(results, expected, settings['rounds'], reference)
    medians = {name: {key: statistics.median(float(r['time_s']) for r in samples)
                      for key, samples in cases.items()} for name, cases in results.items()}
    summary = []
    print(f'Reference: {reference}')
    print('variant       graph           sources   median_ms  time/reference faster_chunks')
    for name, cases in medians.items():
        for graph, grammar, _, _ in rows:
            for size in settings['source_sizes']:
                keys = sorted(k for k in cases if k[:3] == (graph, grammar, size))
                ratios = [cases[k]/medians[reference][k] for k in keys]
                ms = 1000*statistics.median(cases[k] for k in keys)
                ratio = math.exp(statistics.mean(math.log(r) for r in ratios))
                wins = sum(r < 1 for r in ratios)
                summary.append(dict(variant=name, reference=reference, graph=graph, cnf=grammar, source_size=size,
                                    median_ms=ms, geometric_time_ratio=ratio, faster_chunks=wins,
                                    chunks=len(keys)))
                print(f'{name:13} {Path(graph).stem:15} {size:7} {ms:11.3f} {ratio:15.3f} {wins:5}/{len(keys)}')
    with (out/'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print('Ratios below 1 are faster. Medians use consecutive rounds, then chunks.')


def main():
    args = arguments()
    if args.analyze:
        analyze(args.analyze.resolve())
        return
    sizes = list(map(int, args.source_sizes.split(',')))
    if min(sizes) <= 0 or args.rounds <= 0 or args.chunks <= 0 or len(set(sizes)) != len(sizes):
        raise ValueError('sizes, rounds and chunks must be positive; sizes must be distinct')
    reference = reference_variant(args.variants, args.reference)
    sources = {name: labstate.source(name, allow_archived=args.allow_archived) for name in args.variants}
    rows = config_rows(args.config.resolve())
    original = labstate.load_lock(args.input_manifest)
    paths = set(original['runtime_files']) | {original['cfg_bench']['path']}
    for graph, grammar, _, pool in rows:
        paths.update(str((CFG_DIR/p).resolve()) for p in (graph, grammar, str(Path(grammar).with_suffix('.rsm')), pool))
    lock = {**original, 'files': {p: original['files'][p] for p in sorted(paths)}}
    lock['files'].update({str(p): {'sha256': labstate.digest(p)} for p in sources.values()})
    labstate.verify_lock(lock)
    stamp = datetime.now(ZoneInfo('Europe/Moscow')).strftime('%Y-%m-%d_%H-%M-%S-%f')
    out = (args.out or LAB/'runs'/(stamp+'_simple')).resolve()
    env = dict(os.environ)  # Preserve every OpenMP setting, including absent ones.
    env['LD_LIBRARY_PATH'] = ':'.join(map(str, LIB_DIRS))
    env['GRAPHBLAS_CACHE_PATH'] = str(out/'jit-cache')
    print('Output:', out, flush=True)
    if not args.dry_run:
        out.mkdir(parents=True, exist_ok=False)
        (out/'libs').mkdir()
        (out/'sources').mkdir()
        (out/'jit-cache').mkdir()
        shutil.copyfile(args.config, out/'config.csv')
        settings = dict(variants=args.variants, reference=reference, source_sizes=sizes, chunks=args.chunks,
                        rounds=args.rounds, hot=args.hot, seed=args.seed, schedule='consecutive-per-chunk',
                        source_hashes={n: labstate.digest(p) for n, p in sources.items()},
                        timing_scopes=original.get('timing_scopes', {}),
                        openmp_environment=labstate.openmp_environment(), cpu_affinity=labstate.cpu_affinity())
        (out/'settings.json').write_text(json.dumps(settings, indent=2)+'\n')
        lock['files'][str(out/'config.csv')] = {'sha256': labstate.digest(out/'config.csv')}
        lock['recipes'] = {'simple': settings}
        lock['id'] = 'cfpq-simple-'+stamp
        lock['parent_manifest_sha256'] = labstate.digest(args.input_manifest)
        (out/'inputs.json').write_text(json.dumps(lock, indent=2)+'\n')
        shutil.copyfile(__file__, out/'run_simple.py')
    for name, source in sources.items():
        if not args.dry_run:
            shutil.copyfile(source, out/'sources'/f'{name}.c')
        command(build_command(source, out/'libs'/f'{name}.so'), ROOT, out, 'build-'+name, env, args.dry_run)
    if not args.build_only:
        for name in sources:
            labstate.verify_lock(lock)
            command(benchmark_command(Path(lock['cfg_bench']['path']), out/'config.csv',
                                      out/'libs'/f'{name}.so', out/f'{name}.csv', args),
                    CFG_DIR, out, name, env, args.dry_run)
            labstate.verify_lock(lock)
        if not args.dry_run:
            analyze(out)
    print('Output:', out)


if __name__ == '__main__':
    main()
