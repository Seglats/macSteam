"""Long unattended fit of the full model (render2) to Dolby on the stress test, over every head.

Stages (resumable: state in fit2-state.json, every evaluation in fit2-log.jsonl):
  1. screen: every head once at neutral knobs
  2. short:  CMA-ES, a few generations, for the best screened heads, every average and dummy head
  3. long:   CMA-ES with restarts (larger population each time) on the best few, until the time runs out

Cost: every measure in every section divided by its hearing threshold, averaged (each section and
measure counts the same), plus a quarter of the worst one so no section is given up for the others.
The ball test is never fitted: its score is shown for each new best, as a check against overfitting.

usage: fit2.py [hours]          status: fit2-status.txt"""
import glob
import json
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
HEADS = sorted(glob.glob(os.path.join(HERE, 'heads', '*.head')))
STATE = os.path.join(HERE, 'fit2-state.json')
LOG = os.path.join(HERE, 'fit2-log.jsonl')
STATUS = os.path.join(HERE, 'fit2-status.txt')
FILES = os.path.expanduser('~/Library/Application Support/SteamDriver/surround-test/files')
WORKERS = 17

#        name        low     high    neutral  log scale
KNOBS = [('itd',      0.7,    1.4,    1.0,     0),
         ('ild',      0.6,    1.6,    1.0,     0),
         ('ild_lo',   0.3,    1.6,    1.0,     0),
         ('ild_hi',   0.5,    2.2,    1.0,     0),
         ('contrast', 0.2,    1.5,    1.0,     0),
         ('smooth',   0.0,    1.0,    0.0,     0),
         ('xover',    0.0,    300,    0.0,     0),
         ('eq',       0.0,    1.6,    1.0,     0),
         ('lo_g',     -8,     8,      0.0,     0),
         ('lo_f',     60,     600,    200,     1),
         ('hi_g',     -10,    6,      0.0,     0),
         ('hi_f',     2000,   14000,  6000,    1),
         ('front',    20,     60,     40,      0),
         ('side',     70,     120,    90,      0),
         ('back',     110,    170,    140,     0),
         ('el_front', -20,    40,     0,       0),
         ('el_side',  -20,    40,     0,       0),
         ('el_back',  -20,    40,     0,       0),
         ('g_side',   -6,     6,      0,       0),
         ('g_back',   -6,     6,      0,       0),
         ('d_side',   0,      5,      0,       0),
         ('d_back',   0,      5,      0,       0),
         ('early',    0,      1,      0.1,     0),
         ('tail',     0,      2,      0.3,     0),
         ('room_lp',  1000,   14000,  4000,    1),
         ('tail_lp',  1000,   14000,  8000,    1),
         ('rt',       0.1,    1.0,    0.3,     0),
         ('tail_ms',  5,      60,     20,      0)]
TARGET = {'left/right dB': 1, 'tone by direction dB': 1, 'timing ms': 0.02, 'level spread dB': 1, 'room dB': 1,
          'moving left/right dB': 1}


def to_unit(v):
    out = []
    for x, (_, lo, hi, _, lg) in zip(v, KNOBS):
        out.append((np.log(x / lo) / np.log(hi / lo)) if lg else (x - lo) / (hi - lo))
    return np.clip(out, 0, 1)


def from_unit(u):
    out = []
    for x, (_, lo, hi, _, lg) in zip(np.clip(u, 0, 1), KNOBS):
        out.append(lo * (hi / lo) ** x if lg else lo + x * (hi - lo))
    return out


def args(v):
    return ['%s=%.6g' % (k[0], x) for k, x in zip(KNOBS, v)]


# ---- worker side ----
_G = _SRC = _BALL = None


def _init():
    global _G, _SRC, _BALL
    os.nice(10)
    sys.path.insert(0, HERE)
    import score_stress as S
    import score
    ref = S.load(os.path.join(HERE, 'stress-dolby-on.f32'))
    _SRC = S.source_mono()
    _G = S.features(ref, S.align(ref, _SRC)[0])
    _BALL = score.ref()


