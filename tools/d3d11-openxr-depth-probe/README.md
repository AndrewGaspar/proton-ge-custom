# D3D11 OpenXR D32S8 probe

This probe reproduces the OVRPlugin-facing path through the Windows OpenXR
loader, WineOpenXR's private DXVK interop, and the exact GE-Proton10-32 DXVK
deployment. It requests a 2064x2208 `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`
swapchain with `DEPTH_STENCIL_ATTACHMENT | SAMPLED` usage.

Stages are intentionally ordered from lowest risk to highest:

1. `bootstrap`: prove that UMU, Proton, and Wine's volatile OpenXR registry are
   ready, then exit before the probe creates an OpenXR session or D3D11 device.
2. `enumerate`: create and enumerate the swapchain images, then print the
   D3D11 resource descriptor.
3. `views`: additionally create the OVR-style typed DSV and depth-plane SRV.
4. `clear`: run two acquire/wait, depth/stencil clear, event-query fence, and
   release cycles.
5. `sample`: additionally sample the cleared depth plane through the typed SRV,
   copy the value to a staging buffer, and verify readback in both cycles.
6. `teardown`: acquire/wait/clear/release, immediately release the OVR-style
   DSV/SRV and destroy the swapchain without an application-side fence, then
   issue an unrelated D3D11 event fence. This isolates the destroy path's own
   GPU drain and wrapper/view teardown ordering.
7. `recreate`: create six OVR-style depth swapchains, queue and release a clear
   on every one, immediately destroy all six without an application-side fence,
   recreate all six, clear/release/fence them, destroy them again, and fence
   once more. This reproduces Unity's startup eye-texture rebuild.
8. `discard`: run `recreate`, but call `ID3D11DeviceContext1::DiscardView` on
   every acquired DSV before clearing it. This covers Oculus XR Plugin's
   `Optimize Buffer Discards: Yes` path separately.
9. `draw-depth`: execute a depth-writing draw against the client-local D32S8
   swapchain with no color attachment. This differs from `clear` by exercising
   an actual graphics pipeline and depth tests. Rerun it first after changing
   probe cleanup so its completed teardown is established before adding color.
10. `draw-native-pair`: bind an ordinary DXVK-created NVIDIA color texture
   alongside the borrowed client-local D32S8 swapchain image. This separates
   mixed attachment provenance from the format and render-pipeline contract.
11. `draw-color`: draw to a real cross-GPU RGBA8 color swapchain without a
   depth attachment. This isolates the imported color attachment and its
   FOREIGN ownership/layout path.
12. `draw-bind`: acquire and bind the imported cross-GPU color RTV and local
   D32S8 DSV together, then unbind without a clear or Draw. This determines
   whether mere D3D11 binding/lifecycle is safe; DXVK may defer actual dynamic
   rendering setup until work is emitted.
13. `draw-dsv-disabled`: draw color to the imported RTV while the local D32S8
   DSV remains bound, but leave depth testing and writes disabled and never
   clear depth. This isolates attachment presence from depth access.
14. `draw`: create a real cross-GPU RGBA8 color swapchain alongside D32S8,
   validate both imported D3D11 resource descriptors, bind a typed RTV and DSV,
   clear them, execute a depth-writing VS/PS draw, complete an event-query
   fence, and release both images. This is the first stage that puts the
   cross-GPU color image and client-local depth image in one graphics render
   pass.
15. `frame`: begin an active stereo OpenXR session, create separate color/depth
   pairs for both eyes, discard/clear/draw each pair without an application
   fence before release, and submit them through `xrEndFrame` as a projection
   layer with `XrCompositionLayerDepthInfoKHR` chains for up to three seconds
   (at most 120 attempts). It then verifies that a post-submit D3D11 event fence
   completes.
16. `unity`: require three images in each per-eye color and depth swapchain,
    destroy the resulting six texture pairs before rendering, recreate all six
    pairs, then run the same discard/draw/projection-depth frame loop. This
    matches the two `XRTextureManager` generations in Walkabout's `Player.log`.

The guarded runner compares kernel Xid counts before and after every stage and
returns 99 after any new NVIDIA Xid. Each stage also has a bounded timeout that
terminates only the probe's UMU process group; the post-run Xid check still runs
after a timeout. It never broadly kills Wine or a wineserver.

Before every draw or frame stage, the runner ignores any inherited
`HYPRLAND_INSTANCE_SIGNATURE`, resolves `hyprctl instances`, requires exactly
one live compositor, and passes that explicit signature to both its status
query and the UMU/Proton process. It refuses to render
unless the live OpenXR session is focused, visible, and reports user presence.
The probe logs each OpenXR/D3D cleanup phase before printing its PASS marker, so
a successful GPU fence cannot be mistaken for a fully completed process.

