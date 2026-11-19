"""Evidence-derived status for the explicit current campaign; no kernel execution."""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import io
import fcntl
import os
import tempfile
import hashlib
import json
from pathlib import Path
import re
from types import SimpleNamespace
from zoneinfo import ZoneInfo

import labstate

LAB = Path(__file__).resolve().parent
CAMPAIGN = LAB / 'CURRENT_CAMPAIGN.json'
REPORT = LAB / 'VALIDATION.json'
CURRENT = LAB / 'CURRENT_EXPERIMENT.md'
CHECKS = ('oracle_existing', 'timing_smoke', 'reduced', 'profile_smoke')


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def relative(path: Path) -> str:
    path = path.resolve()
    return str(path.relative_to(LAB)) if path.is_relative_to(LAB) else str(path)


def atomic_write(path: Path, text: str) -> None:
    # Unique temporary names also make independent run receipts safe to write
    # while a human requests a status refresh in another process.
    mode=path.stat().st_mode & 0o777 if path.exists() else 0o644
    temporary=None
    try:
        with tempfile.NamedTemporaryFile(mode='w',dir=path.parent,prefix=path.name+'.',suffix='.tmp',delete=False) as stream:
            temporary=Path(stream.name)
            stream.write(text); stream.flush(); os.fsync(stream.fileno())
        temporary.chmod(mode)
        temporary.replace(path)
    finally:
        if temporary is not None and temporary.exists(): temporary.unlink()


def files_identity(paths) -> dict[str, str]:
    return {relative(Path(p)): labstate.digest(Path(p)) for p in sorted(set(map(str, paths)))}


def campaign() -> dict:
    return json.loads(CAMPAIGN.read_text())


def registry_identity() -> str:
    rows=labstate.registry()
    projection={n:{k:r[k] for k in ('source','sha256','parent','diff_parent','diff','status')} for n,r in rows.items()}
    return hashlib.sha256(json.dumps(projection,sort_keys=True).encode()).hexdigest()


def execution_context(cmd: list[str], label: str, cwd: Path) -> dict:
    """Bind a command to frozen sources, tools and its exact recipe before execution."""
    definition = campaign()
    rows = labstate.registry()
    selected = {}
    tracked = [LAB/'lab.py', LAB/'labstate.py', LAB/'experiment_status.py', CAMPAIGN]
    kind, recipe, output, source_size = 'other', None, None, None
    def arg(flag):
        return cmd[cmd.index(flag)+1] if flag in cmd else None
    r2 = next((part[3:] for part in cmd if part.startswith('R2=')), None)
    if r2:
        selected = {n:r['sha256'] for n,r in rows.items()
                    if (LAB/r['source']).resolve() == Path(r2).resolve()}
        kind = 'oracle_existing'
        tracked += [LAB/'tests/Makefile', LAB/'tests/test_ds00.c', LAB/'tests/oracle.c',
                    LAB/'tests/oracle.h', LAB.parents[2]/'experimental/test/test_CFPQ_RSM.c']
    elif str(LAB/'run.py') in cmd:
        kind = 'profile_smoke' if '--profile' in cmd else 'timing_smoke'
        names = (arg('--variants') or '').split(',')
        if '--profile' in cmd:
            names = [n for n,r in rows.items() if (LAB/r['source']).resolve() == Path(arg('--baseline')).resolve()]
        selected = {n: rows[n]['sha256'] for n in names if n in rows}
        tracked += [LAB/'run.py']
        if '--profile' in cmd:
            tracked += [LAB/'tests/make_profile_variant.py', LAB/'variants/cfpq_profile.h']
        output = arg('--out')
        lock = labstate.load_lock()
        values = {'config': arg('--config'), 'source_size': int(arg('--source-size')),
                  'chunks': int(arg('--chunks')), 'rounds': int(arg('--rounds')),
                  'seed': int(arg('--seed')), 'threads': int(arg('--threads') or '0'),
                  'hot_rounds': int(arg('--hot-rounds')), 'order': arg('--order'),
                  'mode': 'profile' if '--profile' in cmd else 'timing'}
        source_size = values['source_size']
        for name,r in lock['recipes'].items():
            if values['source_size'] in r['source_sizes'] and all(values[k] == r[k] for k in values if k != 'source_size'):
                recipe = name
                break
        if recipe == 'full': kind = 'performance'
        elif recipe == 'profile_workloads': kind = 'profile_workloads'
        if recipe is None: kind = 'unrecognized_recipe'
    elif '--rsm-lib' in cmd and '-t' in cmd:
        path = Path(arg('--rsm-lib'))
        name = path.stem
        if name in rows: selected[name] = rows[name]['sha256']
        kind, recipe = 'reduced', 'reduced'
        tracked += [path]
        output = str(path.parent.parent)
    elif '-m' in cmd and 'unittest' in cmd:
        kind = 'tooling'
        tracked += [LAB/'run.py', LAB/'tests/make_profile_variant.py', LAB/'variants/cfpq_profile.h']
        tracked += list((LAB/'tests').glob('test_*.py'))
    elif label == 'integrity':
        kind = 'integrity'
        tracked += [LAB/r['source'] for r in rows.values()]
        tracked += [LAB/r['diff'] for r in rows.values()]
    elif label == 'plot':
        kind = 'plot'
        tracked += [LAB/'figures/current_vs_bb7/plot_comparison.py']
    tracked += [LAB/r['source'] for n,r in rows.items() if n in selected]
    return {'schema':1, 'campaign':definition['id'], 'kind':kind, 'recipe':recipe,
            'variants':selected, 'source_size':source_size, 'tools_and_sources':files_identity(tracked),
            'input_manifest_sha256':labstate.digest(labstate.LOCK),
            'output':relative(Path(output)) if output else None,
            'command':cmd, 'cwd':str(cwd.resolve()), 'registry_identities_sha256':registry_identity(),
            'threading_policy':'inherit-openmp-environment',
            'openmp_environment':labstate.openmp_environment(), 'cpu_affinity':labstate.cpu_affinity()}


