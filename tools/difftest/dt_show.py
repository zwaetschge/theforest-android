#!/usr/bin/env python3
"""dt_show.py <outdir> <regex on 'mn ops' or encoding hex> [n] - dump recorded mismatches for matching cases."""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dt_report as R
od, pat = sys.argv[1], sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 2
res = json.load(open(os.path.join(od, 'results.json')))
recs = R.read_records(os.path.join(od, 'dtrecords.bin'))
for c in res['cases']:
    if c['status'] != 'tested':
        continue
    if not (re.search(pat, c['mn'] + ' ' + c['ops']) or pat == c['enc']):
        continue
    rs = recs.get(c['idx'], [])
    print('== %s %s [%s] pass=%d fail=%d nfault=%d lfault=%d' % (c['mn'], c['ops'], c['enc'], c['pass'], c['fail'], c['nfault'], c['lfault']))
    print('   ' + R.lifted_stmt(c))
    k = 0
    for r in rs:
        cls, diffs = R.classify(c, r)
        if cls == 'PASS':
            continue
        print('  [%s]' % cls)
        print('\n'.join(R.dump_case(c, r, cls, diffs)))
        k += 1
        if k >= n:
            break
