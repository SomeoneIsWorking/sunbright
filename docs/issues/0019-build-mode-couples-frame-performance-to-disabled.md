---
id: 19
title: Build mode couples frame performance to disabled GPU diagnostics
status: resolved
symptom: The launchers force Release for acceptable speed while Release strips renderer labels and disables Dawn API validation/robustness; stock Debug is unoptimized and too slow for normal play
tags: build,performance,gpu,diagnostics
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The project used CMake's stock Release/Debug configurations as both an optimization policy and a
diagnostic policy. Both launchers forced Release, while Aurora keyed validation, robustness, and GX
debug groups off `NDEBUG`. That made optimization and useful GPU diagnostics mutually exclusive.

Removing that coupling exposed two independent costs which Release had hidden rather than solved:

- The recomp host left `AuroraConfig::logLevel` at the enum's zero value, `LOG_DEBUG`, so every
  compiled debug call was emitted. The explicit host policy now selects `LOG_INFO`.
- Aurora copied a `vector<string>` debug-group stack into every recorded command. Each render pass
  now interns the stack once per push/pop revision and stores a 32-bit snapshot ID in commands.

## Success conditions

- The default launcher build retains symbols, assertions, Dawn backend/API validation, robustness, and GPU labels.
- Both the default diagnostic configuration and explicit Debug compile performance-critical game/runtime code with optimization.
- Release remains an optional packaging profile, not the only playable build.
- A configuration control proves the compile flags and Dawn feature policy actually used.
- Safe bounded Debug and Release controls execute the same optimized game/runtime work and complete
  without a kernel GPU fault. Absolute 60 Hz renderer work remains issue #9 and is not attributed
  from wall-clock runs on a contended host.

## Resolution

### Resolution (2026-08-27)
Both launchers now use a shared optimized Debug profile with symbols/assertions, and Aurora validation/robustness/labels are explicit rather than NDEBUG-controlled. Real Debug and Release compile commands pass I035; matched guarded runs completed GPU-clean with no build-mode throughput difference. Absolute 60 Hz renderer work remains issue #9.
