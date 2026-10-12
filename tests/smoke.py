#!/usr/bin/env python3
"""Self-contained public beta checks: independent OLS, data IO and refusals."""
import csv
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / 'build/cobra'
APP = ROOT / 'piet/native/build/piet_native'


def run(cmds, folder):
    env = dict(os.environ, COBRA_BIN=str(ENGINE), PIET_SETTINGS_FILE=str(folder / 'settings.json'),
               CECON_NUM_THREADS='2', OPENBLAS_NUM_THREADS='1')
    req = {'kind': 'dispatch', 'cmds': cmds, 'workdir': str(folder), 'cwd': str(folder)}
    p = subprocess.run([str(APP), '--run-block'], input=json.dumps(req), text=True,
                       capture_output=True, env=env, timeout=90)
    if p.returncode:
        raise AssertionError(f'Adapter exit {p.returncode}: {p.stderr[-2000:]} {p.stdout[-2000:]}')
    result = json.loads(p.stdout)
    assert result['ok'], result
    return result['block_lists']


def errors(blocks):
    return [b for b in blocks if b.get('type') == 'error']


def solve(a, b):
    # Small independent normal-equation reference, partial-pivot elimination.
    a = [list(row) + [v] for row, v in zip(a, b)]
    n = len(b)
    for k in range(n):
        j = max(range(k, n), key=lambda j: abs(a[j][k]))
        a[k], a[j] = a[j], a[k]
        pivot = a[k][k]
        assert abs(pivot) > 1e-12
        a[k] = [v / pivot for v in a[k]]
        for j in range(n):
            if j != k:
                scale = a[j][k]
                a[j] = [x - scale * y for x, y in zip(a[j], a[k])]
    return [row[-1] for row in a]


def main():
    assert ENGINE.is_file() and APP.is_file(), 'Build both executables first.'
    with tempfile.TemporaryDirectory(prefix='piet-public-test-') as d:
        folder = Path(d)
        lists = run(['sysuse synthetic_panel', 'summarize fatal beertax unemp',
                     'regress fatal beertax unemp', 'save roundtrip.dta, replace',
                     'clear', 'use roundtrip.dta', 'regress fatal beertax unemp'], folder)
        assert len(lists) == 7
        for blocks in lists:
            assert not errors(blocks), errors(blocks)
        rows = list(csv.DictReader((ROOT / 'piet/packaging/sample/synthetic_panel.csv').open()))
        x = [[float(r['beertax']), float(r['unemp']), 1.] for r in rows]
        y = [float(r['fatal']) for r in rows]
        xtx = [[sum(row[i]*row[j] for row in x) for j in range(3)] for i in range(3)]
        xty = [sum(row[i]*v for row,v in zip(x,y)) for i in range(3)]
        beta = solve(xtx, xty)
        sse = sum((v-sum(a*b for a,b in zip(row,beta)))**2 for row,v in zip(x,y))
        variance = sse/(len(y)-3)
        inverse = [solve(xtx, [float(j == i) for j in range(3)]) for i in range(3)]
        expected = dict(zip(['beertax','unemp','(intercept)'], zip(beta, [math.sqrt(variance*inverse[i][i]) for i in range(3)])))
        for idx in (2,6):
            tables = [b['coeftable'] for b in lists[idx] if 'coeftable' in b]
            assert len(tables) == 1, lists[idx]
            actual = {r['name']:r for r in tables[0]}
            for name,(b,se) in expected.items():
                assert math.isclose(actual[name]['b'], b, rel_tol=2e-6, abs_tol=2e-6), (name,actual[name],b)
                assert math.isclose(actual[name]['se'], se, rel_tol=2e-6, abs_tol=2e-6), (name,actual[name],se)
        assert (folder/'roundtrip.dta').stat().st_size > 100
        print('PASS: synthetic sample, OLS coefficients/SE against independent reference, DTA roundtrip')
        for cmd in ['egen withheld = concat(fatal beertax)',
                    'egen withheld = cut(fatal)',
                    'bcal create withheld, from(year) replace']:
            blocks = run(['sysuse synthetic_panel',cmd],folder)
            assert not errors(blocks[0]), blocks[0]
            failure = errors(blocks[1])
            assert failure and 'provenance review' in json.dumps(failure), blocks[1]
            assert not (folder/'withheld.stbcal').exists()
        # The native option parser can reject at() before entering the withheld
        # cut implementation; the valid syntax must still never compute output.
        cut = run(['sysuse synthetic_panel', 'egen withheld = cut(fatal), at(0 1 2 10)'], folder)
        assert errors(cut[-1]), cut
        print('PASS: all three withheld features reject explicitly; no calendar written')
        blocks = run(['sysuse synthetic_panel','definitely_not_a_command'],folder)
        assert errors(blocks[-1]), blocks
        print('PASS: unknown command rejected')
    print('Public smoke checks passed. Coverage is limited to the cases above.')


if __name__ == '__main__':
    main()
