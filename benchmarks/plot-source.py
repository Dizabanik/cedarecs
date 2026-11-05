"""Kama-style throughput publication from complete, audited ECS measurements."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import textwrap

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import Patch
from matplotlib.font_manager import FontProperties
from matplotlib.textpath import TextPath
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
P=argparse.ArgumentParser()
P.add_argument('--prefix',default='promotion-v121')
P.add_argument('--output',type=Path,default=ROOT/'promotion/automatic-v121')
P.add_argument('--preview',action='store_true',help='Render the small charts before the full poster and atlas')
A=P.parse_args()
OUT=A.output.resolve()
COLORS={'cedar':'#07866c','cedar-packed':'#07866c','cedar-indexed':'#2a9b95',
        'entt':'#536eae','pico':'#bf9147','flecs':'#bd7278',
        'auto':'#07866c','before':'#a1a9b2','ordinary':'#536eae','concurrent':'#2a9b95'}
NAMES={'cedar':'Cedar ECS','cedar-packed':'Cedar packed','cedar-indexed':'Cedar indexed',
       'entt':'EnTT','pico':'pico_ecs','flecs':'Flecs',
       'auto':'Cedar auto','before':'Cedar v107','ordinary':'Forced ordinary','concurrent':'Forced concurrent'}
TEXT='#18272f'; MUTED='#637583'
plt.rcParams.update({'font.family':'DejaVu Sans','text.color':TEXT,'axes.labelcolor':MUTED,
    'xtick.color':MUTED,'ytick.color':TEXT,'svg.fonttype':'none','pdf.fonttype':42})
BASES=[('BM_CreateEmptyEntities','create_empty'),('BM_CreateEntities','create_components'),
       ('BM_DestroyEntities','destroy_entities'),('BM_AddComponent','add_component'),
       ('BM_RemoveAddComponent','remove_add'),('BM_UnpackNoComponent','read_absent'),
       ('BM_UnpackOneComponent','read_one'),('BM_UnpackTwoComponents','read_two'),
       ('BM_UnpackThreeComponents','read_three')]
FRIENDLY=dict(BASES);FRIENDLY['BM_CreateNoEntities']='empty_world'
INPUTS=set();OUTPUTS=[];PLOTTED=[];PAGES=[]

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(suffix,name):
    path=ROOT/'results'/(A.prefix+'-'+suffix)/name;INPUTS.add(path)
    if name.endswith('.csv'):return list(csv.DictReader(path.open()))
    if name.endswith('.jsonl'):return [json.loads(line) for line in path.read_text().splitlines()]
    return json.loads(path.read_text())
def num(x):
    if x==0:return '0'
    if abs(x)<.01:return f'{x:.3g}'
    if abs(x)<1:return f'{x:.4f}'
    if abs(x)<100:return f'{x:.2f}'
    if abs(x)<10000:return f'{x:,.1f}'
    return f'{x:,.0f}'
def delta(x):return f'{x:+.1f}%'
def short(case):
    for a,b in [('entities=','N='),('threads=','T='),('systems=','sys='),('shuffled=','shuffle='),
                ('1000000','1M'),('100000','100k'),('65536','65,536'),('10000','10k'),('1000','1k')]:
        case=case.replace(a,b)
    return case.replace('/', ' · ').replace(',', ', ')
def series(framework,row,prefix,denominator=1):
    return dict(framework=framework,**{s:float(row[prefix+s+'_ns'])/denominator for s in ('median','p25','p75')})
def entity_cards():
    meta=read('entities','metadata.json');assert 'completed_utc' in meta and meta['cedar_read_mode']=='checked'
    rows={r['benchmark']:r for r in read('entities','summary.csv')};raw=read('entities','raw.jsonl')
    assert len(rows)==28
    order=[(base+'/'+str(n),label,n) for base,label in BASES for n in (1,1024,65536)]
    order.append(('BM_CreateNoEntities','empty_world',0));cards=[]
    assert set(rows)=={x[0] for x in order}
    for key,label,n in order:
        counts={int(r['entities']) for r in raw if r['name']==key};assert len(counts)==1
        visited=counts.pop();denominator=max(1,visited)
        cards.append(dict(dataset='entities',key=key,title=label,parameters=f'N={n:,}' if n else 'world lifecycle',
            input=n,visited=visited,unit='entity-visit' if visited else 'world',reference='cedar',
            series=[series(f,rows[key],f+'_cpu_',denominator) for f in ('cedar','entt','pico','flecs')]))
    return cards
def native_cards(suite):
    meta=read(suite,'metadata.json');assert 'completed_utc' in meta
    rows=read(suite,'summary.csv');assert len(rows)==(50 if suite=='original' else 386)
    INPUTS.add(ROOT/'results'/(A.prefix+'-'+suite)/'raw.jsonl')
    cards=[]
    for r in sorted(rows,key=lambda x:x['case']):
        title,parameters=short(r['case']).split(' · ',1)
        cards.append(dict(dataset=suite,key=r['case'],title=title,parameters=parameters,
            input=int(r['entities']),visited=None,unit='operation',reference='cedar',
            series=[series(f,r,f+'_') for f in ('cedar','flecs')]))
    return cards
def registry_cards():
    meta=read('registry','metadata.json');assert 'completed_utc' in meta
    INPUTS.add(ROOT/'results'/(A.prefix+'-registry')/'raw.jsonl')
    rows=read('registry','summary.csv');assert len(rows)==24
    return [dict(dataset='registry',key=f"{r['threads']}/{r['worlds']}/{r['commands']}",
        title='enqueue_value' if int(r['commands']) else 'command_scope',
        parameters=f"T={r['threads']} · worlds={r['worlds']}",threads=int(r['threads']),worlds=int(r['worlds']),
        unit='operation',reference='auto',
        series=[series(f,r,f+'_') for f in ('auto','before','ordinary','concurrent')]) for r in rows]
def memory_cards():
    meta=read('memory','metadata.json');assert 'completed_utc' in meta and not meta['smoke']
    INPUTS.add(ROOT/'results'/(A.prefix+'-memory')/'raw.jsonl')
    groups={}
    for r in read('memory','summary.csv'):
        groups.setdefault((r['scenario'],int(r['entities']),int(r['step']),r['phase']),{})[r['framework']]=r
    cards=[]
    for (scenario,n,step,phase),group in sorted(groups.items()):
        assert len(group)==5
        for metric,label in [('accounted_storage_delta','accounted_storage'),('footprint_delta','physical_footprint')]:
            cards.append(dict(dataset='memory',key=f'{scenario}/{n}/{step}/{phase}/{metric}',
                title=label,parameters=f'{scenario} · N={n:,} · {phase} · step={step}',
                scenario=scenario,input=n,phase=phase,metric=metric,unit='MiB',reference=None,
                series=[dict(framework=f,**{s:float(group[f][metric+'_'+s])/1048576 for s in ('median','p25','p75')})
                    for f in ('cedar-packed','cedar-indexed','entt','pico','flecs')]))
    return cards
def remember(cards):
    for c in cards:
        ref=next((s['median'] for s in c['series'] if s['framework']==c['reference']),None)
        for s in c['series']:
            assert s['p25']<=s['median']<=s['p75']
            PLOTTED.append(dict(dataset=c['dataset'],case=c['key'],title=c['title'],parameters=c['parameters'],
                unit=c['unit'],**s,mops=None if c['unit']=='MiB' else 1000/s['median'],
                mops_from_timing_p25=None if c['unit']=='MiB' else 1000/s['p75'],
                mops_from_timing_p75=None if c['unit']=='MiB' else 1000/s['p25'],
                reference_throughput_gain_percent=None if ref is None else (s['median']/ref-1)*100))
def heading(fig,title,frameworks,unit='Mop/s ↑',size=24):
    height=fig.get_figheight()
    fig.text(.045,1-.25/height,title,fontsize=size,fontweight='bold',va='top')
    fig.text(.965,1-.30/height,unit,fontsize=11,color=MUTED,ha='right',va='top')
    # Explicit figure coordinates keep long, multi-page PDF legends inside
    # the page independently of a renderer's automatic legend bounding box.
    width=fig.get_figwidth();x=.045;y=1-.86/height
    font=FontProperties(family='DejaVu Sans',size=11)
    for f in frameworks:
        fig.add_artist(plt.Rectangle((x,y-.045/height),.105/width,.105/height,
            transform=fig.transFigure,facecolor=COLORS[f],edgecolor='none'))
        fig.text(x+.17/width,y,NAMES[f],fontsize=11,va='center')
        text_width=TextPath((0,0),NAMES[f],prop=font).get_extents().width/72
        x+=(.17+text_width+.30)/width
def draw(ax,c,small=False):
    ax.set_facecolor('white')
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_xticks([]);ax.tick_params(axis='y',length=0,pad=7)
    is_memory=c['unit']=='MiB';values=[];lo=[];hi=[]
    for s in c['series']:
        values.append(s['median'] if is_memory else 1000/s['median'])
        lo.append(s['p25'] if is_memory else 1000/s['p75'])
        hi.append(s['p75'] if is_memory else 1000/s['p25'])
    span=max(max(hi)-min(0,min(lo)),1e-9)
    left=min(0,min(lo))*1.10
    right=max(0,max(hi))+span*.34
    ax.set_xlim(left,right)
    y=np.arange(len(values))[::-1]
    for pos,s,value,a,b in zip(y,c['series'],values,lo,hi):
        f=s['framework'];color=COLORS[f]
        ax.barh(pos,value,height=.68,color=color,zorder=2)
        ax.plot([a,b],[pos,pos],color='#253b46',alpha=.45,lw=.9,zorder=3)
        ax.text(value+span*.028,pos,num(value),va='center',fontsize=7.8 if small else 10,
            fontweight='bold' if f in ('cedar','auto','cedar-packed') else 'normal',color=color)
    ax.set_yticks(y,[NAMES[s['framework']] for s in c['series']],fontsize=7 if small else 9)
    ax.set_ylim(-.7,len(values)-.3)
    title=c['title']+'\n'+c['parameters']
    ax.set_title('\n'.join(textwrap.wrap(title.split('\n')[0],52))+'\n'+
                 '\n'.join(textwrap.wrap(c['parameters'],65 if small else 48)),
                 loc='left',fontsize=8 if small else 10,fontfamily='DejaVu Sans Mono',pad=12)
def save(fig,name,dpi=170,formats=('png','svg','pdf')):
    for ext in formats:
        path=OUT/(name+'.'+ext);fig.savefig(path,dpi=dpi,facecolor='white');OUTPUTS.append(path)
    plt.close(fig)
def grid(cards,name,title,frameworks,columns=3,unit='Mop/s ↑'):
    rows=math.ceil(len(cards)/columns)
    height=rows*2.7+1.9
    fig,axes=plt.subplots(rows,columns,figsize=(17.2,height),squeeze=False,facecolor='white')
    fig.subplots_adjust(left=.095,right=.977,bottom=.025,top=1-1.8/height,hspace=.70,wspace=.68)
    heading(fig,title,frameworks,unit)
    for ax,c in zip(axes.flat,cards):draw(ax,c)
    for ax in list(axes.flat)[len(cards):]:ax.axis('off')
    save(fig,name)
def registry_overview(cards):
    selected=[c for c in cards if c['threads'] in (4,8) and c['worlds'] in (2,8) and c['title']=='enqueue_value']
    # One global zero-origin throughput scale, following Kama's own overview.
    fig=plt.figure(figsize=(12.0,7.8),facecolor='white')
    ax=fig.add_axes([.25,.065,.69,.68]);heading(fig,'Cedar ECS · concurrent registry',
        ('auto','before','ordinary','concurrent'))
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_xticks([]);ax.set_yticks([])
    ymax=len(selected)*5;maxval=max(1000/s['median'] for c in selected for s in c['series'])
    ax.set_xlim(0,maxval*1.20);ax.set_ylim(-1,ymax)
    for row,c in enumerate(selected):
        center=ymax-row*5-3
        ax.text(-.025,center+.45,c['parameters'],transform=ax.get_yaxis_transform(),ha='right',va='center',
            fontsize=11,fontfamily='DejaVu Sans Mono')
        for i,s in enumerate(c['series']):
            y=center+1.5-i*.83;value=1000/s['median'];color=COLORS[s['framework']]
            ax.barh(y,value,height=.62,color=color)
            ax.text(value+maxval*.018,y,num(value),va='center',color=color,
                fontsize=11,fontweight='bold' if s['framework']=='auto' else 'normal')
        if row<len(selected)-1:ax.axhline(center-2.8,color='#edf0f3',lw=.8)
    save(fig,'concurrent-registry')
def grouped(cards,name,title):
    """One shared linear throughput scale, matching Kama's README figures."""
    height=len(cards)*1.15+1.8
    fig=plt.figure(figsize=(12.0,height),facecolor='white')
    ax=fig.add_axes([.27,.06,.66,1-.06-1.65/height])
    heading(fig,title,('cedar','entt','pico','flecs'),size=25)
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_xticks([]);ax.set_yticks([])
    ymax=len(cards)*5.3
    peak=max(1000/s['p25'] for c in cards for s in c['series'])
    ax.set_xlim(0,peak*1.18);ax.set_ylim(-.6,ymax)
    for row,c in enumerate(cards):
        center=ymax-row*5.3-3
        ax.text(-.026,center+.25,c['title']+'\n'+c['parameters'],
            transform=ax.get_yaxis_transform(),ha='right',va='center',fontsize=11,
            fontfamily='DejaVu Sans Mono',linespacing=1.5)
        for i,s in enumerate(c['series']):
            y=center+1.5-i*.82;value=1000/s['median'];color=COLORS[s['framework']]
            ax.barh(y,value,height=.63,color=color)
            ax.text(value+peak*.018,y,num(value),va='center',color=color,fontsize=11,
                fontweight='bold' if s['framework']=='cedar' else 'normal')
        if row<len(cards)-1:ax.axhline(center-2.8,color='#edf0f3',lw=.8)
    save(fig,name)
