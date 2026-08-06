---
id: I018
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

SBR_INTERP60_CENSUS motion census (sms-recomp/overrides/interp60_replace.cpp, sbr_i60r_census) — per-tick |cur-prev| over every recorded draw matrix's translation; the one liveness probe in this arc that watches no named object, so its negatives cannot be an artefact of following the wrong camera

## Validated by

run against BOTH classes: <1e4 bucket = 115,991 with the camera held rotating vs 0 with no input at all, same scenario and window length. The >=1e4 bucket is excluded as a constant garbage population that runs backwards between the classes; an earlier verdict that summed it in could never fire.

## Known failure modes

(none recorded yet)