def artifact_paths(context: dict, out: Path, log: Path) -> list[Path]:
    paths = [log]
    if context.get('output'):
        base = LAB/context['output']
        for name in ('summary.csv','execution_status.tsv','pinned-inputs.json','provenance.txt',
                     'inputs.sha256','profile.csv','profile-calls.csv'):
            if (base/name).is_file(): paths.append(base/name)
        for pattern in ('raw/*/round*.csv','profile/results-*.csv','libs/*.stamp','libs/*.so',
                        'profile/src/*.c','profile/profile-*'):
            paths.extend(base.glob(pattern))
    if context['kind'] == 'oracle_existing':
        paths.extend((out/'tests').glob('*_r2'))
    return sorted(set(paths))


def record_start(context: dict, out: Path, label: str, log: Path, started: str) -> Path:
    path=out/(label+'.start.json')
    if path.exists(): raise SystemExit(f'refusing to overwrite an execution start: {path}')
    receipt={**context,'started_at':started,'finished_at':started,'exit_code':None,
             'outcome':'INCOMPLETE','error':'execution started; no completion receipt yet (running or interrupted)',
             'log':relative(log),'artifacts':{}}
    atomic_write(path,json.dumps(receipt,indent=2)+'\n')
    return path


def record(context: dict, out: Path, label: str, log: Path, started: str,
           exit_code: int | None, outcome: str, error: str | None = None) -> Path:
    receipt = {**context, 'started_at':started, 'finished_at':now(),
               'exit_code':exit_code, 'outcome':outcome, 'error':error,
               'log':relative(log), 'artifacts':files_identity(artifact_paths(context,out,log))}
    receipt_path = out/(label+'.result.json')
    if receipt_path.exists():
        raise SystemExit(f'refusing to overwrite an execution receipt: {receipt_path}')
    atomic_write(receipt_path,json.dumps(receipt,indent=2)+'\n')
    return receipt_path


def _rows(path: Path, delimiter=',') -> list[dict]:
    with path.open(newline='') as f:
        return list(csv.DictReader(f,delimiter=delimiter))