```sh
OPENXR_LOADER_DLL=/path/to/OpenXR-1.1/openxr_loader.dll \
  bash tools/d3d11-openxr-depth-probe/build.sh
bash tools/d3d11-openxr-depth-probe/run-guarded.sh enumerate
bash tools/d3d11-openxr-depth-probe/run-guarded.sh views
bash tools/d3d11-openxr-depth-probe/run-guarded.sh clear
bash tools/d3d11-openxr-depth-probe/run-guarded.sh sample
bash tools/d3d11-openxr-depth-probe/run-guarded.sh teardown
bash tools/d3d11-openxr-depth-probe/run-guarded.sh recreate
bash tools/d3d11-openxr-depth-probe/run-guarded.sh discard
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-depth
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-native-pair
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-color
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-bind
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-dsv-disabled
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw
bash tools/d3d11-openxr-depth-probe/run-guarded.sh frame
bash tools/d3d11-openxr-depth-probe/run-guarded.sh unity
```

Run the stages in that order and stop after any failure or exit 99. `enumerate`
must report resource format 19 (`DXGI_FORMAT_R32G8X24_TYPELESS`). `sample` is
the first stage that exercises DSV write, SRV transition, readback, command-list
restore, release, and reacquire together.

`teardown` and every later stage deliberately allocate and destroy real runtime
images. `frame` and `unity` also take over the active immersive session and
submit visible color. Do not run any of them concurrently with another OpenXR
application.

Start with the GPU-free bootstrap check:

```sh
bash tools/d3d11-openxr-depth-probe/run-guarded.sh bootstrap
```

The WiVRn runtime must already be running for every later stage. The probe
loads `wineopenxr.dll`, invokes its private `wineopenxr_init_registry` entry
point, keeps the module loaded, and validates the resulting
`HKCU\\Software\\Wine\\XR` state, extension strings, and device identifiers.
This performs OpenXR instance/system discovery without creating an OpenXR
session. The runner also sets `PROTON_USE_XALIA=0`: a fresh Wine explorer would
otherwise launch Xalia, whose OpenVR provider creates a competing XRizer
session during the preflight. The runner launches the probe directly rather
than through Wine's Steam stub, which does not forward this Unix-path
executable. It uses Proton's `runinprefix` verb, which launches the target
directly through Wine without Proton's `umu.exe` game wrapper. The wrapper's
asynchronous `vrclient_init_registry` path would otherwise duplicate the
probe's synchronous OpenXR-only bootstrap. `runinprefix` also avoids the
default `waitforexitandrun` behavior, which blocks before the probe starts
whenever another process is alive in the Meta prefix. The runner
passes the Meta Proton compatibility-data directory to UMU, not its inner
`pfx` directory; UMU maps that root to `STEAM_COMPAT_DATA_PATH` and Proton then
selects the existing `pfx` child. The runner does not start, restart, or deploy
WiVRn or kill any Wine process.

`runinprefix` deliberately skips Proton's normal game setup, including its
DXVK DLL selection. The runner therefore appends
`d3d11=n;dxgi=n` to the existing `WINEDLLOVERRIDES`, explicitly verifies
`IDXGIVkInteropDevice2` on the created device, and refuses to call
`xrCreateSession` if that private DXVK interface is unavailable. Without that
override, Wine's built-in D3D11 implementation can create a device but cannot
expose the private interop interface; WineOpenXR consequently returns
`XR_ERROR_VALIDATION_FAILURE` from `xrCreateSession` before creating a
swapchain or submitting GPU work.

When staged color is requested, the runner enables only WineOpenXR's Wine debug
channel and saves the entire UMU process output as `runner.stdout.log` and
`runner.stderr.log` alongside the probe-owned logs. Staged color stages are not
certified from the environment flag alone: the runner requires WineOpenXR's
session-activation diagnostic and GPU-timestamped proxy-to-transport copies,
and rejects either a failed copy submission or an unsupported color-swapchain
fallback. Stereo `frame` and `unity` stages require at least two measured
copies, covering both recreated eye swapchains. The `unity` stage refuses to
start unless `WINEOPENXR_STAGED_CROSS_GPU_COLOR=1` is explicit. The captured
diagnostics also preserve the first/average/maximum copy time for later cost
analysis.

