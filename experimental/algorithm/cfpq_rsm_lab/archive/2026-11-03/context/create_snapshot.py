from pathlib import Path
import ast, collections, csv, datetime, difflib, hashlib, io, json, shutil, subprocess, tarfile

repo=Path('/home/hollow/repo/LAGraph')
lab=repo/'experimental/algorithm/cfpq_rsm_lab'
archive=lab/'archive/2026-11-03'
if archive.exists():
    raise SystemExit('Refusing to overwrite an existing snapshot')
archive.mkdir(parents=True)
for sub in ['sources','patches','results','context']:(archive/sub).mkdir()
sha=lambda b:hashlib.sha256(b).hexdigest()
def write_csv(path,rows,fields=None):
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fields or list(rows[0]));w.writeheader();w.writerows(rows)

root=lab/'build/runs/r10-overnight-2026-11-03_01-18-13'
old=lab/'build/runs/overnight-2026-11-02_03-36-52'
workloads=list(csv.DictReader((root/'analysis_s1_s10_workloads.csv').open()))
oldrows=list(csv.DictReader((old/'morning_summary.csv').open()))
shutil.copy2(root/'analysis_s1_s10_workloads.csv',archive/'results/r10_workloads_s1_s10.csv')
shutil.copy2(root/'ANALYSIS_S1_S10.md',archive/'results/R10_ANALYSIS.md')
for name in ['morning_summary.csv','morning_summary.md']:
    shutil.copy2(old/name,archive/'results'/('ds20_21_22_'+name))

specs=[
('ds20','Return orientation','Permanent column/hypersparse W and summary delta; includes conversion/reshape costs.','Negative completed experiment; retain without rerunning.'),
('ds21','Waiting-state deduplication','Subtract W from newly shifted waiting configurations before calls/products/union.','Negative completed experiment; sampled overlap was absent.'),
('ds22','Earlier block visited layout','Block P with extra fresh_B and call reshapes; differs from lean D1.','Negative completed experiment; does not settle D1.'),
('ds30_assign','Masked scalar insertion','Replace growing P, graph_nt and W unions with structural masked scalar true assignment.','Generally neutral/slower on completed 1/10-source runs.'),
('ds31_joint','Joint terminal eligibility','Index outgoing labels by RSM state and graph vertex; skip impossible terminal chains.','Very strong on Java, major regressions on RDF/C-alias; workload-specific.'),
('ds32_empty','Empty/first-contribution paths','Skip empty product unions and swap the first nonempty product into acc.','Promising broadly, especially Java; keep independent.'),
('ds33_format','Adaptive terminal orientation','Operand-size heuristic and private opposite-format cache of immutable graph terminals.','Mostly neutral/slower; Java regression at 10 sources.'),
('ds34_lazy_p','Lazy visited state','Disjoint frontier bags with cached cardinalities, size-rank merges and sequential subtraction.','No broad gain at 1/10 sources; C-alias regression at 10.'),
('ds35_lazy_nt','Lazy summary relations','Disjoint summary bags, streaming component products, sequential masks and final component mxv.','No broad gain at 1/10 sources; C-alias regression at 10.'),
('ds36_pair','Pattern products','Replace BOOL LOR_LAND products with ANY_PAIR under the accepted true-only contract.','Mostly small changes; no large demonstrated broad gain.'),
('ds37_fused','Fused accumulation','Accumulate graph products directly into acc with mxm/LOR, avoiding temporary union.','Regresses Java and C-alias at 10 sources.'),
('ds38_no_product_clear','One redundant-clear removal','Remove only the product scratch clear before overwrite mxm.','Mostly near baseline; not a blanket clear-removal experiment.'),
('ds39_direct_calls','Direct flat calls','Collect call tuples and bulk-build flat M_call, replacing diagonal/reshape/call mxm.','Modest improvements, especially C-alias.'),
('d1_native','Lean block P and calls','Native block call tuples and visited subtraction before copying/reshaping only fresh frontier.','Modest improvements; independent of earlier DS-22.'),
('config_block','Consistent block configurations','QV x V configurations with frontier tuple buckets and implicit RSM routing; no reshape or Kronecker matrix.','Strongest broad candidate so far; not promoted or combined.'),
('d3_masked_product','Visited-masked graph products','Add complemented block P masks to D1 terminal/skip/return products.','No convincing improvement over D1; compare with D1, not only baseline.'),
]
base=lab/'variants/baseline.c'
shutil.copy2(base,archive/'sources/baseline.c')
registry=[]
for name,title,edit,conclusion in specs:
    source=lab/'variants'/f'{name}.c';shutil.copy2(source,archive/'sources'/source.name)
    parent='d1_native' if name=='d3_masked_product' else 'baseline'
    a=(lab/'variants'/f'{parent}.c').read_text().splitlines(True);b=source.read_text().splitlines(True)
    patch=''.join(difflib.unified_diff(a,b,fromfile=f'a/{parent}.c',tofile=f'b/{name}.c'))
    (archive/'patches'/f'{name}.patch').write_text(patch)
    registry.append(dict(variant=name,title=title,edit=edit,parent=parent,source_sha256=sha(source.read_bytes()),parent_sha256=sha((lab/'variants'/f'{parent}.c').read_bytes()),source=f'sources/{name}.c',patch=f'patches/{name}.patch',correctness='accepted pragmatic smoke/lab checks',measured_sources=[1,10,100] if name in ['ds20','ds21','ds22'] else [1,10],incomplete_sources=[] if name in ['ds20','ds21','ds22'] else [100],conclusion=conclusion))
