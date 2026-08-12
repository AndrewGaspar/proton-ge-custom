# D3D11 OpenXR D32S8 probe

This probe reproduces the OVRPlugin-facing path through the Windows OpenXR
loader, WineOpenXR's private DXVK interop, and the exact GE-Proton10-32 DXVK
deployment. It requests a 2064x2208 `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`
swapchain with `DEPTH_STENCIL_ATTACHMENT | SAMPLED` usage.

Stages are intentionally ordered from lowest risk to highest:

1. `bootstrap`: prove that UMU, Proton, and Wine's volatile VR registry are
   ready, then exit before loading OpenXR or creating a D3D11 device.
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
9. `draw`: create a real cross-GPU RGBA8 color swapchain alongside D32S8,
   validate both imported D3D11 resource descriptors, bind a typed RTV and DSV,
   clear them, execute a depth-writing VS/PS draw, complete an event-query
   fence, and release both images. This is the first stage that puts the
   cross-GPU color image and client-local depth image in one graphics render
   pass.
10. `frame`: begin an active stereo OpenXR session, create separate color/depth
   pairs for both eyes, discard/clear/draw each pair without an application
   fence before release, and submit them through `xrEndFrame` as a projection
   layer with `XrCompositionLayerDepthInfoKHR` chains for up to three seconds
   (at most 120 attempts). It then verifies that a post-submit D3D11 event fence
   completes.
11. `unity`: require three images in each per-eye color and depth swapchain,
    destroy the resulting six texture pairs before rendering, recreate all six
    pairs, then run the same discard/draw/projection-depth frame loop. This
    matches the two `XRTextureManager` generations in Walkabout's `Player.log`.

The guarded runner compares kernel Xid counts before and after every stage and
returns 99 after any new NVIDIA Xid. Each stage also has a bounded timeout that
terminates only the probe's UMU process group; the post-run Xid check still runs
after a timeout. It never broadly kills Wine or a wineserver.

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
loads `vrclient_x64.dll`, invokes Proton's `vrclient_init_registry` entry point,
and keeps the module loaded while waiting for `HKCU\\Software\\Wine\\VR` state
1. This is the same preflight used by Heroic, but keeping it in the probe
process preserves the volatile registry state without Heroic's long-lived
companion. The runner launches the probe directly rather than through Wine's
Steam stub, which does not forward this Unix-path executable. It uses a
nonempty numeric game ID and Proton's `run` verb: the default
`waitforexitandrun` blocks before the probe starts whenever another process is
alive in the Meta prefix. The runner
passes the Meta Proton compatibility-data directory to UMU, not its inner
`pfx` directory; UMU maps that root to `STEAM_COMPAT_DATA_PATH` and Proton then
selects the existing `pfx` child. The runner does not start, restart, or deploy
WiVRn or kill any Wine process.

The `bootstrap` stage runs the in-process VR preflight but exits before OpenXR
or D3D11 initialization. The build links the MinGW runtime statically so the
target reaches `main()` without relying on undeployed `libstdc++`, `libgcc`, or
`libwinpthread` DLLs.

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