def verify_receipt(receipt: dict, variants: dict[str, str], lock_sha: str) -> dict:
    """Return a status without falling back to an older successful attempt."""
    result = {'status':'PASS', 'reason':'receipt and artifacts verified',
              'receipt':receipt.get('_path'), 'finished_at':receipt.get('finished_at'),
              'exit_code':receipt.get('exit_code')}
    def fail(state, why):
        result.update(status=state,reason=why)
        return result
    if any(variants.get(n) != h for n,h in receipt.get('variants',{}).items()):
        return fail('STALE','source identity differs from this campaign')
    if receipt.get('kind') == 'integrity' and receipt.get('registry_identities_sha256') != registry_identity():
        return fail('STALE','registry identities or diff parents changed')
    if receipt.get('input_manifest_sha256') != lock_sha:
        return fail('STALE','pinned manifest identity changed')
    if 'openmp_environment' in receipt and receipt['openmp_environment'] != labstate.openmp_environment():
        return fail('STALE','inherited OpenMP environment differs from this invocation')
    if 'cpu_affinity' in receipt and receipt['cpu_affinity'] != labstate.cpu_affinity():
        return fail('STALE','CPU affinity differs from this invocation')
    for name,expected in {**receipt.get('tools_and_sources',{}),**receipt.get('artifacts',{})}.items():
        p = LAB/name
        if not p.is_file() or labstate.digest(p) != expected:
            return fail('STALE',f'missing or changed evidence/tool: {name}')
    if receipt.get('outcome') == 'INCOMPLETE':
        return fail('INCOMPLETE',receipt.get('error') or 'execution timed out')
    if receipt.get('exit_code') != 0 or receipt.get('outcome') != 'PASS':
        return fail('FAIL',receipt.get('error') or f"command exited {receipt.get('exit_code')}")
    try:
        kind = receipt['kind']
        log = (LAB/receipt['log']).read_text()
        if kind == 'oracle_existing':
            match = re.search(r'checks=(\d+) failures=0',log)
            if not match or '200 randomized cases, base seed 2026' not in log or 'PASS: deterministic regressions and randomized oracle cases' not in log:
                return fail('FAIL','oracle suite is incomplete or did not pass')
            passed = re.findall(r'^Test\s+\S+.*\[ OK \]',log,re.M)
            if len(passed) != 10 or 'SUCCESS: All unit tests have passed.' not in log:
                return fail('FAIL','existing 10-case C suite did not complete')
            result['checks'] = int(match[1]); result['existing_tests'] = len(passed)
        elif kind == 'tooling':
            match = re.search(r'Ran (\d+) tests in [^\n]+\n\s*OK\s*$',log)
            if not match: return fail('FAIL','tooling suite lacks a complete successful result')
            result['tests'] = int(match[1])
        elif kind == 'reduced':
            # Verify full config identities, not simply fifteen arbitrary OK lines.
            lock = labstate.load_lock()
            import run
            expected = run.read_config_pairs(Path(lock['reduced_config']))
            found = re.findall(r'CONFIG: grammar: ([^,\n]+), graph: ([^\n]+)',log)
            wanted = sorted((cnf,graph) for graph,cnf in expected)
            if sorted(found) != wanted or len(re.findall(r'Return code: 0\).*?\[OK\]',log)) != len(expected):
                return fail('FAIL','reduced-case identities/results are incomplete or incorrect')
            result['cases'] = len(expected)
            result['reference_cache'] = 'cached counts' if '(Cached:' in log else 'computed counts'
            name = next(iter(receipt['variants']))
            lib = LAB/receipt['output']/'libs'/(name+'.so')
            if str(lib) not in receipt['command']:
                return fail('FAIL','reduced check is not bound to its recorded loaded library')
        elif kind in ('timing_smoke','performance','profile_smoke','profile_workloads'):
            result.update(validate_runner_artifacts(receipt))
        elif kind == 'integrity':
            match=re.search(r'Input/binary/runtime lock verified; (\d+) snapshot hashes and patch applications verified',log)
            if not match or int(match[1])!=len(labstate.registry()):
                return fail('FAIL','integrity audit did not verify the entire registry')
            result['snapshots']=int(match[1])
        elif kind == 'plot':
            result['scope'] = 'plot command only'
    except (OSError,ValueError,KeyError,IndexError,SystemExit) as error:
        return fail('FAIL',f'invalid/incomplete result artifacts: {error}')
    return result