(archive/'variants.json').write_text(json.dumps(registry,indent=2)+'\n')

# Preserve old experiment names/combos without presenting their older controls as current acceptance.
history=collections.defaultdict(lambda:dict(summaries=set(),sources=set(),statuses=collections.Counter(),ratios=[]))
inventory=[]
for path in sorted((lab/'build/runs').rglob('summary.csv')):
    for row in csv.DictReader(path.open()):
        name=row.get('variant')
        if not name or name=='variant':continue
        item=history[name];item['summaries'].add(str(path.relative_to(repo)));item['sources'].add(row.get('source_size','unknown'));item['statuses'][row.get('status','unspecified')]+=1
        inventory.append(dict(variant=name,summary=str(path.relative_to(repo)),graph=row.get('graph',''),grammar=row.get('cnf',row.get('grammar','')),sources=row.get('source_size',''),chunk=row.get('chunk',''),recorded_status=row.get('status',''),ratio=row.get('ratio',row.get('ratio_to_baseline','')),result=row.get('result',''),result_hash=row.get('result_hash','')))
write_csv(archive/'results/all_recorded_summary_rows.csv',inventory)
flat=[]
for name,item in sorted(history.items()):
    kind='control' if name.startswith('baseline') else 'deliberately wrong harness self-test' if name.startswith('_selftest') else 'recent corrected-baseline candidate' if name in {x[0] for x in specs} else 'historical experiment/combination; not reaccepted'
    flat.append(dict(variant=name,kind=kind,summary_files=len(item['summaries']),source_sizes=';'.join(sorted(item['sources'])),recorded_statuses=json.dumps(dict(item['statuses']),sort_keys=True),evidence_paths=json.dumps(sorted(item['summaries']))))
write_csv(archive/'historical_inventory.csv',flat)

