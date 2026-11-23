# Alan Wake 2 path tracing: Xid 109 on NVIDIA (2026-11-23)

## Symptom

Alan Wake 2 (Epic) on GE-Proton11-12 (vkd3d-proton 8293130eb,
`v3.1-1-g8293130eb-GE-Proton(dirty)`), RTX 5090, NVIDIA 620.73.04 (also
615.71.09), 5120x1440, path tracing on (`m_ePTQuality` 4, DLSS RR on,
`m_eRTTransparentQuality` 3): 10-30 s into gameplay the kernel logs
`NVRM: Xid 109 ... CTX SWITCH TIMEOUT`, winevulkan logs `vr -4` (device lost)
and the game shows its "GPU fault" dialog. With path tracing off and RT
transparency 3 it was Xid 13 "Graphics SM Warp Exception: Illegal Instruction
Parameter" instead. GE-Proton10-34 (vkd3d-proton bd3f5e3d) path traces for
10 minutes without a fault.

## Root cause

vkd3d-proton aea5653c ("vkd3d: Expose SM 6.9", in 3.1) reports shader model
6.9 on NVIDIA once the device has KHR_opacity_micromap, maintenance9,
EXT_shader_long_vector and EXT_ray_tracing_invocation_reorder. Alan Wake 2
then takes its SM 6.9 / DXR 1.2 path. Its ray generation shaders
`realTimePathTracingRayGeneration` (path tracing) and
`transparentReflectionRayGeneration` (RT transparency) use ray queries with
opacity micromaps and call `MaybeReorderThread(hint, bits)` twice: once inside
the bounce loop (16 hint bits) and once after it (2 bits), both in divergent
control flow. DXC emits that as `MaybeReorderThread(HitObject_MakeNop(), hint,
bits)`, and dxil-spirv (f4420abf, "Add support for MaybeReorderThread")
translates it to:

```
OpHitObjectRecordEmptyEXT %hit
OpReorderThreadWithHitObjectEXT %hit %hint %uint_16
```

NVIDIA's implementation of the EXT reorder (SPV_EXT_shader_invocation_reorder)
hangs the GPU on these shaders; the NV reorder (SPV_NV_shader_invocation_reorder)
does not. The translation is a faithful one; this is a driver bug that
vkd3d-proton works around (below).

