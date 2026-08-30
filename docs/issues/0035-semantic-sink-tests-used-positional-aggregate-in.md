---
id: 35
title: Semantic sink tests used positional aggregate initializers
status: resolved
symptom: A clean full rebuild failed because tests passed a context pointer into the newly added model-callback field, while an incrementally built tree had hidden the stale aggregate initialization.
state_items: S004,S005
tags: tests,semantic,sink
created: 2026-08-30
updated: 2026-08-30
---

SemanticSink gained submitModel before context. Two tests still used positional aggregate initialization, so one no longer compiled and one built a sink missing the now-required model callback. Replace positional initialization with named fields and provide a planted unexpected-model callback so the tests satisfy the full sink contract without silently accepting model traffic.

### Resolution (2026-08-30)
Both tests now initialize SemanticSink with named submit/submitModel/context fields. Their model callback asserts if called, preserving the test boundary. A clean root rebuild and all 50 root tests pass.
