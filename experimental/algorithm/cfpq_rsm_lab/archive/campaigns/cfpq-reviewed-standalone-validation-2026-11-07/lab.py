#!/usr/bin/env python3
"""One entry point for registered CFPQ RSM variants. Use --help or make help."""
from __future__ import annotations
import argparse
import datetime
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
from zoneinfo import ZoneInfo
import labstate
import experiment_status as status

LAB = Path(__file__).resolve().parent
ROOT = LAB.parents[2]


def arguments():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['list', 'verify', 'check', 'correctness', 'compare', 'profile', 'plot', 'tooling-tests', 'status', 'status-check'])
    p.add_argument('--variant', default='baseline', help='registered active name, or all for checks')
    p.add_argument('--variants', default='baseline,p1,p2,p3,p4,stack_p1234')
    p.add_argument('--recipe', default=None, help='smoke/full or profile_smoke/profile_workloads, from inputs/pinned.json')
    p.add_argument('--sizes', help='subset of the recipe source sizes, comma-separated')
    p.add_argument('--run', type=Path, help='completed timing run to plot')
    p.add_argument('--label', help='optional output label')
    p.add_argument('--all', action='store_true', help='include archives in list')
    p.add_argument('--dry-run', action='store_true', help='print commands without building or running kernels')
    return p.parse_args()


def environment():
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['OMP_NUM_THREADS'] = '1'
    env.pop('OMP_THREAD_LIMIT', None)
    env.pop('CFPQ_RSM_PROFILE_FILE', None)
    env.pop('CFPQ_RSM_PROFILE_TAG', None)
    env['LD_LIBRARY_PATH'] = ':'.join(map(str, [ROOT/'build-exp-release/experimental', ROOT/'build/src', ROOT/'build/src/test', Path('/home/hollow/repo/GraphBLAS/build')]))
    return env


def output(label: str, dry: bool) -> Path:
    stamp = datetime.datetime.now(ZoneInfo('Europe/Moscow')).strftime('%Y-%m-%d_%H-%M-%S-%f')
    if not label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in label):
        raise SystemExit('label must contain only letters, digits, underscore or hyphen')
    out = LAB/'runs'/(stamp+'_'+label)
    if not dry: out.mkdir(parents=True, exist_ok=False)
    return out


def execute(cmd, out: Path, label: str, dry: bool, cwd: Path = ROOT, timeout=None):
    cmd = list(map(str, cmd))
    print('+ '+shlex.join(cmd), flush=True)
    if dry: return
    out.mkdir(parents=True, exist_ok=True)
    log = out/(label+'.log')
    context = status.execution_context(cmd, label, cwd)
    started = status.now()
    exit_code, outcome, error = None, 'FAIL', None
    with log.open('w') as f:
        f.write('+ '+shlex.join(cmd)+'\n'); f.flush()
        status.record_start(context,out,label,log,started)
        status.refresh()
        try:
            env = environment()
            env['GRAPHBLAS_CACHE_PATH'] = str(out/'jit-cache')
            r = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
            exit_code = r.returncode
            outcome = 'PASS' if exit_code == 0 else ('INCOMPLETE' if exit_code == 124 else 'FAIL')
        except subprocess.TimeoutExpired:
            outcome, error = 'INCOMPLETE', 'command timed out'
        except OSError as exc:
            error = str(exc); f.write(error+'\n')
    receipt = status.record(context, out, label, log, started, exit_code, outcome, error)
    status.refresh()
    print(log.read_text()[-1800:], end='', flush=True)
    print('Receipt: '+str(receipt), flush=True)
    if outcome != 'PASS':
        raise SystemExit(f'{outcome} exit={exit_code}; log {log}')


def check(name: str, out: Path, args):
    src = labstate.source(name)
    base = ['make','-C',LAB/'tests',f'R2={src}',f'BIN_DIR={out/"tests"}']
    execute(base+['check','existing'],out,'oracle-existing',args.dry_run,timeout=90)


def runner(recipe, variants, out, args, *, profile=None):
    lock = labstate.load_lock()
    flags = '-O2 -fPIC -shared -std=c11 -L/home/hollow/repo/GraphBLAS/build -Wl,-rpath,'+str(ROOT/'build/src')+' -Wl,-rpath,/home/hollow/repo/GraphBLAS/build'
    cmd = [sys.executable,LAB/'run.py','--input-manifest',labstate.LOCK,
           '--baseline',profile or labstate.source('baseline'),'--variants-dir',LAB/'variants',
           '--variants',','.join(variants),'--cfg-bench-dir',lock['cfg_bench_dir'],
           '--cfg-bench',lock['cfg_bench']['path'],'--skip-cfg-build',
           '--lagraph-lib-dir',ROOT/'build/src','--cflags',flags,
           '--config',recipe['config'],'--chunks',recipe['chunks'],'--rounds',recipe['rounds'],
           '--hot-rounds',recipe['hot_rounds'],'--seed',recipe['seed'],
           '--threads',recipe['threads'],'--order',recipe['order'],'--out',out]
    if profile: cmd+=['--profile']
    return cmd