Before aea5653c the game did not see SM 6.9 and ran an SM 6.8 variant of the
ray generation shader (no SER, no OMM), which works: 1aef6005, the commit right
before, path traces cleanly. Earlier in the range (1102cd83, and c9c6bf2e9 in
GE-Proton11-6) dxil-spirv could not convert that variant ("Failed to convert
DXIL export: ?realTimePathTracingRayGeneration@@YAXXZ"); the game then shows
its GPU-fault dialog without any Xid. A later dxil-spirv bump fixed that
before 1aef6005; it is a different, already fixed problem.

A second, separate dxil-spirv bug showed up on the way: these shaders use the
constant ray flags 0x611 (FORCE_OMM_2_STATE among them) but the SPIR-V lacks
`OpCapability RayTracingOpacityMicromapKHR`, so spirv-val rejects it.
dxil-spirv (c57ff643, "Some cleanups with RQ OMM") analyzes TraceRayInline flags
only once it has seen an AllocateRayQuery2 with the OMM flag, and blocks are
not analyzed in program order. Adding the capability alone does not stop the
hang (E1 below).

## How it was found

Lab instance `aw2-crash` on rayblaster (`quiver --machine rayblaster lab`),
the person's save, the vkd3d-proton DLLs (`d3d12.dll`, `D3D12Core.dll`, x64,
built with mingw-w64 and GE's meson args `-Db_ndebug=true
-Denable_extended_emulation=true`) put over the install through `mod_dir`. A
run is bad when the kernel logs an Xid for AlanWake2.exe during path-traced
gameplay, good after 5 minutes without one.

| Run | vkd3d-proton | Change | Result |
| --- | --- | --- | --- |
| (earlier) | bd3f5e3d (GE 10-34) | none | good, 10 min |
| (earlier) | c9c6bf2e9 (GE 11-6) | none | raygen export fails to convert |
| r5 | 8293130eb, clean upstream (no GE patches) | none | **bad**, Xid 109 ~5 s into gameplay |
| r6 | 8293130eb clean | `VKD3D_DISABLE_EXTENSIONS=VK_NV_ray_tracing_invocation_reorder,VK_EXT_ray_tracing_invocation_reorder` (no SER, so no SM 6.9) | good, 5 min |
| r7 | 1102cd83 (bisect midpoint, before aea5653c) | none | no Xid; raygen export fails to convert, GPU-fault dialog |
| r8 | 8293130eb clean | `VKD3D_SHADER_DUMP_PATH` | bad, Xid 109 (shaders dumped) |
| r9 | 8293130eb clean, E1 | override: SPIR-V + `OpCapability RayTracingOpacityMicromapKHR` | **bad**, Xid 109 |
| r10 | 8293130eb clean, E2 | override: both reorders (and their RecordEmpty) removed | good, 5 min |
| r11 | 8293130eb clean, E5 | override: `OpReorderThreadWithHintEXT hint bits` | **bad**, Xid 109 |
| r12 | 8293130eb clean, E8 | override: `OpReorderThreadWithHintNV hint bits`, NV capability/extension | good, 5 min |
| r13 | 8293130eb clean, E9 | override: RecordEmptyEXT kept, reorder as `OpReorderThreadWithHintNV` (what the fix emits) | not valid: the run stayed on the loading screen (no gameplay); r14 covers it |
| r14 | 8293130eb + GE 0001-0004 + the fix (then carried as patches 0005/0006) | path tracing on | **good**, 10 min 42 s of path tracing, no Xid |
| r15 | 8293130eb clean | path tracing off, RT transparency 3, RR on | **bad**, Xid 109 at the end of loading (the earlier session saw Xid 13 here) |
| r16 | 8293130eb + GE 0001-0004 + the fix (then carried as patches 0005/0006), with `VKD3D_SHADER_DUMP_PATH` | path tracing off, RT transparency 3, RR on | **good**, 6 min, no Xid; the dumped transparentReflectionRayGeneration has `OpReorderThreadWithHintNV` twice and passes spirv-val |
| r17 | aea5653c ("vkd3d: Expose SM 6.9") | none | **bad**, Xid 109 22 s into gameplay |
| r18 | 1aef6005 (its parent, same dxil-spirv 826a71a0) | none | **good**, 5 min of path tracing, no conversion errors (SM 6.8) |

First bad commit: **aea5653c "vkd3d: Expose SM 6.9"**. Its parent uses the same
dxil-spirv (826a71a0), so the jump is not a dxil-spirv move and there was
nothing to bisect inside dxil-spirv. GE's vkd3d patches play no part (r5 is
clean upstream). The SM 6.9 exposure is not wrong in itself: it is what makes
the game run the shaders that trip the driver.

The overrides used `VKD3D_SHADER_OVERRIDE` (a `<hash>.lib.<export>.spv` per
export, `VKD3D_SHADER_DEBUG=info` shows "Overriding shader hash ...") on the
two exports dumped in r8: `971d5a2ce351617e` (realTimePathTracingRayGeneration)
and `0a55d145f42092ab` (transparentReflectionRayGeneration). Setting
`VKD3D_SHADER_OVERRIDE` also forces `pipeline_library_ignore_spirv`, so the
game's pipeline library cannot hand back the old SPIR-V.

The dumped DXIL converts offline with dxil-spirv's CLI
(`dxil-spirv 971d5a2ce351617e.lib.dxil --entry realTimePathTracingRayGeneration
--opacity-micromap --raw-access-chains-nv`) to the same SPIR-V shape, and
spirv-val (SPIRV-Tools v2026.4) reports the missing OMM capability on it.

## Fix

The fix lives in the `vkd3d-proton` submodule, which now points at the fork
AndrewGaspar/vkd3d-proton (as `dxvk` points at AndrewGaspar/dxvk):

- AndrewGaspar/dxil-spirv, branch `quiver-nv-ser-hint`, on 8b48f0e0 (the
  dxil-spirv that vkd3d-proton 8293130eb pins):
  - b518f715 "Add an option to lower hint-only MaybeReorderThread to the NV
    opcode": `DXIL_SPV_OPTION_INVOCATION_REORDER_HINT_NV` (57). With it,
    MaybeReorderThread on a NOP hit object becomes `OpReorderThreadWithHintNV`.
    Reorders on real hit objects stay EXT.
  - 576eeb0c "Always analyze TraceRayInline flags for the OMM ray flag
    capability": the spirv-val fix (not the hang).
- AndrewGaspar/vkd3d-proton, branch `quiver-aw2-ser-fix`, on 8293130eb:
  - 5d7b1e44 "dxil-spirv: point at AndrewGaspar/dxil-spirv quiver-nv-ser-hint".
  - 623c11ae "vkd3d: Reorder by hint with the NV opcode when NV SER is
    available": a new shader target extension, set when the device has
    VK_NV_ray_tracing_invocation_reorder, turns the dxil-spirv option on.
    SER keeps reordering; AMD and Mesa are unchanged. The new target extension
    changes vkd3d's shader cache key, so SPIR-V cached by earlier builds is not
    reused.

GE's vkd3d patches 0001-0004 apply on top unchanged. The same change was first
carried as GE-style patches on `custom-ge11-aw2-pt-fix` (superseded; that is
the build the lab runs above verified, and the fork commits hold identical
code).

Rebuilding GE-Proton with it: in a full checkout of this branch (`git submodule
update --init --recursive`, which now fetches vkd3d-proton and its dxil-spirv
from the forks), `./patches/protonprep-valve-staging.sh &> patchlog.txt`, check
`grep -i -e fail -e error patchlog.txt`, then in a build directory
`../configure.sh --build-name=GE-Proton11-12-aw2fix && make redist`. Quicker:
build only vkd3d-proton (meson cross with `build-win64.txt`, the args above)
and replace `files/lib/wine/vkd3d-proton/x86_64-windows/{d3d12,d3d12core}.dll`
in a copy of an installed GE-Proton11-12 (as `GE-Proton11-12-hypxr` was made).
Proton copies those into a prefix's `system32` when the build changes, so give
the copy its own name/version, or a prefix that already ran GE-Proton11-12
may keep the old DLLs.

## Upstream

- vkd3d-proton / dxil-spirv: report the EXT reorder hang on NVIDIA with the
  workaround (draft below), and the missing OMM capability (dxil-spirv).
- NVIDIA: the driver bug proper (EXT reorder hangs where NV reorder does not).
  A repro needs the two dumped exports; they are game shaders, so share them
  privately rather than attaching them to a public issue.

### Draft: dxil-spirv / vkd3d-proton issue

> **Alan Wake 2 SM 6.9 path tracing hangs NVIDIA (Xid 109) with
> OpReorderThreadWithHitObjectEXT; OpReorderThreadWithHintNV works**
>
> vkd3d-proton 8293130eb (3.1), dxil-spirv 8b48f0e0, RTX 5090, NVIDIA
> 620.73.04 and 615.71.09, Linux. Since "vkd3d: Expose SM 6.9" Alan Wake 2
> uses its DXR 1.2 path. With path tracing on, the GPU hangs 10-30 s into
> gameplay (`NVRM: Xid 109 CTX SWITCH TIMEOUT`, VK_ERROR_DEVICE_LOST); with
> path tracing off and RT transparency on, Xid 13 (Illegal Instruction
> Parameter). The ray generation exports realTimePathTracingRayGeneration
> and transparentReflectionRayGeneration call MaybeReorderThread(hint, bits)
> (on HitObject_MakeNop) in divergent control flow, one inside a loop.
>
> Replacing only those two exports' SPIR-V (VKD3D_SHADER_OVERRIDE):
> no reorder: fine; OpReorderThreadWithHintEXT: hangs;
> OpReorderThreadWithHintNV: fine; RecordEmptyEXT + OpReorderThreadWithHintNV:
> fine. So it looks like an NVIDIA EXT SER bug. As a workaround we lower
> MaybeReorderThread on a NOP hit object to OpReorderThreadWithHintNV when
> VK_NV_ray_tracing_invocation_reorder is present (patch attached). Happy to
> share the DXIL privately.
>
> Separately: in the same shaders the constant ray flags 0x611 include
> FORCE_OMM_2_STATE but the module lacks `OpCapability
> RayTracingOpacityMicromapKHR` (spirv-val error). The RayQuery_TraceRayInline
> analysis only looks at flags after requires_opacity_micromap_tracing has
> been set by an AllocateRayQuery2, which depends on block visit order.
