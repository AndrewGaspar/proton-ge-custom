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

Native Wayland reports `currentExtent = 0xFFFFFFFF`. We don't use that
here. Keeping `currentExtent` at the client rect means applications that follow
it (DXVK, most Vulkan games) behave exactly as before. Only an application that
asks for a larger extent gets one.

## Patches

- `patches/wine-hotfixes/pending/win32u-allow-larger-wayland-swapchain.patch`,
  applied last in `protonprep-valve-staging.sh`. It was generated against the
  fully patched tree.
- `patches/vkd3d-proton/0004-vkd3d-use-the-buffer-size-when-the-surface-accepts-larger-images.patch`.
  It isn't gated separately: a surface with max == current, which covers every
  surface without the Wine switch, keeps the old path.

## Limits

- The managed cross-process child-rendering path (`wineland-child-rendering`,
  off-screen child windows exported over dmabuf) and locally retired background
  presenters keep exact-size semantics. A larger swapchain there still reports
  `VK_SUBOPTIMAL_KHR`. This feature targets toplevel windows that present
  through the host swapchain.
- With the switch on, a swapchain that is larger than a window that has shrunk
  is scaled down instead of being flagged suboptimal. DXGI applications resize on
  WM_SIZE, so they are unaffected.
- Not covered: DXVK (D3D11) still follows `currentExtent`, so a D3D11 game
  keeps window-sized buffers. Only vkd3d-proton (D3D12) uses the larger
  extent.

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