def complete_poster(cards):
    columns=4;rows=math.ceil(len(cards)/columns)
    fig,axes=plt.subplots(1,columns,figsize=(27,rows*.50+1.8),facecolor='white')
    fig.subplots_adjust(left=.018,right=.985,bottom=.005,top=.985,wspace=.06)
    # Full poster is a result table with zero-origin bars independently scaled
    # within each case, never truncated absolute bars or logarithmic gaps.
    for col,ax in enumerate(axes):
        ax.set_xlim(0,1);ax.set_ylim(rows,0);ax.axis('off')
        for row,c in enumerate(cards[col*rows:(col+1)*rows]):
            if row%2==0:ax.axhspan(row+.01,row+.99,color='#f6f8f9',zorder=0)
            case=c['dataset'].upper()+' · '+c['title']+' · '+c['parameters']
            ax.text(.015,row+.035,'\n'.join(textwrap.wrap(case,84)),fontsize=6.5,
                fontfamily='DejaVu Sans Mono',va='top',linespacing=1.1)
            maxrate=max(1000/s['median'] for s in c['series'])
            height=.098 if len(c['series'])==4 else .145
            for i,s in enumerate(c['series']):
                y=row+.47+i*(.12 if len(c['series'])==4 else .20)
                f=s['framework'];rate=1000/s['median'];color=COLORS[f]
                ax.barh(y,rate/maxrate*.48,left=.15,height=height,color=color)
                ax.text(.012,y,NAMES[f].replace(' ECS',''),fontsize=5.8,va='center',color=color)
                ax.text(.65,y,num(rate)+' Mop/s',fontsize=6.2,va='center',color=color,
                    fontweight='bold' if f=='cedar' else 'normal')
                ax.text(.84,y,num(s['median'])+' ns/op',fontsize=5.7,va='center',color=TEXT)
    fig.text(.018,.995,'Cedar ECS · complete comparison',fontsize=22,fontweight='bold',va='top')
    fig.text(.985,.995,'Mop/s ↑',fontsize=11,ha='right',va='top',color=MUTED)
    save(fig,'comparison-complete',dpi=145)