def validate_runner_artifacts(receipt: dict) -> dict:
    """Reuse the runner's count/hash acceptance, independently of its summary text."""
    import run
    lock = labstate.load_lock()
    recipe = lock['recipes'][receipt['recipe']]
    cmd = receipt['command']; size = int(cmd[cmd.index('--source-size')+1])
    expected = run.expected_keys(run.read_config_pairs(Path(recipe['config'])),
                                 SimpleNamespace(source_size=size,chunks=recipe['chunks']))
    base = LAB/receipt['output']
    if labstate.digest(base/'pinned-inputs.json') != receipt['input_manifest_sha256']:
        raise ValueError('run used a different pinned manifest')
    if receipt['kind'] in ('timing_smoke','performance'):
        names = list(receipt['variants'])
        statuses = _rows(base/'execution_status.tsv','\t')
        scheduled = {(n,str(i)) for n in names for i in range(recipe['rounds'])}
        actual = [(r['display'],r['round']) for r in statuses]
        if len(actual) != len(scheduled) or set(actual) != scheduled or any(r['exit_code']!='0' or r['outcome']!='ok' for r in statuses):
            raise ValueError('child execution passes are failed, missing or duplicated')
        data = {n:{} for n in names}
        for n in names:
            for i in range(recipe['rounds']):
                for row in run.read_rows(base/'raw'/n/f'round{i}.csv'):
                    key = (row['graph'],row['cnf'],row['source_size'],row['chunk'])
                    run.add_result_row(data,n,key,i,row)
            stamp = (base/'libs'/(n+'.stamp')).read_text()
            if receipt['variants'][n] not in stamp:
                raise ValueError(f'{n} library stamp does not identify the selected source')
        problems,stats = run.accept_rows(data,expected,recipe['rounds'],'baseline')
        summary = _rows(base/'summary.csv')
        summary_keys = [(r['variant'],r['graph'],r['cnf'],r['source_size'],r['chunk']) for r in summary]
        wanted = {(n,*k) for n in names for k in expected}
        if len(summary_keys)!=len(wanted) or set(summary_keys)!=wanted or any(r['status']!='ok' or r['hash_ok']!='1' or r['count_ok']!='1' or int(r['n_runs'])!=recipe['rounds'] for r in summary):
            raise ValueError('summary completeness/result acceptance failed')
    else:
        data = {'reference':{},'profile':{}}
        for display in data:
            for path in (base/'profile').glob(f'results-{display}-*.csv'):
                for row in run.read_rows(path):
                    key = (row['graph'],row['cnf'],row['source_size'],row['chunk'])
                    run.add_result_row(data,display,key,0,row)
        problems,stats = run.accept_rows(data,expected,1,'reference')
        measured = _rows(base/'profile.csv'); calls = _rows(base/'profile-calls.csv')
        if len(measured)!=len(expected) or len(calls)!=2*len(expected):
            raise ValueError('missing/extra profile calls')
        for row in measured:
            key = (row['graph'],row['cnf'],row['source_size'],row['chunk'])
            if key not in expected or row['sanity_ok']!='1' or row['warmup_sanity_ok']!='1':
                raise ValueError('profile identities or sanity checks failed')
        identities = [(r['graph'],r['cnf'],r['source_size'],r['chunk'],r['warmup']) for r in calls]
        wanted = {( *k,w) for k in expected for w in ('0','1')}
        if len(identities)!=len(wanted) or set(identities)!=wanted or any(r['sanity_ok']!='1' for r in calls):
            raise ValueError('profile warm-up/measured identities are incomplete or repeated')
        if next(iter(receipt['variants'].values())) not in (base/'libs/baseline.stamp').read_text():
            raise ValueError('profile reference stamp does not identify the selected source')
    if problems or any(s['status']!='ok' for s in stats.values()):
        raise ValueError('; '.join(problems[:4]) or 'raw result acceptance failed')
    return {'cases_per_variant':len(expected), 'rounds':recipe['rounds'],
            'scope':'tiny smoke; no performance verdict' if receipt['recipe']=='smoke' else receipt['recipe']}


