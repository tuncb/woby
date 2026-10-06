"""Validate comparisons before reporting parser performance."""
import math
import statistics


def summarize(records, models, variants, rounds):
    summaries=[]
    for model in models:
        rows=[r for r in records if r['model']==model]
        if not rows or len({r['fingerprint'] for r in rows})!=1:
            raise ValueError('Geometry mismatch or missing model: '+model)
        result=dict(model=model,variants={})
        for variant in variants:
            group=[r for r in rows if r['variant']==variant]
            if len(group)!=rounds or {r['round'] for r in group}!=set(range(1,rounds+1)):
                raise ValueError(f'Missing or repeated rounds: {model}/{variant}')
            metrics={}
            for key in group[0]:
                if not (key.endswith('_ms') or key.endswith('_ms_sum') or key in ('workers','chunks')):
                    continue
                values=[r[key] for r in group]
                if any(not math.isfinite(v) or v<0 for v in values):
                    raise ValueError('Invalid measurement: '+key)
                metrics[key]=dict(median=statistics.median(values),minimum=min(values),maximum=max(values))
            result['variants'][variant]=metrics
        summaries.append(result)
    return summaries


def regressions(summaries, relative_limit=1.05, absolute_allowance_ms=.1):
    result=[]
    for row in summaries:
        reference=row['variants']['legacy']['total_ms']['median']
        candidate=row['variants']['prototype']['total_ms']['median']
        if candidate>max(reference*relative_limit, reference+absolute_allowance_ms):
            result.append(dict(model=row['model'],legacy_ms=reference,prototype_ms=candidate,ratio=candidate/reference))
    return result