def ascii_table(headers,rows):
    text=[[str(x) for x in headers],*[[str(x) for x in row] for row in rows]]
    widths=[max(len(row[i]) for row in text) for i in range(len(headers))]
    rule=lambda left,middle,right:left+middle.join('─'*(w+2) for w in widths)+right
    line=lambda row:'│'+'│'.join(' '+v.ljust(w)+' ' if i==0 else ' '+v.rjust(w)+' '
                               for i,(v,w) in enumerate(zip(row,widths)))+'│'
    return '\n'.join([rule('┌','┬','┐'),line(text[0]),rule('├','┼','┤'),
                      *[line(row) for row in text[1:]],rule('└','┴','┘')])
def text_tables(entities,native,registry):
    selected=[c for c in entities if c['input']==65536]
    content=['# Cedar ECS throughput comparisons','',
        'Rates are `1000 / median ns/op`. Positive Δ favors Cedar; negative favors the peer. '
        'All samples contribute to the median; no best-pass selection. Entity cases use the actual visited-entity counter, '
        'and the empty-world case uses one world. Different operations have different work per operation.','']
    for peer in ('entt','pico','flecs'):
        rows=[]
        for c in entities:
            a=next(s for s in c['series'] if s['framework']=='cedar');b=next(s for s in c['series'] if s['framework']==peer)
            rows.append([c['title']+(f"/{c['input']:,}" if c['input'] else ''),num(1000/a['median']),num(a['median']),
                         num(1000/b['median']),num(b['median']),delta((b['median']/a['median']-1)*100)])
        content+=['## Cedar vs '+NAMES[peer],'','```text',ascii_table(
            ['Benchmark / input','Cedar Mop/s','Cedar ns/op',NAMES[peer]+' Mop/s',NAMES[peer]+' ns/op','Δ vs '+NAMES[peer]],rows),'```','']
    path=OUT/'entity-tables.md';path.write_text('\n'.join(content));OUTPUTS.append(path)
    full=['# Complete native throughput results','',
          'All 822 native C configurations; throughput is reciprocal median operation latency. '
          'Positive Δ favors Cedar. Old/new comparisons use the same inputs, iteration counts, clocks and kernels.','']
    for suite,cards in native.items():
        rows=[]
        for c in cards:
            a,b=c['series'];rows.append([c['key'],num(1000/a['median']),num(a['median']),num(1000/b['median']),
                                         num(b['median']),delta((b['median']/a['median']-1)*100)])
        full+=['## '+suite.capitalize(),'','```text',ascii_table(
            ['Benchmark','Cedar Mop/s','Cedar ns/op','Flecs Mop/s','Flecs ns/op','Δ vs Flecs'],rows),'```','']
    path=OUT/'complete-timings.md';path.write_text('\n'.join(full));OUTPUTS.append(path)
    compact=[]
    for c in selected:
        by={s['framework']:s for s in c['series']};a=by['cedar'];best=min(by[f]['median'] for f in ('entt','pico','flecs'))
        compact.append([c['title'],*[num(1000/by[f]['median']) for f in ('cedar','entt','pico','flecs')],delta((best/a['median']-1)*100)])
    path=OUT/'compact-table.txt';path.write_text(ascii_table(
        ['Benchmark · N=65,536','Cedar Mop/s','EnTT Mop/s','pico Mop/s','Flecs Mop/s','Δ vs best peer'],compact)+'\n');OUTPUTS.append(path)
    return selected
