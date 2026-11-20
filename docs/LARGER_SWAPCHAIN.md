# Vulkan swapchains larger than their window (Wayland, opt-in)

Goal: a game presents a 2x-wide side-by-side image (e.g. 3840x1080) into a
1920x1080 window and the full-resolution buffer reaches the compositor, which
scales it into the window. HypXRland shows a stereo-declared window per eye by
cropping each half of the surface's buffer (including a covering subsurface
such as Wine's client surface), so each eye gets full resolution
(hypxrland docs/openxr/05-configuration.md §8.6, `sbs` = 2x buffer in a 1x box).

## Switch

`WINE_VULKAN_ALLOW_LARGER_SWAPCHAIN=1` in the game's environment. It takes
effect only on Vulkan instances that use `VK_KHR_wayland_surface`, which means
the Wine Wayland driver (`PROTON_ENABLE_WAYLAND=1`). With X11, or without the
variable, nothing changes. An X window's buffer is the window's size, so the
X11 driver can't present a larger buffer.

## Chain (static reading of the GE-Proton11-12 protonprep tree)

| Layer | Default behaviour | With the switch |
|---|---|---|
| win32u `adjust_surface_capabilities` | min = max = current = client rect | current = min = client rect; **max = max(host max, client rect)**. An empty client rect still reports max = 0, so minimized-window detection keeps working |
| win32u `vkCreateSwapchainKHR` | host swapchain at the app's `imageExtent` (raised to the host minimum); asks for `VK_PRESENT_SCALING_STRETCH` when the extent differs from the client rect | same, except it doesn't request STRETCH for a swapchain that covers the client rect (the compositor scales it) |
| win32u acquire/present | `VK_SUBOPTIMAL_KHR` whenever the extent differs from the client rect | `VK_SUBOPTIMAL_KHR` only when the swapchain doesn't cover the client rect (`swapchain_extents_fit()`) |
| winewayland client surface | `wp_viewport` destination = client rect, no source set | unchanged: the whole buffer is scaled into the client rect |
| vkd3d-proton DXGI swapchain | Vulkan swapchain at `currentExtent`; the present blit scales the DXGI back buffer into it | when the surface reports max > current and the DXGI buffer is at least the surface size, the Vulkan swapchain is created at the **DXGI buffer size**. ResizeBuffers forces a recreate in that mode |
| DXVK `Presenter::pickImageExtent` (D3D11/D3D10, DXGI) | returns `currentExtent` whenever it is defined; the present blit scales the back buffer into the window-sized swapchain | when the surface reports max > current and the preferred extent (the DXGI buffer size, set by `D3D11SwapChain::ChangeProperties` → `Presenter::setSurfaceExtent`) is at least `currentExtent` in both axes, the Vulkan swapchain is created at the **buffer size**, clamped to `maxImageExtent`. The blitter then copies 1:1 (static: `DxvkSwapchainBlitter` sets `needsBlit` only when the source and destination extents differ). ResizeBuffers already recreates the swapchain through `setSurfaceExtent` |

Native Wayland reports `currentExtent = 0xFFFFFFFF`. We don't use that
here. Keeping `currentExtent` at the client rect means applications that follow
it (most Vulkan games, and unpatched DXVK) behave exactly as before. Only an
application that asks for a larger extent gets one.

## Patches

- `patches/wine-hotfixes/pending/win32u-allow-larger-wayland-swapchain.patch`,
  applied last in `protonprep-valve-staging.sh`. It was generated against the
  fully patched tree.
- `patches/vkd3d-proton/0004-vkd3d-use-the-buffer-size-when-the-surface-accepts-larger-images.patch`.
  It isn't gated separately: a surface with max == current, which covers every
  surface without the Wine switch, keeps the old path.
