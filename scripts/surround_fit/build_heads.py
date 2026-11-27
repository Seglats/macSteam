import glob, os, sys
from multiprocessing import Pool
import heads
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'heads')
def one(p):
    name = os.path.basename(os.path.dirname(p)) + '-' + os.path.splitext(os.path.basename(p))[0]
    dst = os.path.join(OUT, name + '.head')
    if os.path.exists(dst): return name + ' (kept)'
    try:
        db, dl = heads.table(p); heads.save(OUT, name, db, dl); return name
    except Exception as e:
        return f'skip {name}: {e}'
if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    files = sorted(set(sum([glob.glob(os.path.join(heads.HRTF, p)) for p in list(heads.GROUPS.values()) + heads.SINGLES], [])))
    with Pool(16) as pool:
        for r in pool.imap_unordered(one, files): print(r, flush=True)
    heads.main(OUT)     # the group averages (singles already built)