# Human-readable catalogue with links to immutable source copies and immediate-parent patches.
lines=['# CFPQ RSM experiment archive — 2026-11-03','','This is an immutable local snapshot outside the ignored build directory. Do not overwrite it; create a dated successor for future measurements. No new benchmark was run to make this archive. No variant has been promoted or combined.','','## Start here','','- [Recent candidate catalogue](#recent-candidate-catalogue): exact source snapshots, parent-specific patches and conclusions.','- [Historical inventory](historical_inventory.csv): every variant/display name recovered from old summary files, including combinations, controls and deliberate wrong-result self-tests.','- [All recorded summary rows](results/all_recorded_summary_rows.csv): retained original status/ratio fields and evidence paths; these are not newly validated results.','- [R10 analysis](results/R10_ANALYSIS.md) and [per-workload 1/10-source results](results/r10_workloads_s1_s10.csv).','- [Completed DS-20/21/22 results](results/ds20_21_22_morning_summary.md) and [CSV](results/ds20_21_22_morning_summary.csv).','- [Machine-readable variant registry](variants.json), [snapshot metadata](context/snapshot.json), `evidence.tar.gz` and its [member manifest](evidence_manifest.jsonl).','','## Recent candidate catalogue','','Every row has passed the previously agreed smoke/lab correctness checks. This records acceptance; it does not reopen testing. Completed DS-20/21/22 timings cover 1/10/100 sources. The other 13 have complete 1/10-source measurements; the interrupted 100-source attempt is archived as partial, not accepted timing data.','','| Variant | Exact change | Saved implementation / diff | Measured conclusion |','| --- | --- | --- | --- |']
for row in registry:
    lines.append(f"| {row['variant']} — {row['title']} | {row['edit']} | [source]({row['source']}) · [diff vs {row['parent']}]({row['patch']}) | {row['conclusion']} |")
lines += ['', '## Baselines, interpretation and scope','','The corrected parent is [baseline.c](sources/baseline.c), SHA-256 `'+sha(base.read_bytes())+'`. D3 alone uses d1_native as its immediate control; both hashes are in variants.json. Retained run provenance binds measured libraries to source and dependency hashes. Ratios are variant/baseline: lower is faster. Compare the duplicate baselines before interpreting small differences. Process peak memory includes adapter/runtime state and is not isolated kernel memory.', '', 'The older I1–I6/S1–S7 names and combinations are preserved in historical_inventory.csv and the full evidence archive, including stage0–stage5, correctness/port checks, DS-00/01 reports and negative-control self-tests. Their baselines, configurations and harness versions differ. Original `ok` fields are historical claims, not an independent review under the corrected baseline; do not pool them with R9/R10 or treat each combination as a standalone ablation. Names alone do not establish exactly which historical component survived; inspect the saved source/patch/provenance.', '', 'Discarded D4/D5 wide label packing is not listed as an implemented optimization. Generic buffer reuse and other discussion-only ideas must not be marked measured. Source snapshots of the advanced CFL implementation/wrapper are archived as research context, not as extra RSM benchmark candidates.', '', '## Benchmark and threading record','','Recent timing settings: all seven configs/lab.csv pairs; sources 1/10/100 planned; 30 chunks, 5 measured rounds, 1 unmeasured warm-up per chunk/display, seed 2026, duplicate baseline displays, --threads 0, uninstrumented timing. Preserve interrupted raw CSVs rather than filling missing rounds. All completed R10 1/10-source comparisons matched result counts and hashes.', '', 'The R10 runner unsets OMP_NUM_THREADS/OMP_THREAD_LIMIT but never recorded actual per-operation threads. On 2026-11-03 an initialization-only probe of the same installed libraries returned OpenMP max 12 and GraphBLAS cap 12; small GraphBLAS calls and serial adapter preparation can still occupy one core. This does not prove the overnight active-thread count. Repeated per-chunk graph/RSM preparation dominates much of elapsed time; see the saved analysis. No 100-source continuation is required or authorized by this archive.', '', '## What evidence.tar.gz preserves','','- All existing lab/build files except regenerable JIT-cache directories and Python bytecode caches: raw CSVs, logs, status/commands, summaries, provenance, source snapshots, generated variants, patches/rejects, profile outputs, build scripts, shared libraries and test executables. This includes partial/failed/interrupted runs and accepted smoke evidence.','- Current lab sources, configs, scripts and tests; reviewer work orders, DeepSeek feedback, current production RSM and advanced CFL/wrapper research sources.','- CFG_bench sources, Makefile, configs, tests sources/reduced fixtures and applicable instructions; selected GraphBLAS/LAGraph initialization/thread-control sources and headers.','- Input/dependency manifests remain in run provenance. Large external graph datasets, external installed dependency binaries and JIT caches are not copied. Their existing paths/hashes are retained where provenance recorded them; this is not a complete dependency installation or remote backup.','', 'Archive paths beginning `repo/` are repository-relative; `external/CFG_bench/` and `external/GraphBLAS/` identify external checkout context. Archived shared objects/executables are provenance artifacts, not portable replacements for the recorded libraries. Do not execute old binaries merely to inspect results.', '', '## Integrity and recovery','','From this directory run `python3 verify.py` to verify SHA256SUMS and every evidence archive member against evidence_manifest.jsonl. This reads data only. List archive members with `tar -tzf evidence.tar.gz`. To restore, extract into a new empty directory, never over a live checkout:','','```bash','mkdir /tmp/cfpq-rsm-archive-restore','tar -xzf evidence.tar.gz -C /tmp/cfpq-rsm-archive-restore','```','','Original absolute paths inside logs/provenance are historical. The archive manifest maps them to saved member paths. Original sources and runs were not moved or deleted. Add this directory to version control or copy it with its checksum file when transferring it; no Git commit or external upload is performed by the snapshot operation.']
(archive/'README.md').write_text('\n'.join(lines)+'\n')