- DXVK: commit fffc4f68 ("dxvk: use the back buffer size when the surface
  accepts larger images") on the dxvk fork's `custom-ge11` branch, pinned by
  the `dxvk` gitlink. It changes only `Presenter::pickImageExtent` in
  `src/dxvk/dxvk_presenter.cpp`, and like the vkd3d-proton patch it isn't
  gated separately. The fork carries its own DXVK changes as submodule commits
  (the OpenXR interop ones before it), so there is no `patches/dxvk/` copy;
  the GE patches in `patches/dxvk/` still apply on top of it unchanged (checked
  by applying them in protonprep order).
  D3D9 passes the window size as its preferred extent, so it keeps the old
  path.

## Limits

- The managed cross-process child-rendering path (`wineland-child-rendering`,
  off-screen child windows exported over dmabuf) and locally retired background
  presenters keep exact-size semantics. A larger swapchain there still reports
  `VK_SUBOPTIMAL_KHR`. This feature targets toplevel windows that present
  through the host swapchain.
- With the switch on, a swapchain that is larger than a window that has shrunk
  is scaled down instead of being flagged suboptimal. DXGI applications resize on
  WM_SIZE, so they are unaffected.
- D3D12 (vkd3d-proton) and D3D11/D3D10 (DXVK) use the larger extent. D3D9
  and D3D8 (DXVK) do not: their preferred extent is the window size. A native
  Vulkan game only gets one if it asks for an extent above `currentExtent`.

## Verification (live, 2026-11-19)

The test build was `GE-Proton11-12-hypxr`: a reflink copy of GE-Proton11-12
with a host-built `win32u.so` and mingw-built `d3d12core.dll` (x86_64 and
i386) swapped in. The test program, `tools/larger-swapchain-smoke/sbstest.c`,
is a D3D12 window of about 1920x1058 (Hyprland floating) with a DXGI
swapchain twice as wide. Its left and right halves differ, and it follows
WM_SIZE with `ResizeBuffers(2*w, h)`. It ran under `proton run` with a scratch
compat dir, `PROTON_ENABLE_WAYLAND=1` and `WAYLAND_DEBUG=1`.

| Run | dmabuf `create_immed` sizes | client-surface viewport | vkd3d-proton |
|---|---|---|---|
| hypxr, switch on, 2x buffers | 3840x1058, 3840x1054 | `set_destination(1920, 1058)`, no `set_source` | "Surface accepts larger images, using buffer size 3840x1058 instead of surface size 1920x1058" |
| hypxr, switch off, 2x buffers | 1920x1058, 1920x1054 | same | swapchain at surface size (blit downscale), as before |
| original GE-Proton11-12, switch on, 2x buffers | 1920x1058, 1920x1054 | same | as before: the switch has no effect there |
| hypxr, switch on, 1x buffers | 1920x1058, 1920x1054 | same | unchanged path |

Every run exited cleanly. Hyprland resized the floating window once or twice per run
(1058 <-> 1054 high). Each resize recreated the swapchain at the new
buffer size. No `VK_SUBOPTIMAL_KHR` loops occurred. The compositor-side scaling
and HypXRland's per-eye crop have not been checked visually.

### D3D11 through DXVK (live, 2026-11-19)

The test build was the same `GE-Proton11-12-hypxr`, now also carrying DXVK built
from dxvk fffc4f68 with the GE `patches/dxvk/` set applied
(`v3.1.1-177-gfffc4f6846f632f-GE-Proton(dirty)-hypxr`, x86_64 and i386
`d3d11`, `dxgi`, `d3d10core`, `d3d9`, `d3d8`). The original GE-Proton11-12
ships `v3.1.1-174-gf86377680682448-GE-Proton(dirty)`. The DLLs were built
with the host's mingw-w64 GCC 16.2 and GE's CFLAGS, `--buildtype=plain --strip`,
so they link UCRT (`api-ms-win-crt-*`), where GE's GCC 14 builds link
`msvcrt.dll`. The test program,
`tools/larger-swapchain-smoke/sbstest11.c`, is the D3D11 version of
`sbstest.c`: a FLIP_DISCARD DXGI swapchain twice as wide as the client rect,
with halves painted differently by `ClearView`, and `ResizeBuffers(2*w, h)` on
WM_SIZE. It ran under `proton run` with scratch compat and client-install dirs on
tmpfs, `PROTON_ENABLE_WAYLAND=1`, `WAYLAND_DEBUG=1` and `DXVK_LOG_LEVEL=info`.
A runtime Hyprland rule kept it on an unseen workspace without focus. Hyprland
tiled it to 2560x1574 (monitor scale 1.25, so the window box is 2048x1259).

| Run | dmabuf `create_immed` sizes | client-surface viewport | DXVK |
|---|---|---|---|
| hypxr, switch on, 2x buffers (8 runs) | 5120x1574 | `set_destination(2048, 1259)`, no `set_source` | "Presenter: Surface accepts larger images, using buffer size 5120x1574 instead of surface size 2560x1574" on every swapchain creation |
| hypxr, switch on, 2x, window floated mid-run | 5120x1574, then 4800x1350 | `set_destination(1920, 1080)` after the float | ResizeBuffers to 4800x1350 recreated the swapchain at the new buffer size |
| hypxr, switch off, 2x buffers | 2560x1574 | same | swapchain at surface size (blit downscale), as before |
| hypxr, switch on, 1x buffers | 2560x1574 | same | unchanged path |
| original GE-Proton11-12, switch on / off, 2x buffers | 2560x1574 | same | as before: the switch has no effect there |

The `set_source` calls in the log are on the toplevel (shm) surface, not on the
client surface that carries the dmabufs. No `VK_SUBOPTIMAL_KHR` was returned.
Every configuration, including the original GE-Proton11-12 and the switch off,
got about one frame per second and a `VK_ERROR_OUT_OF_DATE_KHR` every few
seconds (Wine: "present wait stalled"), because the workspace was never shown.
That comes from the test setup, not from the patch.

All but one run exited 0. The first switch-on run on a fresh prefix failed its
first `vkCreateSwapchainKHR` at 5120x1574 with `VK_ERROR_INITIALIZATION_FAILED`,
after which the test exits on the failed Present. The Wine log for that run was
not kept. It didn't recur in 10 later switch-on runs, including one on another
fresh prefix. The compositor-side scaling and HypXRland's per-eye crop have not
been checked visually for D3D11 either.
