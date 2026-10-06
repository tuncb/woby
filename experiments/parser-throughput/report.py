# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.10"]
# ///
"""Publish parser medians/ranges and a standalone comparison chart."""
import argparse
import csv
import json
from pathlib import Path

import analysis


LABELS = {
    'tiny': 'One triangle', 'quads': '250,000 quads', 'lines': '200k positions + lines/points',
    'bus': 'BusGameMap', 'cloud': '10M positions + 10M point records', 'bennu': 'Bennu',
    'powerplant': 'Powerplant', 'negative_seams': '50k triangles, negative attribute indices',
    **{f'positions_{n}': f'{n:,} positions, scientific notation' for n in (4096,16384,65536,262144)},
}


def publish(source, destination):
    data=json.loads(source.read_text(encoding='utf-8'))
    models=[row['id'] for row in data['inputs']]
    variants=list(dict.fromkeys(row['variant'] for row in data['records']))
    data['summaries']=analysis.summarize(data['records'],models,variants,data['rounds'])
    data['regressions']=analysis.regressions(data['summaries'])
    destination.mkdir(parents=True,exist_ok=True)
    (destination/'measurements.json').write_text(json.dumps(data,indent=2),encoding='utf-8')
    with (destination/'timings.csv').open('w',newline='',encoding='utf-8') as stream:
        writer=csv.writer(stream)
        writer.writerow(['model','variant','metric','median','minimum','maximum'])
        for row in data['summaries']:
            for variant,metrics in row['variants'].items():
                for key,value in metrics.items():
                    writer.writerow([row['model'],variant,key,value['median'],value['minimum'],value['maximum']])

    def timing(value):
        return f"{value['median']:.2f} ({value['minimum']:.2f}–{value['maximum']:.2f})"

    table=['| Model | Legacy ms | Previous prototype ms | Fixed prototype ms | Fixed / legacy |',
           '| --- | ---: | ---: | ---: | ---: |']
    for row in data['summaries']:
        values=row['variants']
        legacy=values['legacy']['total_ms']
        candidate=values['prototype']['total_ms']
        before=timing(values['before']['total_ms']) if 'before' in values else '—'
        table.append(f"| {LABELS[row['model']]} | {timing(legacy)} | {before} | {timing(candidate)} | {candidate['median']/legacy['median']:.3f}× |")
    (destination/'results.md').write_text('\n'.join(table)+'\n',encoding='utf-8')

    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig,ax=plt.subplots(figsize=(10,6.7),layout='constrained')
    for offset,variant,label,color in ((-.15,'before','Previous prototype','#b65b4c'),(.15,'prototype','Fixed prototype','#157b8a')):
        present=[(i,row) for i,row in enumerate(data['summaries']) if variant in row['variants']]
        ax.scatter([row['variants'][variant]['total_ms']['median']/row['variants']['legacy']['total_ms']['median'] for _,row in present],
                   [i+offset for i,_ in present],label=label,color=color,s=45,zorder=3)
    ax.axvline(1,color='#58616a',linewidth=1)
    ax.set_yticks(range(len(models)),[LABELS[model] for model in models])
    ax.invert_yaxis()
    ax.set_xlim(left=0)
    ax.set_xlabel('Median parser wall time / legacy wall time (lower is faster)')
    ax.set_title(f"Parser throughput · {data['rounds']} fresh processes per variant/model",loc='left',weight='bold',pad=18)
    ax.grid(axis='x',alpha=.2)
    ax.spines[['top','right','left']].set_visible(False)
    ax.legend(loc='upper right',frameon=False)
    fig.savefig(destination/'comparison.svg')
    fig.savefig(destination/'comparison.png',dpi=170)
    plt.close(fig)
    print(json.dumps(dict(records=len(data['records']),regressions=data['regressions'])))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('destination',type=Path)
    args=parser.parse_args()
    publish(args.source,args.destination)