def latest_attempt(attempts: list[dict]) -> dict | None:
    return max(attempts,key=lambda r:(r['finished_at'],r.get('_path',''))) if attempts else None


def read_attempts(campaign_id: str) -> tuple[list[dict], list[str]]:
    attempts=[]; problems=[]; completed=set()
    for suffix in ('result','start'):
        for path in sorted((LAB/'runs').glob('*/*.'+suffix+'.json')):
            identity=str(path).removesuffix('.'+suffix+'.json')
            if suffix=='start' and identity in completed: continue
            try:
                receipt=json.loads(path.read_text())
                if receipt.get('campaign')!=campaign_id: continue
                required=('kind','finished_at','started_at','variants','tools_and_sources','artifacts','exit_code','outcome','log')
                if receipt.get('schema')!=1 or any(k not in receipt for k in required):
                    raise ValueError('incomplete or unsupported execution receipt')
                if not isinstance(receipt['variants'],dict) or not isinstance(receipt['artifacts'],dict):
                    raise ValueError('invalid receipt identity/artifact maps')
                datetime.fromisoformat(receipt['finished_at'])
                receipt['_path']=relative(path)
                attempts.append(receipt)
                if suffix=='result': completed.add(identity)
            except (OSError,ValueError,TypeError) as error:
                problems.append(f'{relative(path)}: unreadable receipt: {error}')
    return attempts,problems


def state() -> dict:
    definition = campaign(); rows = labstate.registry()
    locked = definition['variants']; lock_sha = labstate.digest(labstate.LOCK)
    attempts,problems=read_attempts(definition['id'])
    environment_error=None
    try:
        labstate.verify_lock(labstate.load_lock())
    except SystemExit as error:
        environment_error=str(error); problems.append(environment_error)
    def select(kind,name=None,size=None):
        selected = latest_attempt([r for r in attempts if r['kind']==kind and (name is None or name in r.get('variants',{})) and (size is None or r.get('source_size')==size)])
        result=verify_receipt(selected,locked,lock_sha) if selected else {'status':'NOT_RUN','reason':'no current-campaign execution receipt'}
        if environment_error: result.update(status='STALE',reason=environment_error)
        return result
    variants={}
    sizes=definition['planned_performance']['source_sizes']
    for name,sha in locked.items():
        checks={kind:select(kind,name) for kind in CHECKS}
        row=rows.get(name)
        integrity='PASS'
        try:
            if row is None or row['sha256']!=sha: raise ValueError('registry source differs from campaign')
            labstate.source(name)
        except (SystemExit,ValueError) as error:
            integrity='STALE'; problems.append(f'{name}: {error}')
        performance_by_size={str(size):select('performance',name,size) for size in sizes}
        profiles_by_size={str(size):select('profile_workloads',name,size) for size in sizes}
        performance=coverage_status(performance_by_size)
        variants[name]={'source':row['source'] if row else None,'sha256':sha,'integrity':integrity,
                        'checks':checks,'performance':performance,'performance_by_source_size':performance_by_size,'workload_profiles_by_source_size':profiles_by_size}
    production_sha=labstate.digest(LAB.parent/'LAGraph_CFPQ_RSM.c')
    if production_sha!=locked.get('baseline'):
        problems.append('production source differs from the agreed baseline')
    tooling=select('tooling'); integrity_audit=select('integrity')
    bad_attempts=[]
    # Failed attempts remain visible even after a later successful retry.
    for r in attempts:
        if r.get('outcome')!='PASS' or r.get('exit_code')!=0:
            bad_attempts.append({'receipt':r['_path'],'kind':r['kind'],'exit_code':r.get('exit_code'),
                                 'outcome':r.get('outcome'),'variants':list(r.get('variants',{}))})
    healthy=(not problems and tooling['status']=='PASS' and integrity_audit['status']=='PASS' and all(v['integrity']=='PASS' and all(c['status']=='PASS' for c in v['checks'].values()) for v in variants.values()))
    return {'schema':1,'campaign':definition['id'],'title':definition['title'],
            'input_manifest':relative(labstate.LOCK),'input_manifest_sha256':lock_sha,
            'campaign_sha256':labstate.digest(CAMPAIGN),'registry_snapshots':len(rows),
            'distinct_source_hashes':len({r['sha256'] for r in rows.values()}),
            'production_sha256':production_sha,
            'threading_policy':'inherit-openmp-environment',
            'openmp_environment':labstate.openmp_environment(), 'cpu_affinity':labstate.cpu_affinity(),
            'overall':'VALIDATED_FOR_CORRECTNESS_AND_TINY_PROFILING' if healthy else 'NEEDS_ATTENTION',
            'latest_attempt_at':max((r['finished_at'] for r in attempts),default=None),'tooling':tooling,'integrity_audit':integrity_audit,'validation_plan':definition['validation_plan'],'variants':variants,'problems':problems,'failed_attempts':bad_attempts,
            'known_limits':definition['known_limits'],
            'legacy_evidence':'Pre-receipt runs retained in runs/ and build/. They are not promoted into current PASS statuses.',
            'performance_conclusion':'NOT MEASURED unless a complete pinned full-recipe receipt passes raw acceptance.'}


