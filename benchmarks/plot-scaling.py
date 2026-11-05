"""Add entity-scaling charts from the published measurements.

Run with Python and Matplotlib. Existing bar-chart assets are never opened for
writing. Only the files named in OUTPUT_NAMES below are generated.
"""
import csv
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, FuncFormatter, LogLocator, NullFormatter

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parent / 'assets/benchmarks'
SUMMARY = HERE / 'data/entities-summary.csv'
METADATA = HERE / 'data/entities-metadata.json'
OUTPUT_NAMES = ('scaling-read-one.svg', 'scaling-read-one.png',
                'scaling-entities.svg', 'scaling-entities.png')
FRAMEWORKS = ('cedar', 'entt', 'pico', 'flecs')
NAMES = {'cedar': 'Cedar ECS', 'entt': 'EnTT', 'pico': 'pico_ecs', 'flecs': 'Flecs'}
COLORS = {'cedar': '#07866c', 'entt': '#2464d8', 'pico': '#ed941c', 'flecs': '#df5d48'}
MARKERS = {'cedar': 'o', 'entt': '^', 'pico': 's', 'flecs': 'D'}
SIZES = (1, 1024, 65536)
CASES = (
    ('BM_CreateEmptyEntities', 'Create empty entities'),
    ('BM_CreateEntities', 'Create with components'),
    ('BM_DestroyEntities', 'Destroy entities'),
    ('BM_AddComponent', 'Add a component'),
    ('BM_RemoveAddComponent', 'Remove + add a component'),
    ('BM_UnpackNoComponent', 'Read absent component'),
    ('BM_UnpackOneComponent', 'Read one component'),
    ('BM_UnpackTwoComponents', 'Read two components'),
    ('BM_UnpackThreeComponents', 'Read three components'),
)
TEXT = '#2a3f5f'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def time_label(ns, _):
    for scale, suffix in ((1e9, 's'), (1e6, 'ms'), (1e3, 'µs'), (1, 'ns')):
        if ns >= scale * .999:
            return f'{ns / scale:g}{suffix}'
    return f'{ns:g}ns'


def style_axes(ax, small=False):
    ax.set_facecolor('#e5ecf6')
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlim(.75, SIZES[-1] * 1.3)
    ax.xaxis.set_major_locator(FixedLocator(SIZES))
    ax.set_xticklabels(('1', '1,024', '65,536'))
    ax.xaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5)))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_major_locator(LogLocator(base=10, numticks=8))
    ax.yaxis.set_major_formatter(FuncFormatter(time_label))
    ax.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5)))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.grid(which='major', color='white', linewidth=1.1)
    ax.grid(which='minor', color='white', linewidth=.65, alpha=.7)
    ax.set_axisbelow(True)
    ax.tick_params(axis='both', which='both', length=0, pad=8,
                   labelsize=10 if small else 12, colors=TEXT)
    for spine in ax.spines.values():
        spine.set_visible(False)


def draw_case(ax, base, title, small=False):
    style_axes(ax, small)
    all_low, all_high = [], []
    for framework in (*FRAMEWORKS[1:], 'cedar'):
        values = [rows[f'{base}/{n}'] for n in SIZES]
        median = [float(r[f'{framework}_cpu_median_ns']) for r in values]
        low = [float(r[f'{framework}_cpu_p25_ns']) for r in values]
        high = [float(r[f'{framework}_cpu_p75_ns']) for r in values]
        all_low.extend(low)
        all_high.extend(high)
        cedar = framework == 'cedar'
        ax.fill_between(SIZES, low, high, color=COLORS[framework],
                        alpha=.12 if cedar else .07, linewidth=0)
        ax.plot(SIZES, median, color=COLORS[framework],
                linewidth=(2.8 if small else 3.4) if cedar else (1.6 if small else 2),
                marker=MARKERS[framework], markersize=(5 if small else 7) if cedar else (4 if small else 5),
                markeredgecolor='white', markeredgewidth=.7, zorder=5 if cedar else 3)
    ax.set_ylim(min(all_low) / 1.65, max(all_high) * 1.65)
    ax.set_title(title, loc='left', fontsize=13 if small else 18,
                 color=TEXT, pad=15, fontweight='medium')


def legend(fig, **kwargs):
    handles = [Line2D([], [], color=COLORS[f], marker=MARKERS[f],
                      linewidth=3.4 if f == 'cedar' else 2,
                      markersize=7 if f == 'cedar' else 5,
                      label=NAMES[f]) for f in FRAMEWORKS]
    result = fig.legend(handles=handles, frameon=False, labelcolor=TEXT, **kwargs)
    result.get_texts()[0].set_weight('bold')