def atlas(sections):
    path=OUT/'benchmark-atlas.pdf'
    with PdfPages(path,metadata={'Title':'Cedar ECS benchmark comparison','Subject':'Complete measured cases; throughput and native memory'}) as pdf:
        for name,cards,frameworks in sections:
            for start in range(0,len(cards),12):
                part=cards[start:start+12]
                fig,axes=plt.subplots(4,3,figsize=(16.0,17.5),squeeze=False,facecolor='white')
                fig.subplots_adjust(left=.10,right=.985,bottom=.035,top=.89,hspace=.87,wspace=.73)
                heading(fig,name,frameworks,'MiB ↓' if part[0]['unit']=='MiB' else 'Mop/s ↑',size=21)
                for ax,c in zip(axes.flat,part):draw(ax,c,small=True)
                for ax in list(axes.flat)[len(part):]:ax.axis('off')
                pdf.savefig(fig,facecolor='white');plt.close(fig)
                PAGES.append(dict(page=len(PAGES)+1,section=name,cases=[c['key'] for c in part]))
            print('Atlas section complete: '+name,flush=True)
    OUTPUTS.append(path)
def main():
    OUT.mkdir(parents=True,exist_ok=False)
    entities=entity_cards();native={s:native_cards(s) for s in ('original','packed','indexed')}
    registry=registry_cards();memory=memory_cards()
    timings=entities+[c for rows in native.values() for c in rows];assert len(timings)==850
    for group in (timings,registry,memory):remember(group)
    selected=text_tables(entities,native,registry)
    grid(selected,'comparison','Cedar ECS comparison',('cedar','entt','pico','flecs'))
    grouped([c for c in selected if c['title'].startswith('read_')],
            'read-throughput','Cedar ECS · read throughput')
    grouped([c for c in selected if not c['title'].startswith('read_')],
            'lifecycle-throughput','Cedar ECS · lifecycle throughput')
    grid(entities,'comparison-entities','Cedar ECS · all entity cases',('cedar','entt','pico','flecs'))
    if not A.preview:complete_poster(timings)
    registry_overview(registry)
    grid(registry,'concurrent-registry-complete','Cedar ECS · all registry cases',('auto','before','ordinary','concurrent'))
    largest=max(c['input'] for c in memory)
    chosen=[c for c in memory if c['input']==largest and c['phase']=='populated' and
            c['metric']=='accounted_storage_delta' and c['scenario'] in
            ('position_velocity','dense_payload','sparse_payload','fragmented')]
    grid(chosen,'memory','ECS memory comparison',('cedar-packed','cedar-indexed','entt','pico','flecs'),columns=2,unit='MiB ↓')
    sections=[('Entity comparison',entities,('cedar','entt','pico','flecs')),
           *[(s.capitalize()+' · Cedar vs Flecs',cards,('cedar','flecs')) for s,cards in native.items()],
           ('Concurrent registry',registry,('auto','before','ordinary','concurrent')),
           ('Memory comparison',memory,('cedar-packed','cedar-indexed','entt','pico','flecs'))]
    if not A.preview:atlas(sections)
    path=OUT/'plotted-values.csv'
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,list(PLOTTED[0]));w.writeheader();w.writerows(PLOTTED)
    OUTPUTS.append(path)
    data=OUT/'data';data.mkdir()
    for suffix in ('entities','original','packed','indexed','registry','first-use','memory','query-memory'):
        for name in ('summary.csv','metadata.json'):
            source=ROOT/'results'/(A.prefix+'-'+suffix)/name;INPUTS.add(source)
            shutil.copy2(source,data/(suffix+'-'+name))
    shutil.copy2(__file__,OUT/'plot-source.py')
    for p in sorted(data.iterdir()):OUTPUTS.append(p)
    source=json.loads((ROOT/'provenance.json').read_text())
    meta=dict(cedar=source['cedarecs'],kama=source['kama'],timing_cases=850,preview=A.preview,
        registry_cases=len(registry),memory_panels=len(memory),atlas_pages=PAGES,
        inputs={str(p.relative_to(ROOT)):sha(p) for p in sorted(INPUTS)},
        outputs={str(p.relative_to(OUT)):sha(p) for p in OUTPUTS},
        plot_source_sha256=sha(Path(__file__)),
        encoding='Kama-style zero-origin horizontal throughput bars. Grid panels and complete-poster cases scale independently from zero; registry overview shares one scale. Mop/s=1000/median ns/op, not a median of reciprocal samples. Entity operations normalize by actual visited counters; empty-world is per world. Whiskers convert latency IQR endpoints. Memory uses linear baseline-delta MiB and lower is better. No best-pass selection or outcome-based case selection. Compact subset: all nine entity operations at the largest input. Losses remain visible.')
    (OUT/'provenance.json').write_text(json.dumps(meta,indent=2)+'\n')
    print('Published complete graphics and throughput tables',flush=True)

if __name__=='__main__':main()
