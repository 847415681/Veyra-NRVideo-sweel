"""Summarise the three recorded windows; no product/device/settings mutations.

Each latency field is already a per-report P95. Its median here is explicitly
the median of those reported P95s, not a pooled frame percentile.
"""
from pathlib import Path
import argparse,datetime,hashlib,json,re,statistics

parser=argparse.ArgumentParser()
parser.add_argument('--log',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
windows={'4k-off':('2026-10-12T14:14:29','2026-10-12T14:15:06'),
         '4k-nr-xess':('2026-10-12T14:16:00','2026-10-12T14:16:23'),
         '2k-off':('2026-10-12T14:17:25','2026-10-12T14:17:55')}
channels=('capture-timing','frame-rate','content-dedup','provider-flow','frame-flow','display-latency','capture-driver-age','present-cost','present-contract')
samples={name:{tag:[] for tag in channels} for name in windows}
pattern=re.compile(r'([A-Za-z0-9]+)=(-?[0-9]+(?:\.[0-9]+)?)(?=\s|$)')
for line in args.log.open(encoding='utf8',errors='replace'):
    if len(line)<24:continue
    time=line[:19]
    name=next((n for n,(a,b) in windows.items() if a<=time<=b),None)
    if name is None:continue
    tag=next((t for t in channels if '['+t+']' in line),None)
    if tag is None:continue
    fields={k:float(v) for k,v in pattern.findall(line)}
    fields['timestamp']=line[:24]
    samples[name][tag].append(fields)

def distribution(values):
    return dict(n=len(values),min=min(values),median=statistics.median(values),max=max(values)) if values else None

def rate(rows,key):
    rows=[r for r in rows if key in r]
    if len(rows)<2:return None
    span=(datetime.datetime.fromisoformat(rows[-1]['timestamp'].replace('Z','+00:00'))-
          datetime.datetime.fromisoformat(rows[0]['timestamp'].replace('Z','+00:00'))).total_seconds()
    if span<=0 or rows[-1][key]<rows[0][key]:return None
    return dict(first=rows[0][key],last=rows[-1][key],seconds=span,perSecond=(rows[-1][key]-rows[0][key])/span)

results={}
for name,tags in samples.items():
    results[name]=dict(window=windows[name],channels={})
    for tag,rows in tags.items():
        keys=sorted(set().union(*(r.keys() for r in rows))-{'timestamp'}) if rows else []
        results[name]['channels'][tag]=dict(count=len(rows),fields={key:distribution([r[key] for r in rows if key in r]) for key in keys})
    results[name]['counterRates']={k:rate(tags['capture-timing'],k) for k in ('received','processed','dropped','displaySubmits')}
output=dict(source=str(args.log.resolve()),sha256=hashlib.sha256(args.log.read_bytes()).hexdigest(),
    scope='Historical log windows aligned to MOV creation times; not a new optical measurement. Median of per-log P95 fields is not a pooled percentile. Driver timestamp age is not independently calibrated hardware latency.',windows=results)
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(output,ensure_ascii=False,indent=2),'utf8')
for name,result in results.items():
    f=result['channels']['capture-timing']['fields']
    print(name,json.dumps({k:f[k]['median'] for k in ('revision','callbackToPresentReturnP95Ms','gpuReadyP95Ms','gpuNrP95Ms','gpuFlowP95Ms','schedulingWaitP95Ms','readAgeMs')},ensure_ascii=False),result['counterRates'])