def cost_of(rows):
    terms = {}
    for sec, d in rows.items():
        for k, t in TARGET.items():
            if k in d:
                terms[f'{sec}/{k}'] = d[k] / t
    terms['overall tone'] = float(np.abs(rows['overall tone']['offset per octave']).mean())
    v = np.array(list(terms.values()))
    return float(v.mean() + 0.25 * v.max()), terms


def evaluate(job):
    head, v, keep = job
    import score_stress as S
    fd, out = tempfile.mkstemp(suffix='.f32', dir=HERE)
    os.close(fd)
    try:
        subprocess.run([os.path.join(HERE, 'render2'), head, out] + args(v), check=True, cwd=HERE,
                       stdout=subprocess.DEVNULL)
        sig = S.load(out)
        rows = S.compare(S.features(sig, S.align(sig, _SRC)[0]), _G)
        cost, terms = cost_of(rows)
        summ = S.summary(rows)
        summ['room dB'] = float(np.mean([r['room dB'] for r in rows.values() if 'room dB' in r]))
        summ['overall tone dB'] = terms['overall tone']
        worst = max(terms, key=terms.get)
        res = {'head': os.path.basename(head)[:-5], 'v': [float(x) for x in v], 'cost': cost, 'summary': summ,
               'worst': [worst, terms[worst]]}
        if keep:
            res['ball'] = ball_check(head, v)
        return res
    finally:
        os.unlink(out)


def ball_check(head, v):
    import score
    fd, out = tempfile.mkstemp(suffix='.f32', dir=HERE)
    os.close(fd)
    try:
        subprocess.run([os.path.join(HERE, 'render2'), head, out, 'ball'] + args(v), check=True, cwd=HERE)
        d = score.compare(score.features(out), _BALL)
        return {k: float(x) for k, x in d.items()}
    finally:
        os.unlink(out)