Projection stages follow Monado's frame-driven session transition contract.
After beginning at `READY`, the probe interleaves event draining with complete
`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` cycles; a non-renderable frame is
ended empty before polling for the resulting `SYNCHRONIZED`, `VISIBLE`, and
`FOCUSED` transitions. Readiness has a ten-second wall-clock bound and requires
at least `VISIBLE` before drawing even when `shouldRender` is true. Once the
first frame renders, the actual workload retains its three-second and 120-frame
bounds. The probe sleeps briefly after an immediately non-renderable frame and
rate-limits diagnostics instead of exhausting a fixed readiness attempt count.
`STOPPING` is paired with `xrEndSession`; `EXITING` and `LOSS_PENDING` abort with
normal resource cleanup.

The runner records WiVRn from a journal cursor through a two-second teardown
drain. New WiVRn builds identify every asynchronous client-fence warning by IPC
client, PID, application, frame, and consecutive timeout count. The runner
selects the final probe client created after Proton/xrizer and wineopenxr's
temporary preflight clients, ignores warnings from HypXRland and hypxrhud, and
fails only a terminal fence error or five consecutive 100 ms waits attributed
to that client. A legacy untagged warning is reported as attribution
unavailable rather than incorrectly blamed on the probe. Kernel Xids, a
missing PASS marker, nonzero process status, or incomplete cleanup still fail
independently.

The `bootstrap` stage runs the in-process OpenXR cache preflight but exits
before the probe creates a session or D3D11 device. The build links the MinGW
runtime statically so the target reaches `main()` without relying on undeployed
`libstdc++`, `libgcc`, or `libwinpthread` DLLs.

The hermetic contract regression does not access a GPU. It checks the resource
and view format family, the actual-image versus DXVK usage superset, and two
acquire/DSV/SRV/restore/release layout cycles against the edited sources:

```sh
MONADO_SRC=/path/to/monado \
  python3 tools/d3d11-openxr-depth-probe/test_contract.py
# Or: python3 tools/d3d11-openxr-depth-probe/test_contract.py --monado-src /path/to/monado
```

## Experimental staged cross-GPU color

`WINEOPENXR_STAGED_CROSS_GPU_COLOR=1` enables an experimental D3D11-only
swapchain path. It is disabled by default and is intended to test—not assume—the
hypothesis that rendering to an imported linear color image alongside a
client-local optimal depth image triggers the NVIDIA fault.

The WineOpenXR PE and Unix libraries are one private-ABI pair and must always
be built and deployed together from the same prepared Wine tree. Public PE
exports, ELF `NEEDED` entries, and SONAME do not establish compatibility: the
generated Unix dispatch table can change without any of those changing. An
ordinal-zero initialization handshake now verifies a shared ABI magic and the
exact generated call count before any later Unix call. Never reuse an older
`wineopenxr.so` with a newly built PE, or deploy either half alone.

For eligible color swapchains, WineOpenXR exposes ordinary DXVK-owned,
optimal-tiling textures to the application while retaining the runtime's
transport images. On release it flushes DXVK, locks the DXVK submission queue,
copies every mip and array layer into the index-matched transport image, restores
both boundary layouts, and then releases the transport image to the runtime.
The application descriptor remains unchanged; only the host swapchain gains
`XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT`.

The staged path currently rejects depth, multisampled, cube/face, and protected
swapchains. Proxy and transport formats must match exactly, and DXVK must create
an optimal-tiling proxy with transfer-source usage. Any failed capability check
returns a diagnostic rather than silently selecting a different format. Static
swapchains are copied on their single release and remain valid afterward.

When Vulkan timestamps are supported on the DXVK submission queue, WineOpenXR
logs the first copy and every 300th copy with last/average/maximum GPU duration.
This instrumentation is the source of measured copy cost; no performance number
should be claimed until the path has run on the target RTX 5070 topology.

The CPU-only state and source contract is:

```sh
python3 tools/d3d11-openxr-depth-probe/test_staged_color_contract.py
```

It covers FIFO index coupling, repeated and static-image lifecycles, layout
restoration, all-mip/all-layer recording, eligibility gates, and the required
flush/lock/copy/host-release ordering. It does not establish that mixed tiling
is the root cause; the attended discriminator ladder must do that before this
workaround is enabled for Walkabout.

The standalone native Vulkan probe checks the copy primitive independently of
OpenXR, Wine, and DXVK. It creates an optimal-tiling source and a mapped
linear-tiling destination, copies the exact format, and verifies every pixel in
three mips and three array layers. Both `R8G8B8A8_UNORM` and
`R8G8B8A8_SRGB` run through a repeated lifecycle and a one-copy static-style
lifecycle. Exit 77 means the selected device cannot provide a required image or
host-visible memory capability; linear sampled and color-attachment features
are printed for context but are not required by the transport destination.

