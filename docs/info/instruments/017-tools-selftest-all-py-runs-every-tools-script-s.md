---
id: I017
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/selftest_all.py — runs every tools/ script's own --selftest; in .githooks/pre-commit

## Validated by

Refuses (exit 1) when it discovers zero tools, because an empty suite prints the same thing as a passing one. Verified by running it: 3 of 3 discovered tools pass.

## Known failure modes

(none recorded yet)