# ---- driver ----
class Run:
    def __init__(self, hours):
        self.deadline = time.time() + hours * 3600
        self.state = json.load(open(STATE)) if os.path.exists(STATE) else {'screen': {}, 'short': {}, 'long': {}, 'best': None}
        self.pool = ProcessPoolExecutor(WORKERS, initializer=_init)
        self.started = time.time()
        self.evals = 0
        self.stage = ''

    def save(self):
        json.dump(self.state, open(STATE + '.tmp', 'w'))
        os.replace(STATE + '.tmp', STATE)

    def run_jobs(self, jobs):
        res = list(self.pool.map(evaluate, jobs))
        with open(LOG, 'a') as f:
            for r in res:
                f.write(json.dumps(r) + '\n')
        self.evals += len(res)
        for r in res:
            b = self.state['best']
            if b is None or r['cost'] < b['cost']:
                self.state['best'] = r
                self.new_best = True
        return res

    def check_best(self, force=False):
        if getattr(self, 'new_best', False) and (force or time.time() - getattr(self, 'last_check', 0) > 600):
            self.last_check = time.time()
            self.new_best = False
            b = self.state['best']
            b['ball'] = self.pool.submit(evaluate, (os.path.join(HERE, 'heads', b['head'] + '.head'), b['v'], True)).result()['ball']
            self.save()
            self.write_best_wav()

    def write_best_wav(self):
        b = self.state['best']
        out = os.path.join(HERE, 'best2.f32')
        subprocess.run([os.path.join(HERE, 'render2'), os.path.join(HERE, 'heads', b['head'] + '.head'), out] + args(b['v']),
                       check=True, cwd=HERE)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'f32le', '-ar', '48000', '-ac', '2', '-i', out,
                        '-c:a', 'pcm_s24le', os.path.join(FILES, 'stress-fit2-best.wav')], check=True)

    def status(self, extra=''):
        b = self.state['best']
        el = time.time() - self.started
        lines = [time.strftime('%H:%M:%S') + f'  stage: {self.stage}  {extra}',
                 f'evaluations this run: {self.evals}  ({el / 3600:.2f} h, {self.evals / max(el, 1) * 60:.0f}/min); '
                 f'time left: {(self.deadline - time.time()) / 3600:.2f} h']
        if b:
            lines += [f'best: cost {b["cost"]:.3f}  head {b["head"]}',
                      '  ' + '  '.join(f'{k} {v:.2f}' for k, v in b['summary'].items()),
                      f'  worst: {b["worst"][0]} {b["worst"][1]:.2f} x threshold',
                      '  ball check: ' + ('  '.join(f'{k} {v:.2f}' for k, v in b.get('ball', {}).items()) or '-'),
                      '  knobs: ' + ' '.join(args(b['v']))]
        if self.state['short']:
            top = sorted(self.state['short'].items(), key=lambda kv: kv[1]['cost'])[:8]
            lines.append('heads after the short fits: ' + ', '.join(f'{h} {r["cost"]:.2f}' for h, r in top))
        elif self.state['screen']:
            top = sorted(self.state['screen'].items(), key=lambda kv: kv[1])[:8]
            lines.append('screened heads: ' + ', '.join(f'{h} {c:.2f}' for h, c in top))
        open(STATUS, 'w').write('\n'.join(lines) + '\n')

    def cma(self, head, x0, sigma, popsize, gens, tag):
        import cma
        es = cma.CMAEvolutionStrategy(list(to_unit(x0)), sigma, {'bounds': [0, 1], 'popsize': popsize, 'verbose': -9,
                                                                   'seed': int(time.time()) % 100000})
        best = None
        for g in range(gens):
            if time.time() > self.deadline:
                break
            us = es.ask()
            res = self.run_jobs([(head, from_unit(u), False) for u in us])
            es.tell(us, [r['cost'] for r in res])
            for r in res:
                if best is None or r['cost'] < best['cost']:
                    best = r
            self.check_best()
            self.status(f'{tag} {os.path.basename(head)[:-5]} gen {g + 1}/{gens} best here {best["cost"]:.3f} sigma {es.sigma:.3f}')
            if es.stop():
                break
        return best

    def go(self):
        neutral = [k[3] for k in KNOBS]
        # 1. screen
        self.stage = 'screen'
        todo = [h for h in HEADS if os.path.basename(h)[:-5] not in self.state['screen']]
        for i in range(0, len(todo), 68):
            res = self.run_jobs([(h, neutral, False) for h in todo[i:i + 68]])
            for h, r in zip(todo[i:i + 68], res):
                self.state['screen'][os.path.basename(h)[:-5]] = r['cost']
            self.save()
            self.check_best()
            self.status(f'{len(self.state["screen"])}/{len(HEADS)} heads')
        self.check_best(True)
        # 2. short fits
        self.stage = 'short'
        ranked = sorted(self.state['screen'], key=self.state['screen'].get)
        chosen = ranked[:20] + [h for h in ranked if h.endswith('-avg') or h.startswith('mit-') or '-D' in h]
        chosen = list(dict.fromkeys(chosen))
        for h in chosen:
            if h in self.state['short'] or time.time() > self.deadline:
                continue
            r = self.cma(os.path.join(HERE, 'heads', h + '.head'), neutral, 0.25, 17, 14, 'short')
            if r:
                self.state['short'][h] = {'cost': r['cost'], 'v': r['v']}
                self.save()
                self.check_best(True)
        # 3. long fits with restarts on the best few
        self.stage = 'long'
        top = sorted(self.state['short'], key=lambda h: self.state['short'][h]['cost'])[:4]
        pop = 17
        while time.time() < self.deadline:
            for h in top:
                if time.time() > self.deadline:
                    break
                start = self.state['long'].get(h, self.state['short'][h])
                r = self.cma(os.path.join(HERE, 'heads', h + '.head'), start['v'], 0.12, pop, 60, f'long pop {pop}')
                if r and r['cost'] < start['cost']:
                    self.state['long'][h] = {'cost': r['cost'], 'v': r['v']}
                    self.save()
            pop = min(pop * 2, 68)
        self.check_best(True)
        self.stage = 'done'
        self.status('finished')


if __name__ == '__main__':
    Run(float(sys.argv[1]) if len(sys.argv) > 1 else 10).go()
