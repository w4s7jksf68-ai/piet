#!/usr/bin/env python3
"""Synthetic, public-interface regressions for concat(), cut(), and bcal create.

Reference contracts: https://www.stata.com/manuals/degen.pdf (concat and cut),
https://www.stata.com/manuals/dbcal.pdf (calendar induction).  Expectations use
small independently constructed strings, intervals, and civil-date calendars;
no licensed data, implementation files, or private reference logs are needed.

Run after building both executables:
    python3 tests/restored_functions.py
PIET_BIN and COBRA_BIN may point at alternate builds.  main() is also callable
from the public smoke suite.
"""
import csv
from datetime import date, timedelta
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get('PIET_BIN', ROOT / 'piet/native/build/piet_native'))
ENGINE = Path(os.environ.get('COBRA_BIN', ROOT / 'build/cobra'))


def errors(blocks):
    return [block for block in blocks if block.get('type') == 'error']


def run(cmds, folder, failures=()):
    """Run an isolated native session and check each command's error status."""
    env = dict(os.environ, COBRA_BIN=str(ENGINE),
               PIET_SETTINGS_FILE=str(folder / 'settings.json'),
               CECON_NUM_THREADS='2', OPENBLAS_NUM_THREADS='1')
    req = {'kind': 'dispatch', 'cmds': cmds,
           'workdir': str(folder), 'cwd': str(folder)}
    proc = subprocess.run([str(APP), '--run-block'], input=json.dumps(req),
                          text=True, capture_output=True, env=env, timeout=90)
    assert proc.returncode == 0, (proc.returncode, proc.stderr[-2000:], proc.stdout[-2000:])
    result = json.loads(proc.stdout)
    assert result.get('ok'), result
    lists = result['block_lists']
    assert len(lists) == len(cmds), (cmds, lists)
    for index, (cmd, blocks) in enumerate(zip(cmds, lists)):
        failure = errors(blocks)
        if index in failures:
            assert failure, f'Expected an error for {cmd!r}: {blocks}'
        else:
            assert not failure, f'Unexpected error for {cmd!r}: {failure}'
    return lists


def write_csv(folder, names, rows, filename='input.csv'):
    with (folder / filename).open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream)
        writer.writerow(names)
        writer.writerows(rows)


def read_csv(folder, filename='result.csv'):
    with (folder / filename).open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))


def export(filename='result.csv'):
    return f'export delimited using {filename}, replace nolabel'


def assert_column(rows, name, expected):
    actual = [row[name] for row in rows]
    assert len(actual) == len(expected), (name, actual, expected)
    for index, (found, wanted) in enumerate(zip(actual, expected), 1):
        if isinstance(wanted, (int, float)):
            assert math.isclose(float(found), wanted, rel_tol=1e-7, abs_tol=1e-7), \
                (name, index, found, wanted)
        elif wanted is None:
            assert found in ('', '.'), (name, index, found, wanted)
        else:
            assert found == wanted, (name, index, found, wanted)