def coverage_status(by_size: dict) -> dict:
    statuses=[r['status'] for r in by_size.values()]
    if any(s in ('FAIL','STALE','INCOMPLETE') for s in statuses):
        result='NEEDS_ATTENTION'
    elif all(s=='PASS' for s in statuses): result='PASS'
    elif all(s=='NOT_RUN' for s in statuses): result='NOT_RUN'
    else: result='PARTIAL'
    return {'status':result,'measured_source_sizes':[int(n) for n,r in by_size.items() if r['status']=='PASS']}


def registry_statuses(row: dict, report: dict) -> dict:
    result=dict(row)
    if row['name'] not in report['variants']: return result
    v=report['variants'][row['name']]
    c=v['checks']
    result['correctness']='; '.join(f'{n}={c[n]["status"]}' for n in ('oracle_existing','timing_smoke','reduced'))
    result['profiling']=c['profile_smoke']['status']+': tiny profile; full workload profile NOT RUN'
    result['benchmark']='NOT MEASURED; smoke only' if v['performance']['status']=='NOT_RUN' else v['performance']['status']+': full recipe'
    return result


def render_current(report: dict) -> str:
    definition=campaign(); lock=labstate.load_lock()
    latest=datetime.fromisoformat(report['latest_attempt_at']).astimezone(ZoneInfo('Europe/Moscow')).isoformat() if report['latest_attempt_at'] else 'none'
    timing_scope='Only bounded validation is recorded; no full-recipe performance receipt exists.' if all(v['performance']['status']=='NOT_RUN' for v in report['variants'].values()) else 'Full-recipe attempts are recorded separately by source size in VALIDATION.json.'
    lines=['# Current CFPQ RSM experiment','',
           '> Generated from execution receipts by `make status`; do not edit validation claims by hand.', '',
           f"Campaign: `{report['campaign']}`. State: **{report['overall']}**.", '',
           f"Latest recorded attempt: `{latest}` (Europe/Moscow).", '',
           report['title']+'. '+timing_scope+' Production identity is checked against the agreed baseline.', '',
           f"Registry: {report['registry_snapshots']} snapshots / {report['distinct_source_hashes']} distinct source hashes. [Machine evidence](VALIDATION.json), [campaign definition](CURRENT_CAMPAIGN.json), [registry](variants/REGISTRY.md), [backup](BACKUP.md).", '',
           '| Variant | Source SHA-256 (prefix) | Oracle + existing | Loaded smoke | Reduced data | Tiny profile | Full performance |',
           '| --- | --- | --- | --- | --- | --- | --- |']
    for name,v in report['variants'].items():
        c=v['checks']
        lines.append(f"| {name} | `{v['sha256'][:12]}` | {c['oracle_existing']['status']} | {c['timing_smoke']['status']} | {c['reduced']['status']} | {c['profile_smoke']['status']} | {'NOT MEASURED' if v['performance']['status']=='NOT_RUN' else v['performance']['status']} |")
    lines += ['',f"Tooling tests: **{report['tooling']['status']}**"+(f" ({report['tooling']['tests']} tests)." if 'tests' in report['tooling'] else '.'),'',
              'PASS means the latest matching attempt has exit 0, unchanged source/tool identities and artifacts, and passes result-completeness checks. A later failure or timeout supersedes an earlier pass; changed/missing evidence becomes STALE. Old logs without receipts are retained but cannot establish a current PASS.', '',
              f"Environment and all registered diff proofs: **{report['integrity_audit']['status']}**.", '',
              'The fixed recipes and current scope:', '',
              '| Recipe/check | Config | Source size | Chunks | Measured rounds | State |',
              '| --- | --- | --- | --- | --- | --- |']
    for recipe in ('smoke','profile_smoke','full','profile_workloads'):
        r=lock['recipes'][recipe]
        measured='per-variant status above' if recipe in ('smoke','profile_smoke') else 'per-source-size attempts in VALIDATION.json' if recipe=='profile_workloads' else 'per-source-size coverage in VALIDATION.json'
        lines.append(f"| {recipe} | `{r['config']}` | {','.join(map(str,r['source_sizes']))} | {r['chunks']} | {r['rounds']} | {measured} |")
    lines += [f"| reduced correctness | `{lock['reduced_config']}` | supplied starts | not chunked | test mode | 15-case count check |", '',
              'All timed/profile recipes use inherited OpenMP threading. The tooling never sets or unsets OpenMP variables; GraphBLAS/OpenMP choose parallelism. Recipes pin seed 2026, blocked order, one warm-up, the executable path/hash, graphs/CNF/RSM/source pools, and runtime dependencies in `inputs/pinned.json`. Legacy threads=0 means inheritance, not a thread count. The 200-case oracle uses seed 2026. Reduced counts may use the CNF-reference cache; exact small-case oracle checks provide stronger set evidence.', '',
              'Inherited OpenMP environment: `'+json.dumps(report['openmp_environment'],sort_keys=True)+'`. CPU affinity: `'+json.dumps(report['cpu_affinity'])+'`. An empty environment record means runtime defaults. Actual per-operation thread counts are not measured. Receipts from a different environment or affinity are STALE for this invocation.', '',
              'Reproduce from the lab root:', '', '```bash', 'make status', 'make status-check', 'make list',
              'make verify', 'make tooling-tests', *[f'make correctness VARIANT={n}' for n in report['variants']],
              'make profile VARIANT='+next(reversed(report['variants'])),
              'make compare RECIPE=full SIZES=100 VARIANTS='+','.join(report['variants'])+' DRY_RUN=1','```','',
              'The final command only prints the expensive full recipe. Removing DRY_RUN deliberately starts performance work; it is not part of the current validation.', '',
              'Evidence selected for each check:', '', '| Variant/check | Latest receipt | Result |', '| --- | --- | --- |']
    for name,v in report['variants'].items():
        for kind,c in v['checks'].items():
            link=f"[receipt]({c['receipt']})" if c.get('receipt') else 'none'
            lines.append(f"| {name}/{kind} | {link} | {c['status']}: {c['reason']} |")
    if report['integrity_audit'].get('receipt'):lines += ['',f"Integrity receipt: [{report['integrity_audit']['receipt']}]({report['integrity_audit']['receipt']})."]
    if report['tooling'].get('receipt'):lines += ['',f"Tooling receipt: [{report['tooling']['receipt']}]({report['tooling']['receipt']})."]
    lines += ['', 'Unmeasured work and limits:', ''] + ['- '+x for x in report['known_limits']]
    lines += ['', 'Failure history:', '']
    lines += [f"- [{r['receipt']}]({r['receipt']}): {r['outcome']} / exit {r['exit_code']} ({r['kind']})." for r in report['failed_attempts']] or ['- No failed receipt in this campaign. Historical failed/partial runs remain in legacy evidence.']
    if report['problems']:lines += ['', 'Attention required:', '']+['- '+p for p in report['problems']]
    if 'java_comparison' in definition:
        java=definition['java_comparison']
        lines += ['', 'Requested Java-only comparison (separate simple-run results):', '', '```bash',
                  'python3 run_simple.py --config '+java['config']+' --variants '+' '.join(java['variants'])+
                  ' --reference '+java['reference']+' --source-sizes '+','.join(map(str,java['source_sizes']))+
                  ' --chunks '+str(java['chunks'])+' --rounds '+str(java['rounds'])+' --hot --dry-run',
                  '```', '', 'This selects Avrora and Batik and the two named variants. The one-shot timings include construction. Remove `--dry-run` to run the comparison. The Java results and legacy full-recipe results cover different workloads.']
    return '\n'.join(lines)+'\n'