def main():
    args = arguments()
    if args.action in ('status','status-check'):
        report = status.refresh(check_only=args.action=='status-check')
        print(report['overall'])
        print('Evidence: VALIDATION.json; campaign: CURRENT_EXPERIMENT.md')
        return 0
    if args.action=='list': status.refresh()
    rows = labstate.registry(); lock = labstate.load_lock()
    if args.action == 'list':
        print('NAME             STATUS       PARENT           CORRECTNESS / BENCHMARK')
        for name,row in rows.items():
            if args.all or row['status'] != 'archived':
                print(f"{name:16} {row['status']:12} {row['parent']:16} {row['correctness']} / {row['benchmark']}")
                print('  '+row['description']+'\n  '+row['source'])
        print(f"\n{len(rows)} snapshots; {sum(r['status']=='active' for r in rows.values())} active. Full catalogue: variants/REGISTRY.csv")
        return 0
    if args.action == 'verify':
        out=output('integrity-check',args.dry_run)
        script="import labstate; labstate.verify_lock(labstate.load_lock()); print(f'Input/binary/runtime lock verified; {labstate.verify_diffs()} snapshot hashes and patch applications verified')"
        execute([sys.executable,'-c',script],out,'integrity',args.dry_run,cwd=LAB,timeout=90)
        return 0
    if args.action == 'plot':
        if args.run is None: raise SystemExit('plot requires --run PATH (make plot RUN=PATH)')
        run = args.run.resolve()
        if not run.is_relative_to(LAB/'runs'): raise SystemExit('plot expects a run under this lab/runs directory')
        execute([sys.executable,LAB/'figures/current_vs_bb7/plot_comparison.py','--summary',run/'summary.csv','--output',run/'figures/comparison'],run,'plot',args.dry_run)
        return 0
    if args.action == 'tooling-tests':
        out=output('tooling-tests',args.dry_run)
        execute([sys.executable,'-m','unittest','discover','-s',LAB/'tests','-p','test_*.py'],out,'unittest',args.dry_run,timeout=90)
        return 0
    if args.action in ('check','correctness'):
        names = [n for n,r in rows.items() if r['status']=='active'] if args.variant=='all' else [args.variant]
        for name in names:
            labstate.source(name)
            out=output(args.label or args.action+'-'+name,args.dry_run)
            check(name,out,args)
            if args.action == 'correctness':
                selected=list(dict.fromkeys(['baseline',name]))
                recipe=lock['recipes']['smoke'];cmd=runner(recipe,selected,out/'smoke',args)+['--source-size','1']
                execute(cmd,out,'loaded-smoke',args.dry_run,timeout=90)
                files = [Path(lock['reduced_config'])]
                # Verify every reduced graph/query/source before the loaded-library sweep.
                if not args.dry_run:
                    import run as run_module
                    files += run_module.config_input_paths(Path(lock['reduced_config']),Path(lock['cfg_bench_dir']))
                    labstate.verify_lock(lock,list(map(str,files)))
                cfg=lock['cfg_bench']['path']; lib=out/'smoke/libs'/(name+'.so')
                execute(['timeout','60',cfg,'-t','-c',lock['reduced_config'],'-a','CFL_CFPQ_RSM','--rsm-lib',lib,'--use-start-nodes','--compute-results'],out,'reduced15',args.dry_run,cwd=Path(lock['cfg_bench_dir']),timeout=70)
                if not args.dry_run: labstate.verify_lock(lock,list(map(str,files)))
        return 0
    profile = args.action == 'profile'
    recipe_name = args.recipe or ('profile_smoke' if profile else 'smoke')
    recipe = lock['recipes'].get(recipe_name)
    if recipe is None or recipe['mode'] != ('profile' if profile else 'timing'):
        raise SystemExit('recipe mode mismatch; see inputs/pinned.json')
    names=[args.variant] if profile else list(dict.fromkeys(['baseline']+args.variants.split(',')))
    for name in names:labstate.source(name)
    sizes=list(map(int,args.sizes.split(','))) if args.sizes else recipe['source_sizes']
    if not set(sizes)<=set(recipe['source_sizes']):raise SystemExit('source size is not in the pinned recipe')
    for size in sizes:
        out=output(args.label or args.action+'-'+recipe_name+'-s'+str(size),args.dry_run)
        cmd=runner(recipe,names,out,args,profile=labstate.source(names[0]) if profile else None)+['--source-size',size]
        execute(cmd,out,'command',args.dry_run)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