def save(fig, stem):
    for extension in ('svg', 'png'):
        path = ASSETS / f'{stem}.{extension}'
        assert path.name in OUTPUT_NAMES
        metadata = {'Date': None} if extension == 'svg' else {}
        fig.savefig(path, dpi=180, facecolor='white', metadata=metadata)
    plt.close(fig)


plt.rcParams.update({'font.family': 'DejaVu Sans', 'text.color': TEXT,
                     'axes.labelcolor': TEXT, 'svg.fonttype': 'none',
                     'svg.hashsalt': 'cedar-entity-scaling-v125'})
with SUMMARY.open(newline='') as source:
    records = list(csv.DictReader(source))
rows = {r['benchmark']: r for r in records}
expected = {f'{base}/{n}' for base, _ in CASES for n in SIZES}
assert len(records) == len(rows) == 28
assert set(rows) == expected | {'BM_CreateNoEntities'}
metadata = json.loads(METADATA.read_text())
assert metadata['repeats'] == 32 and metadata['cedar_read_mode'] == 'checked'
points = []
for base, title in CASES:
    for n in SIZES:
        for framework in FRAMEWORKS:
            r = rows[f'{base}/{n}']
            median, low, high = (float(r[f'{framework}_cpu_{stat}_ns'])
                                 for stat in ('median', 'p25', 'p75'))
            assert 0 < low <= median <= high
            points.append(dict(benchmark=f'{base}/{n}', title=title, entity_input=n,
                               framework=framework, median_cpu_ns=median,
                               p25_cpu_ns=low, p75_cpu_ns=high))

fig, ax = plt.subplots(figsize=(12, 7.5))
fig.subplots_adjust(left=.105, right=.79, bottom=.13, top=.9)
draw_case(ax, 'BM_UnpackOneComponent', 'Read one component')
ax.set_xlabel('Entities (input)', fontsize=13, labelpad=13)
ax.set_ylabel('CPU time per iteration', fontsize=13, labelpad=13)
legend(fig, loc='upper left', bbox_to_anchor=(.805, .9), fontsize=12,
       title='Frameworks', title_fontsize=13, handlelength=2.5, labelspacing=.85)
save(fig, 'scaling-read-one')

fig, axes = plt.subplots(3, 3, figsize=(15, 12))
fig.subplots_adjust(left=.08, right=.975, bottom=.08, top=.88, hspace=.35, wspace=.31)
fig.text(.08, .96, 'Entity scaling', fontsize=22, fontweight='medium', va='top')
legend(fig, loc='upper right', bbox_to_anchor=(.975, .965), ncol=4,
       fontsize=12, handlelength=2.5, columnspacing=1.8)
for ax, (base, title) in zip(axes.flat, CASES):
    draw_case(ax, base, title, small=True)
fig.supxlabel('Entities (input)', fontsize=13, y=.022)
fig.supylabel('CPU time per iteration', fontsize=13, x=.014)
save(fig, 'scaling-entities')

values_path = HERE / 'scaling-values.csv'
with values_path.open('w', newline='') as out:
    writer = csv.DictWriter(out, fieldnames=list(points[0]))
    writer.writeheader()
    writer.writerows(points)
outputs = [*(ASSETS / name for name in OUTPUT_NAMES), values_path]
provenance = dict(
    measured_source_sha256=metadata['cedar']['snapshot_sha256'],
    input_files={str(p.relative_to(HERE)): sha(p) for p in (SUMMARY, METADATA)},
    generator_sha256=sha(Path(__file__)),
    outputs={str(p.relative_to(HERE.parent)): sha(p) for p in outputs},
    metric='Median CPU nanoseconds per original benchmark iteration, without per-entity normalization',
    axes='Logarithmic x and y; each overview panel has its own y limits',
    markers='Only the three measured entity inputs: 1, 1024, 65536',
    lines='Straight connections between measured points; no additional sizes are estimated',
    bands='Observed 25th to 75th timing percentiles; not confidence intervals',
    frameworks=FRAMEWORKS, source_process_rounds=metadata['repeats'],
    overview_cases=len(CASES) * len(SIZES), unique_plotted_points=len(points),
    omitted_case='BM_CreateNoEntities has no positive entity input; retained in the existing 28-case charts',
    matplotlib_version=matplotlib.__version__,
)
(HERE / 'scaling-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(f'Created {len(OUTPUT_NAMES)} chart files from {len(points)} measured points.')