Build it with the system Vulkan loader and headers:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
  tools/d3d11-openxr-depth-probe/staged-color-vulkan.cpp \
  -lvulkan -o /tmp/staged-color-vulkan
```

The optional argument is the physical-device index printed by the probe. For
example, after selecting the desired Vulkan ICD(s):

```sh
/tmp/staged-color-vulkan 0
```

This is a real GPU test. Do not run it during an attended XR session or while a
faulted NVIDIA channel is still active. A passing copy/readback establishes the
local optimal-to-linear primitive only; it does not validate WineOpenXR queue
ordering, cross-GPU import, or the mixed color/depth failure hypothesis.

## Guarded Walkabout launch

The separate harness validates the active WiVRn service, an established
headset transport, the live compositor's focused/visible/present state, the
complete locally built Heroic Meta runner, the exact matched WineOpenXR PE/Unix
pair,
the native DXVK payload and prefix copies, and an idle Walkabout process before
launch. It also pins build 780 and verifies that only the two required
non-Oculus compatibility bytes are changed while both known depth paths remain
enabled:

```sh
bash tools/d3d11-openxr-depth-probe/run-walkabout-guarded.sh check
ALLOW_LIVE_WALKABOUT=1 \
  bash tools/d3d11-openxr-depth-probe/run-walkabout-guarded.sh run
```

`run` performs exactly one bounded Heroic protocol launch. The installed
`/opt/Heroic/heroic` package is deliberately not used: its upstream bundle does
not contain the custom Meta runner. The harness instead verifies the hash of
the custom main bundle, the complete electron-vite build tree, its Electron
host, and its package manifest. It refuses to run while either installed or
custom Heroic owns the single-instance lock. The local Electron host is given
the pinned built renderer through `ELECTRON_RENDERER_URL`; this preserves the
real Heroic configuration and reaches `frontendReady` without relying on the
missing source-tree `public/index.html`.

The staging flag,
native D3D11/DXGI overrides, and a run-specific DXVK log directory are passed
to the Heroic Electron process and inherited by the official Meta launch
chain. The harness identifies the exact Walkabout PID, saves only the relevant
OpenXR/DXVK environment keys, and waits for a fresh `Player.log` that reports
`Shared Depth Buffer: Yes` plus a non-null depth texture request before
starting the 20-second health window. At that point it snapshots the process
maps and requires the hash-verified WineOpenXR PE/Unix pair and native
D3D11/DXGI files to be mapped. The exact DXVK D3D11 and DXGI logs must identify
the NVIDIA adapter. Proton may map its own WineOpenXR PE path or the
byte-identical prefix copy; both paths are pinned before launch, and the exact
one observed in the process map is recorded and hashed.

The current A/B harness keeps only OVRPlugin's non-Oculus-runtime capability
patch. It requires the original, unmodified `OculusXRPlugin.dll` and refuses
the old `test ecx,ecx` -> `xor ecx,ecx` patch that forced separate per-eye
textures. The run requires Unity to request Single Pass Instanced, records
whether both Unity and the Oculus eye layer actually select `Texture Array` or
fall back to `Separate`, and retains shared depth plus the staged cross-GPU
color path in either case. This distinguishes “the byte patch forced Multi
Pass” from “the unmodified plugin/runtime selected Multi Pass itself.”
`HEROIC_ROOT` may point at a controlled shadow build for this A/B; the four
`WALKABOUT_EXPECTED_HEROIC_{MAIN,BUILD,ELECTRON,PACKAGE}_SHA256` variables must
then pin its exact artifacts rather than weakening the existing checks.

The kernel Xid count and exact Unity-client disconnect are checked throughout.
On every outcome the harness first stops its recorded Heroic launcher, then stops
only the exact Meta prefix with GE-Proton's wineserver, drains diagnostics for
two seconds, and then attributes terminal or persistent fence waits from the
cursor-scoped WiVRn log to the exact Unity PID. A late Xid or unattributable fence result overrides apparent
success. Heroic output, filtered environment and maps, DXVK logs, OAF output,
the final `Player.log`, process snapshots, hashes, and a machine-readable
classification are retained below
`${XDG_STATE_HOME:-$HOME/.local/state}/hypxrland/walkabout-d32s8`.

The source-contract test does not need a runtime or GPU. `bootstrap` does not
need a headset and submits no probe rendering, though Proton's VR preflight may
initialize runtime helper graphics. Swapchain stages need the real NVIDIA
client device and WiVRn;
`frame`, `unity`, and Walkabout additionally need a live focused immersive
session. A headless Monado compositor can validate API lifecycle and failure
handling, but it cannot reproduce the NVIDIA Xid because that fault is in the
proprietary NVIDIA queue executing DXVK's imported-image workload.
