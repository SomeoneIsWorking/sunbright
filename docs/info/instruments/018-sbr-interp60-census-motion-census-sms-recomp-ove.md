---
id: I018
kind: instrument
status: DISTRUSTED
created: 2026-08-06
distrusted_on: 2026-08-12
---

## Instrument

SBR_INTERP60_CENSUS motion census (sms-recomp/overrides/interp60_replace.cpp, sbr_i60r_census) — per-tick |cur-prev| over every recorded draw matrix's translation; the one liveness probe in this arc that watches no named object, so its negatives cannot be an artefact of following the wrong camera

## Validated by

run against BOTH classes: <1e4 bucket = 115,991 with the camera held rotating vs 0 with no input at all, same scenario and window length. The >=1e4 bucket is excluded as a constant garbage population that runs backwards between the classes; an earlier verdict that summed it in could never fire.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-12

RETIRED, not caught lying. The SBR_INTERP60_CENSUS motion census lived in sms-recomp/overrides/interp60_replace.cpp, deleted in 21aa561 with the rest of the substitute-and-re-issue interp60 stack. The file is gone, so this cannot be run. Same hazard as I012: it was marked 'trusted', and a trusted entry pointing at a deleted file is a wrong answer delivered confidently to whoever consults the registry instead of searching. The claims it produced (C030, C031) are marked superseded for the same reason.

> Every result this instrument produced is suspect until it is re-validated.
