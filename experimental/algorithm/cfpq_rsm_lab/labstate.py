"""Frozen registry and input-lock validation shared by the CLI and runner."""
from __future__ import annotations
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

LAB = Path(__file__).resolve().parent
REGISTRY = LAB / 'variants/REGISTRY.csv'
LOCK = LAB / 'inputs/pinned.json'
KERNEL = Path('experimental/algorithm/LAGraph_CFPQ_RSM.c')


def openmp_environment(env=None) -> dict[str, str]:
    """Record runtime settings without supplying, changing or deleting any."""
    inherited = os.environ if env is None else env
    return {k: inherited[k] for k in sorted(inherited)
            if k.startswith(('OMP_', 'GOMP_', 'KMP_'))}


def cpu_affinity() -> list[int] | None:
    return sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def registry(path: Path = REGISTRY) -> dict[str, dict]:
    with path.open(newline='') as f:
        rows = list(csv.DictReader(f))
    result = {r['name']: r for r in rows}
    if len(result) != len(rows):
        raise SystemExit('duplicate variant name in registry')
    for r in rows:
        for field in ('source', 'diff'):
            p = (LAB / r[field]).resolve()
            if not p.is_relative_to(LAB) or not p.is_file():
                raise SystemExit(f"invalid registry {field} for {r['name']}: {p}")
    return result


def source(name: str, *, allow_archived: bool = False) -> Path:
    row = registry().get(name)
    if row is None:
        raise SystemExit(f'unregistered variant {name!r}; use make list')
    if row['status'] == 'archived' and not allow_archived:
        raise SystemExit(f'{name} is archived; explicit --allow-archived is required')
    path = LAB / row['source']
    if digest(path) != row['sha256']:
        raise SystemExit(f'frozen source hash mismatch: {name}: {path}')
    if path.stat().st_mode & 0o222:
        raise SystemExit(f'frozen snapshot is writable: {path}; expected mode 0444')
    return path


def verify_diffs() -> int:
    rows = registry()
    for name, row in rows.items():
        candidate = source(name, allow_archived=True)
        parent = source(row['diff_parent'], allow_archived=True)
        with tempfile.TemporaryDirectory(prefix='cfpq-patch-') as tmp:
            work = Path(tmp)
            target = work / KERNEL
            target.parent.mkdir(parents=True)
            target.write_bytes(parent.read_bytes())
            r = subprocess.run(['patch', '--batch', '-p1', '-s', '-i', str(LAB / row['diff'])],
                               cwd=work, capture_output=True, text=True)
            if r.returncode or digest(target) != row['sha256']:
                raise SystemExit(f"patch verification failed: {name}: {r.stdout}{r.stderr}")
            if target.read_bytes() != candidate.read_bytes():
                raise SystemExit(f'patch bytes differ: {name}')
    return len(rows)


def load_lock(path: Path = LOCK) -> dict:
    lock = json.loads(path.read_text())
    if lock.get('schema') != 1:
        raise SystemExit(f'unsupported input manifest schema: {path}')
    return lock


def verify_lock(lock: dict, selected: list[str] | None = None) -> None:
    paths = set(selected) if selected is not None else set(lock['files'])
    paths.update(lock['runtime_files'])
    paths.add(lock['cfg_bench']['path'])
    for name in sorted(paths):
        expected = lock['files'].get(name)
        p = Path(name)
        if expected is None or not p.is_file() or digest(p) != expected['sha256']:
            raise SystemExit(f'pinned file hash mismatch or missing file: {name}')
    p = Path(lock['cfg_bench']['path'])
    if digest(p) != lock['cfg_bench']['sha256']:
        raise SystemExit(f'pinned cfg_bench hash mismatch: {p}')


def validate_run(args, input_paths: list[Path]) -> dict | None:
    # Internal unit-test fixtures may supply a SimpleNamespace without a lock.
    # The public argparse CLI always supplies the pinned manifest; no CLI opt-out.
    path = getattr(args, 'input_manifest', None)
    if path is None:
        return None
    lock = load_lock(path)
    if not args.skip_cfg_build:
        raise SystemExit('pinned comparisons require --skip-cfg-build; use make compare/profile')
    binary = Path(lock['cfg_bench']['path'])
    if args.cfg_bench is None:
        args.cfg_bench = binary
    if args.cfg_bench.resolve() != binary.resolve():
        raise SystemExit(f'cfg_bench must be the pinned binary: {binary}')
    recipes = [r for r in lock['recipes'].values()
               if Path(r['config']).resolve() == args.config.resolve()
               and r['mode'] == ('profile' if args.profile else 'timing')]
    match = next((r for r in recipes if args.source_size in r['source_sizes']
                  and args.seed == r['seed'] and args.chunks == r['chunks']
                  and args.rounds == r['rounds'] and args.threads == r['threads']
                  and getattr(args, 'hot_rounds', 1) == r['hot_rounds']
                  and getattr(args, 'order', 'blocked') == r['order']), None)
    if match is None:
        raise SystemExit('run settings/config are not a pinned recipe; use make help')
    verify_lock(lock, [str(p.resolve()) for p in input_paths] + [str(args.config.resolve())])
    return lock
