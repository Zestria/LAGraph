#!/usr/bin/env python3
"""Plot the completed R13 experiment using the reference bars.svg layout.

Bars: median of 30 source-chunk medians; each chunk median uses five rounds.
Whiskers: minimum and maximum of those 30 chunk medians (not confidence bounds).
The SVG retains editable text. Warm-up measurements are not in the input CSVs.
"""
from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import statistics

os.environ.setdefault('MPLCONFIGDIR', '/tmp/lagraph-cfpq-matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
import numpy as np

LAB = Path(__file__).resolve().parents[2]
DEFAULT_RUN = LAB / 'build/runs/current-vs-bb7-2026-11-04_00-17-29'
GRAPHS = ('go_hierarchy', 'eclass', 'go', 'init', 'mm', 'block', 'batik', 'avrora')
SERIES = (
    ('current', 'Optimized RSM', '#E4572E'),
    ('baseline', 'Baseline RSM', '#2E86AB'),
    ('CFL_multsrc_adv', 'CFL_multsrc_adv', '#3BB273'),
)


def load_data(run: Path):
    chunks = {}
    for series, _, _ in SERIES:
        for round_id in range(5):
            path = run / 'raw' / series / f'round{round_id}.csv'
            with path.open() as source:
                rows = list(csv.DictReader(source))
            assert len(rows) == 240, f'{path}: expected 240 rows'
            seen = set()
            for row in rows:
                graph = Path(row['graph']).stem
                chunk = int(row['chunk'])
                assert graph in GRAPHS and row['source_size'] == '100'
                assert 0 <= chunk < 30
                assert (graph, chunk) not in seen, f'{path}: duplicate case'
                seen.add((graph, chunk))
                expected_algo = 'CFL_multsrc_adv' if series == 'CFL_multsrc_adv' else 'CFL_CFPQ_RSM'
                assert row['algo_name'] == expected_algo
                value = float(row['time_s'])
                assert np.isfinite(value) and value > 0
                chunks.setdefault((series, graph, chunk), {})[round_id] = value
    data = {}
    for series, _, _ in SERIES:
        for graph in GRAPHS:
            values = []
            for chunk in range(30):
                rounds = chunks[(series, graph, chunk)]
                assert set(rounds) == set(range(5))
                values.append(statistics.median(rounds.values()))
            data[(series, graph)] = np.array(values)
    return data


def plot_summary(path: Path, output: Path):
    """Use the existing runner's chunk summaries; no historical layout assumptions."""
    import math
    with path.open() as f:
        rows = list(csv.DictReader(f))
    if not rows or any(r['status'] != 'ok' for r in rows):
        raise SystemExit('refusing to plot an empty/failed/incomplete comparison')
    groups = {}
    keys = {}
    for row in rows:
        if not math.isfinite(float(row['median_s'])) or float(row['median_s']) <= 0:
            raise SystemExit('invalid timing in summary')
        name = row['variant']
        case = (row['graph'],row['cnf'],row['source_size'],row['chunk'])
        if case in keys.setdefault(name,set()): raise SystemExit('duplicate summary case')
        keys[name].add(case)
        groups.setdefault((name,Path(row['graph']).stem,row['source_size']),[]).append(float(row['median_s']))
    if any(v != next(iter(keys.values())) for v in keys.values()):
        raise SystemExit('variant case sets differ')
    labels = sorted({(g,size) for _,g,size in groups})
    names = list(keys)
    fig,ax = plt.subplots(figsize=(max(8,len(labels)*1.5),5))
    width = .8/len(names)
    for i,name in enumerate(names):
        med=[];lo=[];hi=[]
        for graph,size in labels:
            v=groups[(name,graph,size)];m=statistics.median(v)
            med.append(m);lo.append(m-min(v));hi.append(max(v)-m)
        ax.bar(np.arange(len(labels))+(i-(len(names)-1)/2)*width,med,width,label=name,yerr=[lo,hi],capsize=2)
    ax.set_xticks(np.arange(len(labels)),[f'{g}\ns={size}' for g,size in labels])
    ax.set_ylabel('Completed kernel seconds; median of chunk medians')
    smoke=any(int(r['n_runs'])<5 for r in rows)
    ax.set_title('SMOKE CHECK — performance not established' if smoke else 'Pinned variant comparison')
    ax.legend();fig.tight_layout();output.parent.mkdir(parents=True,exist_ok=True)
    for ext in ('svg','png'):
        dest=output.with_suffix('.'+ext);fig.savefig(dest,dpi=160);print(dest)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, help='plot an arbitrary accepted run.py summary (active lab mode)')
    parser.add_argument('--run', type=Path, default=DEFAULT_RUN)
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'bars')
    args = parser.parse_args()
    if args.summary is not None:
        return plot_summary(args.summary, args.output)
    data = load_data(args.run)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    plt.rcParams.update({
        'font.family': 'DejaVu Sans',
        'font.size': 13,
        'axes.titlesize': 16,
        'svg.fonttype': 'none',
        'axes.unicode_minus': False,
    })
    fig, ax = plt.subplots(figsize=(11.2, 8.1))
    fig.subplots_adjust(left=0.17, right=0.97, top=0.855, bottom=0.145)
    values = np.concatenate(list(data.values()))
    xmin = float(values.min()) / 1.7
    xmax = float(values.max()) * 1.6
    ax.set_xscale('log')
    ax.set_xlim(xmin, xmax)
    step = 0.245
    table = []
    for graph_id, graph in enumerate(GRAPHS):
        for series_id, (series, label, color) in enumerate(SERIES):
            v = data[(series, graph)]
            median = float(np.median(v))
            low, high = float(v.min()), float(v.max())
            y = graph_id + (series_id - 1) * step
            ax.barh(y, median - xmin, left=xmin, height=step * 0.83,
                    color=color, edgecolor='none', zorder=2)
            ax.errorbar(median, y, xerr=[[median - low], [high - median]],
                        fmt='none', ecolor='#202020', elinewidth=1.15,
                        capsize=3, capthick=1.1, zorder=3)
            table.append({'graph': graph, 'algorithm': label, 'source_size': 100,
                          'chunks': 30, 'measured_rounds': 5,
                          'median_s': f'{median:.12f}',
                          'min_chunk_median_s': f'{low:.12f}',
                          'max_chunk_median_s': f'{high:.12f}'})

    ax.set_yticks(range(len(GRAPHS)), GRAPHS)
    ax.set_ylim(len(GRAPHS) - 0.5, -0.6)
    ax.set_title('|S| = 100', fontweight='bold', pad=15)
    ax.set_xlabel('Time (seconds, logarithmic scale)', labelpad=10)
    ax.tick_params(axis='y', length=0, pad=10)
    ax.tick_params(axis='x', which='major', labelsize=12)
    ax.grid(axis='x', which='major', color='#bec4ca', alpha=0.65,
            linestyle='--', linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for side in ('top', 'right'):
        ax.spines[side].set_visible(False)
    for side in ('left', 'bottom'):
        ax.spines[side].set_color('#454b50')
    handles = [Patch(facecolor=color, edgecolor='none', label=label)
               for _, label, color in SERIES]
    fig.legend(handles=handles, loc='upper center', bbox_to_anchor=(0.56, 0.995),
               ncol=3, frameon=False, handlelength=2.4, columnspacing=2.0)
    fig.text(0.57, 0.925, '30 source chunks  ·  5 measured rounds + 1 warm-up',
             ha='center', color='#535a61', fontsize=11)
    fig.text(0.17, 0.055,
             'Bars: median of chunk medians. Whiskers: min–max across 30 chunks.',
             ha='left', color='#535a61', fontsize=10)
    fig.text(0.17, 0.030,
             'Baseline: bb7dc96  ·  Optimized: current RSM  ·  CFL: all optimizations enabled',
             ha='left', color='#535a61', fontsize=10)
    for extension in ('svg', 'png'):
        output = args.output.with_suffix('.' + extension)
        fig.savefig(output, dpi=240, facecolor='white', bbox_inches='tight', pad_inches=0.12)
        print(output.resolve())
    plt.close(fig)
    with args.output.with_name(args.output.name + '_data.csv').open('w', newline='') as target:
        writer = csv.DictWriter(target, fieldnames=list(table[0]))
        writer.writeheader()
        writer.writerows(table)


if __name__ == '__main__':
    main()
