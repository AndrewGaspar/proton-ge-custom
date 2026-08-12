# D3D11 OpenXR D32S8 probe

This probe reproduces the OVRPlugin-facing path through the Windows OpenXR
loader, WineOpenXR's private DXVK interop, and the exact GE-Proton10-32 DXVK
deployment. It requests a 2064x2208 `DXGI_FORMAT_D32_FLOAT_S8X24_UINT`
swapchain with `DEPTH_STENCIL_ATTACHMENT | SAMPLED` usage.

Stages are intentionally ordered from lowest risk to highest:

1. `enumerate`: create and enumerate the swapchain images, then print the
   D3D11 resource descriptor.
2. `views`: additionally create the OVR-style typed DSV and depth-plane SRV.
3. `clear`: run two acquire/wait, depth/stencil clear, event-query fence, and
   release cycles.
4. `sample`: additionally sample the cleared depth plane through the typed SRV,
   copy the value to a staging buffer, and verify readback in both cycles.

The guarded runner compares kernel Xid counts before and after every stage and
returns 99 after any new NVIDIA Xid. It never kills Wine or a wineserver.

```sh
bash tools/d3d11-openxr-depth-probe/build.sh
bash tools/d3d11-openxr-depth-probe/run-guarded.sh enumerate
bash tools/d3d11-openxr-depth-probe/run-guarded.sh views
bash tools/d3d11-openxr-depth-probe/run-guarded.sh clear
bash tools/d3d11-openxr-depth-probe/run-guarded.sh sample
```

Run the stages in that order and stop after any failure or exit 99. `enumerate`
must report resource format 19 (`DXGI_FORMAT_R32G8X24_TYPELESS`). `sample` is
the first stage that exercises DSV write, SRV transition, readback, command-list
restore, release, and reacquire together.

The WiVRn runtime must already be running. The runner does not start, restart,
or deploy any component.

The hermetic contract regression does not access a GPU. It checks the resource
and view format family, the actual-image versus DXVK usage superset, and two
acquire/DSV/SRV/restore/release layout cycles against the edited sources:

```sh
MONADO_SRC=/path/to/monado \
  python3 tools/d3d11-openxr-depth-probe/test_contract.py
# Or: python3 tools/d3d11-openxr-depth-probe/test_contract.py --monado-src /path/to/monado
```