def test_concat(folder):
    write_csv(folder, ['id', 'n', 's', 'code'], [
        [1, 12, 'Alpha', 1], [2, -3, '', 2], [3, '', 'Omega', 3],
        [4, 0, 'A B', 1], [5, 123.5, 'a,b', 2], [6, '', '', 3],
    ])
    run(['import delimited using input.csv, clear',
         'label define regions 1 "north" 2 "southwest"',
         'label values code regions',
         'egen joined = concat(n s), punct("|")',
         'egen commas = concat(s code), punct(",")',
         'egen parens = concat(s code), punct(")")',
         'egen embedded = concat(s code), punct("a)b")',
         'egen selected = concat(s code) if id != 3 in 2/5, punct("|")',
         'egen formatted = concat(n s), format(%9.2f) punct("|")',
         'egen decoded = concat(s code), decode maxlength(4) punct("|")',
         export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'joined', ['12|Alpha', '-3|', '.|Omega', '0|A B', '123.5|a,b', '.|'])
    assert_column(rows, 'commas', ['Alpha,1', ',2', 'Omega,3', 'A B,1', 'a,b,2', ',3'])
    assert_column(rows, 'parens', ['Alpha)1', ')2', 'Omega)3', 'A B)1', 'a,b)2', ')3'])
    assert_column(rows, 'embedded', ['Alphaa)b1', 'a)b2', 'Omegaa)b3', 'A Ba)b1', 'a,ba)b2', 'a)b3'])
    assert_column(rows, 'selected', ['', '|2', '', 'A B|1', 'a,b|2', ''])
    assert_column(rows, 'formatted', ['12.00|Alpha', '-3.00|', '.|Omega', '0.00|A B', '123.50|a,b', '.|'])
    assert_column(rows, 'decoded', ['Alpha|nort', '|sout', 'Omega|3', 'A B|nort', 'a,b|sout', '|3'])
    # The byte limit must stop before a partial UTF-8 character. If no whole
    # character fits, the observed decode behavior falls back to the code.
    write_csv(folder, ['code'], [[1], [2]], filename='unicode.csv')
    run(['import delimited using unicode.csv, clear',
         'label define unicode_labels 1 "서울" 2 "éclair"',
         'label values code unicode_labels',
         'egen four = concat(code), decode maxlength(4)',
         'egen one = concat(code), decode maxlength(1)', export('unicode-result.csv')], folder)
    rows = read_csv(folder, 'unicode-result.csv')
    assert_column(rows, 'four', ['서', 'écl'])
    assert_column(rows, 'one', ['1', '2'])


def test_cut(folder):
    values = [-1, 0, 0, 1.5, 2, 4.9, 5, 9.9, 10, '']
    write_csv(folder, ['id', 'x'], [[i, value] for i, value in enumerate(values, 1)])
    run(['import delimited using input.csv, clear',
         'egen intervals = cut(x), at(0 2 5 10)',
         'egen codes = cut(x), at(0 2 5 10) icodes',
         'egen labeled = cut(x), at(0 2 5 10) label',
         'decode labeled, generate(labeltext)',
         'egen selected = cut(x) if id != 3 in 2/7, at(0 2 5 10)',
         'egen stepped = cut(x), at(0(2)10) icodes',
         export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'intervals', [None, 0, 0, 0, 2, 2, 5, 5, None, None])
    assert_column(rows, 'codes', [None, 0, 0, 0, 1, 1, 2, 2, None, None])
    assert_column(rows, 'labeled', [None, 0, 0, 0, 1, 1, 2, 2, None, None])
    assert_column(rows, 'labeltext', ['', '0-', '0-', '0-', '2-', '2-', '5-', '5-', '', ''])
    assert_column(rows, 'selected', [None, 0, None, 0, 2, 2, 5, None, None, None])
    assert_column(rows, 'stepped', [None, 0, 0, 0, 1, 2, 2, 4, None, None])


def test_cut_groups(folder):
    # For distinct, evenly spaced data the two groups have four members each.
    write_csv(folder, ['id', 'x'], [[i, i] for i in range(1, 9)] + [[9, '']])
    run(['import delimited using input.csv, clear',
         'egen halves = cut(x), group(2)',
         'egen qualified = cut(x) in 3/6, group(2)', export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'halves', [0, 0, 0, 0, 1, 1, 1, 1, None])
    assert_column(rows, 'qualified', [None, None, 0, 0, 1, 1, None, None, None])
    # Equal inputs must stay together: ties cannot be split to force equal size.
    write_csv(folder, ['id', 'x'], [[i, x] for i, x in enumerate([1, 1, 1, 2, 2, 3, 4, 4], 1)],
              filename='ties.csv')
    run(['import delimited using ties.csv, clear',
         'egen grouped = cut(x), group(3)', export('ties-result.csv')], folder)
    rows = read_csv(folder, 'ties-result.csv')
    # Synthetic black-box reference: tied cutpoints can leave code 0 unused.
    assert_column(rows, 'grouped', [1, 1, 1, 1, 1, 2, 2, 2])
    write_csv(folder, ['x'], [[7], [7], [7]], filename='constant.csv')
    run(['import delimited using constant.csv, clear',
         'egen grouped = cut(x), group(3)', export('constant-result.csv')], folder)
    assert_column(read_csv(folder, 'constant-result.csv'), 'grouped', [2, 2, 2])
    write_csv(folder, ['x'], [[x] for x in [1, 1, 1, 1, 2, 2, 3, 3]],
              filename='four-groups.csv')
    run(['import delimited using four-groups.csv, clear',
         'egen grouped = cut(x), group(4) label',
         'decode grouped, generate(labeltext)',
         'generate probe = _n - 1',
         'label values probe grouped',
         'decode probe, generate(edgetext)', export('four-groups-result.csv')], folder)
    rows = read_csv(folder, 'four-groups-result.csv')
    assert_column(rows, 'grouped', [1, 1, 1, 1, 2, 2, 3, 3])
    assert_column(rows, 'labeltext', ['1-', '1-', '1-', '1-', '1.5-', '1.5-', '2.5-', '2.5-'])
    assert_column(rows[:5], 'edgetext', ['1-', '1-', '1.5-', '2.5-', '4-'])
    write_csv(folder, ['x'], [[1], [2], [3]], filename='nplusone.csv')
    run(['import delimited using nplusone.csv, clear',
         'egen grouped = cut(x), group(4)', export('nplusone-result.csv')], folder)
    codes = [float(row['grouped']) for row in read_csv(folder, 'nplusone-result.csv')]
    assert len(codes) == 3 and all(value.is_integer() and 0 <= value < 4 for value in codes), codes
    assert codes == sorted(codes), codes


def test_cut_fractional_boundary(folder):
    # Synthetic black-box reference around a decimal cutpoint, not a guessed
    # machine-epsilon rule: all three values enter the interval starting .3.
    write_csv(folder, ['x'], [[.29999999999], [.3], [.30000001]])
    run(['import delimited using input.csv, clear',
         'egen codes = cut(x), at(0 .3 .4 1) icodes', export()], folder)
    assert_column(read_csv(folder), 'codes', [1, 1, 1])
    write_csv(folder, ['x'], [[.29999999999], [.3], [.30000001], [.4]],
              filename='grouped.csv')
    run(['import delimited using grouped.csv, clear',
         'egen codes = cut(x), group(2)', export('grouped-result.csv')], folder)
    assert_column(read_csv(folder, 'grouped-result.csv'), 'codes', [1, 1, 1, 1])


def test_egen_errors(folder):
    write_csv(folder, ['id', 'x', 's'], [[1, 1, 'one'], [2, 2, 'two'], [3, '', '']])
    invalid = [
        'egen bad = concat(x s), nonexistent',
        'egen bad = concat(no_such_variable)',
        'egen bad = concat(x s), by(id)',
        'egen bad = concat(x s), by()',
        'egen bad = concat(x s), format()',
        'egen bad = concat(x s), maxlength()',
        'egen bad = concat(x s), punct() punct("|")',
        'egen bad = concat(x s), punct("x"',
        'egen x = concat(x s)',
        'egen bad = cut(x)',
        'egen bad = cut(x), at(0 2 5) group(2)',
        'egen bad = cut(x), at(5 2 0)',
        'egen bad = cut(x), group(0)',
        'egen bad = cut(x), group(2junk)',
        'egen bad = cut(x), at(0 1junk 2)',
        'egen bad = cut(s), at(0 1 2)',
        'egen bad = cut(x), at(0 1 2) by(id)',
        'egen bad = cut(x), at(0 1 2) by()',
    ]
    commands = ['import delimited using input.csv, clear', export('before.csv')]
    commands += invalid + [export('after.csv')]
    run(commands, folder, failures=range(2, 2 + len(invalid)))
    assert read_csv(folder, 'before.csv') == read_csv(folder, 'after.csv'), \
        'A rejected egen command changed the data or left a target column behind'


def test_egen_dta_roundtrip(folder):
    write_csv(folder, ['n', 's'], [
        [0, 'left'], [.5, ''], [1, 'two words'], ['', 'gap'], [3, 'tail'],
    ])
    lists = run(['import delimited using input.csv, clear',
                 'set type double',
                 'egen joined = concat(s n), punct("|")',
                 'egen category = cut(n), at(0 .5 2 4) label',
                 'egen byte fractional = cut(n), at(0 .5 2 4)',
                 'decode category, generate(labels_before)',
                 'describe joined category fractional', export('before.csv'),
                 'save semantic.dta, replace', 'clear', 'use semantic.dta',
                 'decode category, generate(labels_after)',
                 'describe joined category fractional', export('after.csv')], folder)
    before, after = read_csv(folder, 'before.csv'), read_csv(folder, 'after.csv')
    strings = ['left|0', '|.5', 'two words|1', 'gap|.', 'tail|3']
    labels = ['0-', '.5-', '.5-', '', '2-']
    for rows in (before, after):
        assert_column(rows, 'joined', strings)
        assert_column(rows, 'category', [0, 1, 1, None, 2])
        assert_column(rows, 'fractional', [0, .5, .5, None, 2])
    assert_column(before, 'labels_before', labels)
    assert_column(after, 'labels_after', labels)
    descriptions = [block for blocks in lists for block in blocks
                    if block.get('type') == 'describe']
    assert len(descriptions) == 2, descriptions
    for description in descriptions:
        variables = {var['name']: var for var in description['variables']}
        assert variables['joined']['dtype'].startswith('str'), variables['joined']
        assert variables['fractional']['dtype'] in ('float', 'double'), variables['fractional']


def td(day):
    return (day - date(1960, 1, 1)).days


def calendar_input(folder):
    first, last = date(2024, 1, 1), date(2024, 1, 19)
    days = [first + timedelta(days=i) for i in range((last - first).days + 1)]
    included = [day for day in days if day.weekday() < 5 and day != date(2024, 1, 10)]
    write_csv(folder, ['id', 'd'], [[i, td(day)] for i, day in enumerate(included, 1)])
    return included


def test_calendar(folder):
    included = calendar_input(folder)
    center = included.index(date(2024, 1, 8))
    lists = run(['import delimited using input.csv, clear',
                 'bcal create weekdays, from(d) range(2024jan01 2024jan19) '
                 'centerdate(2024jan08) generate(b) purpose("Synthetic weekdays and holiday")',
                 'generate mapped = bofd("weekdays", d)',
                 'generate roundtrip = dofb(b, "weekdays")',
                 'describe b', 'format b %12.0g', export()], folder)
    assert (folder / 'weekdays.stbcal').is_file()
    rows = read_csv(folder)
    expected = [index - center for index in range(len(included))]
    assert_column(rows, 'b', expected)
    assert_column(rows, 'mapped', expected)
    assert_column(rows, 'roundtrip', [td(day) for day in included])
    descriptions = [block for blocks in lists for block in blocks
                    if block.get('type') == 'describe']
    assert len(descriptions) == 1, descriptions
    assert descriptions[0]['variables'][0]['dtype'] == 'long', descriptions

    # A new process has to reload the generated file, not reuse create's cache.
    probes = [date(2023, 12, 31) + timedelta(days=i) for i in range(22)]
    write_csv(folder, ['d'], [[td(day)] for day in probes], filename='probes.csv')
    run(['import delimited using probes.csv, clear',
         'generate mapped = bofd("weekdays", d)',
         'generate roundtrip = dofb(mapped, "weekdays")', export('probes-result.csv')], folder)
    rows = read_csv(folder, 'probes-result.csv')
    mapping = {day: index - center for index, day in enumerate(included)}
    assert_column(rows, 'mapped', [mapping.get(day) for day in probes])
    assert_column(rows, 'roundtrip', [td(day) if day in mapping else None for day in probes])


def test_calendar_replace_cache(folder):
    included = calendar_input(folder)
    run(['import delimited using input.csv, clear',
         'bcal create cached, from(d) centerdate(2024jan01)',
         'generate before = bofd("cached", d)',
         'bcal create cached, from(d) centerdate(2024jan02) replace',
         'generate after = bofd("cached", d)',
         'generate roundtrip = dofb(after, "cached")', export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'before', list(range(len(included))))
    assert_column(rows, 'after', [i - 1 for i in range(len(included))])
    assert_column(rows, 'roundtrip', [td(day) for day in included])


def test_calendar_dateformat(folder):
    included = calendar_input(folder)
    center = included.index(date(2024, 1, 8))
    run(['import delimited using input.csv, clear',
         'bcal create dmydates, from(d) dateformat(dmy) '
         'range(01jan2024 19jan2024) centerdate(08jan2024) generate(b)',
         'generate roundtrip = dofb(b, "dmydates")',
         'format b %12.0g', export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'b', [index - center for index in range(len(included))])
    assert_column(rows, 'roundtrip', [td(day) for day in included])


def test_calendar_extension(folder):
    # Observed synthetic extension: original td0..13 plus the two requested
    # endpoints are included. Other dates added to the span remain omitted.
    write_csv(folder, ['d'], [[value] for value in range(14)])
    run(['import delimited using input.csv, clear',
         'bcal create extended, from(d) range(1959dec25 1960jan22) '
         'centerdate(1960jan01) generate(b)',
         'format b %12.0g', export()], folder)
    assert_column(read_csv(folder), 'b', list(range(14)))
    probes = list(range(-8, 23))
    write_csv(folder, ['d'], [[value] for value in probes], filename='probes.csv')
    run(['import delimited using probes.csv, clear',
         'generate b = bofd("extended", d)',
         'generate roundtrip = dofb(b, "extended")', export('probes-result.csv')], folder)
    rows = read_csv(folder, 'probes-result.csv')
    included = [-7] + list(range(14)) + [21]
    mapping = {day: index - 1 for index, day in enumerate(included)}
    assert_column(rows, 'b', [mapping.get(day) for day in probes])
    assert_column(rows, 'roundtrip', [day if day in mapping else None for day in probes])


def test_calendar_qualifiers(folder):
    dates = [date(2024, 1, 1) + timedelta(days=i) for i in range(8)]
    write_csv(folder, ['d', 'flag'], [[td(day), int(i != 2)] for i, day in enumerate(dates)])
    run(['import delimited using input.csv, clear',
         'bcal create subset if flag == 1 in 2/6, from(d)',
         'generate b = bofd("subset", d)', export()], folder)
    assert_column(read_csv(folder), 'b', [None, 0, None, 1, 2, 3, None, None])


def test_calendar_excludemissing(folder):
    dates = [date(2024, 1, 1) + timedelta(days=i) for i in range(5)]
    missing_pattern = [(1, 1), ('', 1), (1, ''), ('', ''), (1, 1)]
    write_csv(folder, ['d', 'a', 'b'],
              [[td(day), a, b] for day, (a, b) in zip(dates, missing_pattern)])
    run(['import delimited using input.csv, clear',
         'bcal create allmiss, from(d) excludemissing(a b) generate(defaultb)',
         'bcal create anymiss, from(d) excludemissing(a b, any) generate(anyb)',
         'format defaultb anyb %12.0g', export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'defaultb', [0, 1, 2, None, 3])
    assert_column(rows, 'anyb', [0, None, None, None, 1])


def test_calendar_errors(folder):
    calendar_input(folder)
    run(['import delimited using input.csv, clear',
         'bcal create locked, from(d)'], folder)
    original = (folder / 'locked.stbcal').read_bytes()
    invalid = [
        'bcal create locked, from(d)',
        'bcal describe locked in 1/2',
        'bcal check d in 1/2',
        'bcal dir if id > 0',
        'bcal create invalid, from(d) maxgap(0) generate(never)',
        'bcal create invalid, from(d) maxgap',
        'bcal create invalid, from(d) dateformat',
        'bcal create invalid, from(d) range',
        'bcal create invalid, from(d) centerdate',
        'bcal create invalid, from(d) replace(0)',
        'bcal create invalid, from(d) maxgap(2) maxgap(3)',
        'bcal create invalid extra, from(d)',
        'bcal create invalid, from(d) dateformat(bogus)',
        'bcal create invalid, from(d) range(2024jan19 2024jan01)',
        'bcal create invalid, from(d) range(2024jan03 2024jan19)',
        # These outward endpoints are weekend dates, whose weekdays are absent
        # from the original source: an observed endpoint-inference error, not a general
        # prohibition on extending a calendar's range).
        'bcal create invalid, from(d) range(2023dec31 2024jan20)',
        'bcal create invalid, from(d) centerdate(bogus)',
        'bcal create invalid, from(no_such_variable)',
        'bcal create invalid, from(d) nonexistent',
        'bcal create invalid',
    ]
    commands = ['import delimited using input.csv, clear', export('before.csv')]
    commands += invalid + [export('after.csv')]
    run(commands, folder, failures=range(2, 2 + len(invalid)))
    assert read_csv(folder, 'before.csv') == read_csv(folder, 'after.csv'), \
        'A rejected calendar command changed the dataset'
    assert (folder / 'locked.stbcal').read_bytes() == original, \
        'Calendar file changed without replace'
    assert not (folder / 'invalid.stbcal').exists(), \
        'Invalid options left a calendar file behind'


def test_calendar_maxgap(folder):
    # A maximum of two consecutive omitted dates admits Mon/Thu, not Mon/Fri.
    write_csv(folder, ['d'], [[td(date(2024, 1, 1))], [td(date(2024, 1, 4))]])
    run(['import delimited using input.csv, clear',
         'bcal create twogap, from(d) maxgap(2)',
         'generate b = bofd("twogap", d)', export()], folder)
    assert_column(read_csv(folder), 'b', [0, 1])
    write_csv(folder, ['d'], [[td(date(2024, 1, 1))], [td(date(2024, 1, 5))]],
              filename='too-wide.csv')
    run(['import delimited using too-wide.csv, clear', export('before.csv'),
         'bcal create biggap, from(d) maxgap(2) generate(never)',
         export('after.csv')], folder, failures=(2,))
    assert not (folder / 'biggap.stbcal').exists()
    assert read_csv(folder, 'before.csv') == read_csv(folder, 'after.csv')


def test_calendar_fractional_and_invalid_dates(folder):
    # Positive fractions distinguish accepted dates from rejection. Induction
    # floors negative dates; generate() still uses standalone bofd truncation.
    write_csv(folder, ['d'], [[1], [1.5], [2]])
    run(['import delimited using input.csv, clear',
         'bcal create fraction, from(d) generate(b)',
         'generate roundtrip = dofb(b, "fraction")',
         'format b %12.0g', export()], folder)
    rows = read_csv(folder)
    assert_column(rows, 'b', [0, 0, 1])
    assert_column(rows, 'roundtrip', [1, 1, 2])
    write_csv(folder, ['d'], [[-1.5], [1]], filename='negative.csv')
    run(['import delimited using negative.csv, clear',
         'bcal create negative, from(d) generate(b)',
         'generate roundtrip = dofb(b, "negative")',
         'generate first_day = dofb(0, "negative")',
         'format b %12.0g', export('negative-result.csv')], folder)
    rows = read_csv(folder, 'negative-result.csv')
    assert_column(rows, 'b', [None, 1])
    assert_column(rows, 'roundtrip', [None, 1])
    assert_column(rows, 'first_day', [-2, -2])
    for name, dates in [('missing', [1, '', 2]), ('single', [1])]:
        write_csv(folder, ['d'], [[value] for value in dates], filename=f'{name}.csv')
        run([f'import delimited using {name}.csv, clear', export('before.csv'),
             f'bcal create {name}, from(d) generate(never)', export('after.csv')],
            folder, failures=(2,))
        assert not (folder / f'{name}.stbcal').exists()
        assert read_csv(folder, 'before.csv') == read_csv(folder, 'after.csv')


def main():
    assert ENGINE.is_file() and APP.is_file(), \
        'Build both executables first, or set PIET_BIN and COBRA_BIN.'
    checks = [test_concat, test_cut, test_cut_groups, test_cut_fractional_boundary, test_egen_errors,
              test_egen_dta_roundtrip,
              test_calendar, test_calendar_replace_cache, test_calendar_dateformat,
              test_calendar_extension,
              test_calendar_qualifiers, test_calendar_excludemissing,
              test_calendar_errors, test_calendar_maxgap,
              test_calendar_fractional_and_invalid_dates]
    with tempfile.TemporaryDirectory(prefix='piet-restored-functions-') as temp:
        for check in checks:
            folder = Path(temp) / check.__name__
            folder.mkdir()
            check(folder)
            print(f'PASS: {check.__name__}')
    print('Restored-function checks passed using synthetic data.')


if __name__ == '__main__':
    main()