def registry_markdown(report: dict, original: str) -> str:
    begin='<!-- BEGIN GENERATED VALIDATION STATUS -->'
    end='<!-- END GENERATED VALIDATION STATUS -->'
    lines=[begin,'','Current evidence is generated by `make status`; full receipt details are in [VALIDATION.json](../VALIDATION.json).','',
           '| Active name | Correctness checks | Tiny profiling | Full performance |',
           '| --- | --- | --- | --- |']
    for name,v in report['variants'].items():
        c=v['checks']
        correctness='PASS' if all(c[k]['status']=='PASS' for k in ('oracle_existing','timing_smoke','reduced')) else ', '.join(k+'='+c[k]['status'] for k in ('oracle_existing','timing_smoke','reduced'))
        lines.append(f"| {name} | {correctness} | {c['profile_smoke']['status']} | {'NOT MEASURED' if v['performance']['status']=='NOT_RUN' else v['performance']['status']} |")
    block='\n'.join(lines+['',end])
    if begin in original:
        before,remainder=original.split(begin,1)
        _,after=remainder.split(end,1)
        return before+block+after
    return original.rstrip()+'\n\n'+block+'\n'


def _refresh_unlocked(check_only=False) -> dict:
    report=state()
    markdown=render_current(report)
    rows=list(labstate.registry().values())
    derived=[registry_statuses(r,report) for r in rows]
    stream=io.StringIO(newline=''); writer=csv.DictWriter(stream,fieldnames=rows[0],lineterminator='\n');writer.writeheader();writer.writerows(derived)
    csv_text=stream.getvalue()
    registry_md=LAB/'variants/REGISTRY.md'
    markdown_registry=registry_markdown(report,registry_md.read_text())
    if check_only:
        if not REPORT.exists() or json.loads(REPORT.read_text()) != report or not CURRENT.exists() or CURRENT.read_text()!=markdown or labstate.REGISTRY.read_text()!=csv_text or registry_md.read_text()!=markdown_registry:
            raise SystemExit('status files or registry are stale; run make status')
        if report['overall']!='VALIDATED_FOR_CORRECTNESS_AND_TINY_PROFILING':
            raise SystemExit('current validation is incomplete or needs attention; see CURRENT_EXPERIMENT.md')
    else:
        atomic_write(REPORT,json.dumps(report,indent=2)+'\n')
        atomic_write(CURRENT,markdown)
        atomic_write(labstate.REGISTRY,csv_text)
        atomic_write(registry_md,markdown_registry)
    return report


def refresh(check_only=False) -> dict:
    # Serialize the three generated views so an older concurrent writer cannot
    # overwrite a newer receipt's status. Queries remain outside this lock.
    with (LAB/'.status.lock').open('a') as guard:
        fcntl.flock(guard,fcntl.LOCK_EX)
        return _refresh_unlocked(check_only=check_only)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true',help='verify generated status and required checks without changing files')
    args=parser.parse_args(); report=refresh(check_only=args.check)
    print(report['overall'])
    for name,v in report['variants'].items():
        print(name+': '+', '.join(f'{k}={c["status"]}' for k,c in v['checks'].items())+'; performance='+('NOT MEASURED' if v['performance']['status']=='NOT_RUN' else v['performance']['status']))
    print('Evidence: VALIDATION.json; campaign: CURRENT_EXPERIMENT.md')
    return 0

if __name__=='__main__':raise SystemExit(main())