# Store exact review context before bundling, including unstaged modifications, without staging them.
meta=dict(snapshot_date='2026-11-03',captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),repository=str(repo),commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),git_status=subprocess.check_output(['git','status','--short'],cwd=repo,text=True),recent_candidates=len(registry),historical_display_names=len(history),summary_rows=len(inventory),excluded_directory_names=['jit-cache','jit_cache','gb_jit_cache','__pycache__'],notes='Local immutable archive; no benchmark execution, commit, promotion, source mutation or remote backup.')
(archive/'context/snapshot.json').write_text(json.dumps(meta,indent=2)+'\n')
(archive/'context/uncommitted.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=repo))
shutil.copy2(Path(__file__),archive/'context/create_snapshot.py')

# The compact top-level entry point is written before bundling so it too is captured.
(repo/'CFPQ_RSM_OPTIMIZATION_HISTORY.md').write_text('''# CFPQ RSM optimization history

Canonical snapshot: [2026-11-03 experiment archive](experimental/algorithm/cfpq_rsm_lab/archive/2026-11-03/README.md).

The archive preserves all available experiment records: 16 recent optimization variants plus the corrected baseline, historical I/S experiments and combinations, exact sources/parent patches, raw results, logs, commands, correctness reports, profile evidence and checksums. It is outside the ignored build tree; original working files and runs remain untouched.

Recent status: DS-20/21/22 completed 1/10/100-source comparisons and were slower overall. R9/R10 completed all 1/10-source comparisons for 13 candidates with matching counts/hashes; 100-source measurements are partial. config_block and empty/first-contribution handling are promising; joint filtering is Java-specific. No optimization has been promoted or combined based on these results. Inspect the archived workload results rather than treating small timing differences as universal speedups.

Historical experiments used different baselines/harnesses; their recorded statuses must not be reinterpreted as acceptance under the corrected reference. D4/D5 were discarded before implementation. Do not repeat accepted checks or continue 100-source runs merely to fill the archive.

Before a new optimization, consult the catalogue and retain a new immutable snapshot when results change. Full checksum verification and recovery instructions are in the archive README. This is a local archive, not a remote backup or Git commit.
''')

# Build a comprehensive byte-for-byte evidence snapshot, excluding only regenerable caches.
items={}
excluded=[]
exclude_names={'jit-cache','jit_cache','gb_jit_cache','__pycache__','.git'}
def collect(path,prefix):
    if path.is_dir():
        import os
        for current,dirs,files in os.walk(path):
            blocked=[d for d in dirs if d in exclude_names]
            for d in blocked:excluded.append(str(Path(current)/d))
            dirs[:]=[d for d in dirs if d not in exclude_names]
            for name in files:
                p=Path(current)/name
                if p.suffix=='.pyc':continue
                items[prefix+'/'+str(p.relative_to(path))]=p
    elif path.is_file():items[prefix]=path
collect(lab/'build','repo/'+str((lab/'build').relative_to(repo)))
for sub in ['variants','tests','configs','patches']:collect(lab/sub,'repo/'+str((lab/sub).relative_to(repo)))
for p in lab.glob('*'):
    if p.is_file():collect(p,'repo/'+str(p.relative_to(repo)))
for name in ['DEEPSEEK_INSTRUCTIONS.md','DEEPSEEK_FEEDBACK.md','DEEPSEEK_REMAINDER.md','CFPQ_RSM_OPTIMIZATION_HISTORY.md','.gitignore']:
    collect(repo/name,'repo/'+name)
for p in [repo/'experimental/algorithm/LAGraph_CFPQ_RSM.c',repo/'experimental/algorithm/LAGraph_CFL_reachability_multsrc_advanced.c',repo/'experimental/algorithm/LAGraph_CFL_optimized_matrix_opt.c',repo/'experimental/algorithm/LAGraph_CFL_optimized_matrix_opt.h',repo/'src/utility/LAGr_Init.c',repo/'src/utility/LAGraph_SetNumThreads.c',repo/'include/LAGraph.h',repo/'include/LAGraphX.h']:
    collect(p,'repo/'+str(p.relative_to(repo)))
external=Path('/home/hollow/repo/CFG_bench')
for sub in ['src','configs','tests']:
    collect(external/sub,'external/CFG_bench/'+sub)
for name in ['Makefile','AGENTS.md']:
    collect(external/name,'external/CFG_bench/'+name)
for p in [Path('/home/hollow/repo/GraphBLAS/Source/omp/include/GB_nthreads.h'),Path('/home/hollow/repo/GraphBLAS/Source/include/GB_defaults.h')]:
    collect(p,'external/GraphBLAS/'+str(p.relative_to('/home/hollow/repo/GraphBLAS')))
collect(Path('/usr/local/include/suitesparse/GraphBLAS.h'),'external/installed-headers/GraphBLAS.h')
(archive/'context/excluded_cache_directories.json').write_text(json.dumps(sorted(set(excluded)),indent=2)+'\n')
print('Archiving',len(items),'evidence files;',sum(p.stat().st_size for p in items.values())//1024//1024,'MiB before compression',flush=True)
with tarfile.open(archive/'evidence.tar.gz','w:gz',compresslevel=6,format=tarfile.PAX_FORMAT) as tar, (archive/'evidence_manifest.jsonl').open('w') as manifest:
    for member,path in sorted(items.items()):
        data=path.read_bytes()
        info=tarfile.TarInfo(member);info.size=len(data);info.mtime=int(path.stat().st_mtime);info.mode=0o755 if path.stat().st_mode&0o111 else 0o644
        tar.addfile(info,io.BytesIO(data))
        manifest.write(json.dumps(dict(member=member,original_path=str(path),bytes=len(data),sha256=sha(data)))+'\n')
verifier='''#!/usr/bin/env python3
"""Read-only archive and payload verification; does not run benchmark code."""
from pathlib import Path
import hashlib,json,tarfile
root=Path(__file__).resolve().parent
for line in (root/'SHA256SUMS').read_text().splitlines():
    expected,name=line.split('  ',1)
    p=root/name
    assert p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()==expected,name
records={r['member']:r for r in map(json.loads,(root/'evidence_manifest.jsonl').read_text().splitlines())}
seen=set()
with tarfile.open(root/'evidence.tar.gz','r:gz') as tar:
    for member in tar:
        assert member.isfile() and member.name in records,member.name
        assert member.name not in seen,member.name
        record=records[member.name]
        data=tar.extractfile(member).read()
        assert len(data)==record['bytes'] and hashlib.sha256(data).hexdigest()==record['sha256'],member.name
        seen.add(member.name)
assert seen==set(records),'Missing members'
print(f'PASS: snapshot file checksums and {len(seen)} evidence members verified; no benchmark executed.')
'''
(archive/'verify.py').write_text(verifier)
files=sorted(p for p in archive.rglob('*') if p.is_file())
(archive/'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.relative_to(archive)}\n' for p in files))
print('Created',archive,'archive MiB',round((archive/'evidence.tar.gz').stat().st_size/1024/1024,2),'history names',len(history),'summary rows',len(inventory),flush=True)
