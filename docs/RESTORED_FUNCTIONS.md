# Restored functions: behavior and provenance

Status: unreleased source changes; local validation passed on 2026-10-20.

The original public Beta 1 omitted `egen concat()`, `egen cut()`, and
`bcal create` because the excluded implementations had transcription in their
development records. The replacements below were written independently in the
public source tree. The published Beta 1 tag and download retain their original
behavior.

## Sources and separation

Public behavior specifications:

- [Stata egen manual](https://www.stata.com/manuals/degen.pdf): documented
  `concat()` and `cut()` syntax and behavior.
- [Stata bcal manual](https://www.stata.com/manuals/dbcal.pdf): calendar creation
  options and the calendar-file grammar.

Small, newly constructed synthetic inputs were also run through a licensed
Stata installation as a black box to clarify behavior at ties, fractional dates,
and option boundaries. These observations supplied expected behavior, not
implementation code. No Stata `.ado` source, excluded private implementations,
private reference corpus or test logs, or licensed datasets were used for the
replacements or distributed with them. No manual text or worked examples were
copied for this restoration. Existing public helpers are reused, including the
credited civil-date arithmetic listed in `THIRD_PARTY_NOTICES.txt`.

## Independent designs

- `concat()` walks each selected row and joins fields in the requested order.
  Strings pass through unchanged; numeric fields use the existing public
  formatter or an attached value label under `decode`. `punct()` separates
  fields, and `maxlength()` limits decoded labels by bytes while preserving whole
  UTF-8 characters. If no character fits, the numeric code is used. Validation
  completes before
  the new string column is installed.
- `cut()` assigns values to ordered intervals or to cutpoints calculated from
  the sorted selected sample. Ties stay together. The result contains interval
  starts or integer codes; `label` attaches interval labels. Missing inputs and
  values outside an explicit interval range produce missing results.
- Calendar induction floors fractional daily dates, removes duplicates, and
  sorts the observed date set. An extended range adds its explicit endpoints as
  candidate business dates. All other missing integer dates become holidays,
  including gaps outside the original data span. Weekdays absent from the
  original business dates remain nonbusiness weekdays. An omitted run longer
  than `maxgap()` is rejected. The endpoints and chosen center must be included.
  The writer emits explicit `omit date` records that the existing reader and
  `bofd()`/`dofb()` can consume.

Metadata transport is additive: optional label inputs enable decoded
concatenation, and generated labels and storage types return to the native
session for attachment to the new column. Requests that do not produce this
metadata retain their existing result behavior.

## Supported scope and current limits

Both `egen` functions support the existing `if` and `in` selection path. Their
`by` combinations are rejected. `concat(), format()` reuses the public numeric
formatter's `%w.0g` and `%w.Nf` formats; other renderings are rejected explicitly.
`cut(), at()` accepts explicit numbers and
`from(step)to` sequences, with a current implementation limit of 1,600 edges;
other numlist shorthands are rejected. Calendar creation uses `from()` after `if`,
`in`, and `excludemissing()` selection, with the documented date-order choices,
purpose, center, and maximum-gap options.

`bcal create, personal` remains unsupported. `generate()` combined with `if` or
`in` remains unsupported because this build cannot map the filtered data back to
the original rows. Explicit calendar ranges cannot truncate the observed span.
Extended endpoints must fall on weekdays represented in the original business
dates; an endpoint on an inferred nonbusiness weekday produces a reader error.
Intervening absent dates are holidays, and the extended gaps count toward
`maxgap()`.
Singleton calendars and omitted center dates are rejected. Calendar files are
intended to preserve the date mapping; their bytes and compression style are
not promised to match Stata's files.

Induction floors fractional daily dates. The conversion functions retain their
existing truncation toward zero, so negative fractional source values can yield
missing generated business dates even when calendar creation succeeds.

## Public verification

After building the native application and engine, run:

```sh
python3 tests/restored_functions.py
```

The suite creates synthetic strings, interval and tied-group examples, calendar
dates, and expected answers locally. It checks qualifiers, labels, error paths,
calendar regeneration, omissions, and date conversion. It requires no Stata
installation. All 15 regression groups passed. See `VALIDATION.md` for the
recorded build, existing-test, UI, and source-scan results; a passing
bounded suite does not establish full command compatibility or legal clearance.
