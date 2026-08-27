---
id: 23
title: Native renderer startup races Aurora Vulkan pipeline compilation
status: resolved
symptom: sms-recomp intermittently SIGSEGVs during native renderer startup before its first GPU submit; SDL_CreateGPUDevice unloads a scanned Vulkan ICD while Aurora's pipeline worker calls SetDebugUtilsObjectNameEXT
tags: render,recomp,native,aurora,vulkan,crash,startup
created: 2026-08-28
updated: 2026-08-28
---

A live guarded native-renderer rerun crashed before the native device initialized. coredump PID 3575158 proves two concurrent Vulkan-loader users: the main thread was inside SDL_CreateGPUDevice -> vkEnumeratePhysicalDevices -> loader_unload_scanned_icd -> dlclose, while Aurora pipeline_worker was inside Dawn render-pipeline creation -> SetDebugUtilsObjectNameEXT -> loader_get_icd_and_device.

The submit-flight report is empty because no GPU submission occurred. This is a process startup synchronization defect, not evidence of a GPU reset. Proper resolution must establish a tested quiescence/ordering contract before creating the second Vulkan device; repeated startup attempts are not evidence.

## Evidence

- systemd coredump PID 3575158, 2026-08-28 00:38:42 +03
- main: sms-recomp+0x25457 sbr_render_init -> SDL_CreateGPUDevice -> Vulkan loader dlclose
- worker: sms-recomp+0x4631609 aurora::gfx::pipeline_worker -> Dawn Vulkan debug naming

## Exit condition

A control proves the new synchronization waits for in-flight pipeline creation, a mutation proves it observes the non-quiescent state, the combined Clang tests pass, and a guarded live native A/B launch reaches clean shutdown without validation errors or a device fault.

### Resolution (2026-08-28)
Aurora now pauses its background pipeline-creation gate and waits for in-flight Dawn/Vulkan pipeline work before the host calls SDL_CreateGPUDevice, then resumes immediately. Deterministic gate controls pass, the Clang sms-recomp binary links both public calls, and guarded live native A/B reached 130 presents and clean shutdown with no validation error, signal, or GPU fault on 2026-08-28.
