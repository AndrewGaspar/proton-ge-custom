# D3D11 OpenXR D32S8 probe

This probe reproduces the OVRPlugin-facing path through the Windows OpenXR
loader, WineOpenXR's private DXVK interop, and the exact GE-Proton10-32 DXVK
deployment. It requests a 2064x2208 `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`
swapchain with `DEPTH_STENCIL_ATTACHMENT | SAMPLED` usage.

Stages are intentionally ordered from lowest risk to highest:

1. `bootstrap`: prove that UMU, Proton, and Wine's volatile VR registry are
   ready, then exit before the probe loads OpenXR or creates a D3D11 device.
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
9. `draw-color`: draw to a real cross-GPU RGBA8 color swapchain without a
   depth attachment. This isolates the imported color attachment and its
   FOREIGN ownership/layout path.
10. `draw-depth`: execute a depth-writing draw against the client-local D32S8
   swapchain with no color attachment. This differs from `clear` by exercising
   an actual graphics pipeline and depth tests.
11. `draw-native-pair`: bind an ordinary DXVK-created NVIDIA color texture
   alongside the borrowed client-local D32S8 swapchain image. This separates
   mixed attachment provenance from the format and render-pipeline contract.
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

Before every draw or frame stage, the runner removes any inherited
`HYPRLAND_INSTANCE_SIGNATURE`, resolves `hyprctl instances`, requires exactly
one live compositor, and queries that explicit signature. It refuses to render
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
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-color
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-depth
bash tools/d3d11-openxr-depth-probe/run-guarded.sh draw-native-pair
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

The `bootstrap` stage runs the in-process VR preflight but exits before the
probe's OpenXR or D3D11 initialization. Proton's VR preflight can still
initialize its own runtime helpers and enumerate graphics devices. The build
links the MinGW runtime statically so the target reaches `main()` without
relying on undeployed `libstdc++`, `libgcc`, or `libwinpthread` DLLs.

The hermetic contract regression does not access a GPU. It checks the resource
and view format family, the actual-image versus DXVK usage superset, and two
acquire/DSV/SRV/restore/release layout cycles against the edited sources:

```sh
MONADO_SRC=/path/to/monado \
  python3 tools/d3d11-openxr-depth-probe/test_contract.py
# Or: python3 tools/d3d11-openxr-depth-probe/test_contract.py --monado-src /path/to/monado
```

## Guarded Walkabout launch

The separate harness validates the active WiVRn service, an established
headset transport, a latest presence event of "headset is worn", the locally
built Heroic Meta runner, the installed game, and an idle Walkabout process
before launch:

```sh
bash tools/d3d11-openxr-depth-probe/run-walkabout-guarded.sh check
ALLOW_LIVE_WALKABOUT=1 \
  bash tools/d3d11-openxr-depth-probe/run-walkabout-guarded.sh run
```

`run` performs exactly one bounded Heroic protocol launch. It snapshots the
kernel Xid count, watches for Unity's WiVRn connection and client-fence
timeouts, stops immediately on either failure, and finally stops only the
exact Meta prefix with GE-Proton's own wineserver. Logs, the final `Player.log`,
process snapshots, and a machine-readable classification are retained below
`${XDG_STATE_HOME:-$HOME/.local/state}/hypxrland/walkabout-d32s8`.

The source-contract test does not need a runtime or GPU. `bootstrap` does not
need a headset and submits no probe rendering, though Proton's VR preflight may
initialize runtime helper graphics. Swapchain stages need the real NVIDIA
client device and WiVRn;
`frame`, `unity`, and Walkabout additionally need a live focused immersive
session. A headless Monado compositor can validate API lifecycle and failure
handling, but it cannot reproduce the NVIDIA Xid because that fault is in the
proprietary NVIDIA queue executing DXVK's imported-image workload.
