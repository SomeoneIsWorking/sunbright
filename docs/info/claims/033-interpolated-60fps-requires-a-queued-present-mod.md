---
id: C033
kind: claim
status: holds
created: 2026-08-06
tags: interpolation,presentation
---

## Claim

An interpolated presentation emitted between two simulation frames requires a queued present mode;
mailbox replacement can discard that intermediate frame even while counters report the requested
presentation rate.

## Evidence

The same two-presents-per-tick sequence was observed in both directions: mailbox mode discarded the
pending intermediate image, while FIFO-relaxed mode displayed it.

## What would falsify it

A controlled two-presents-per-tick run reliably displays the intermediate image under mailbox mode,
or drops it under FIFO-relaxed mode for a different proven cause.
