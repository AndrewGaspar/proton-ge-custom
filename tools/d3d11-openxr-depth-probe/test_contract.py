#!/usr/bin/env python3

import argparse
import importlib.util
import os
import pathlib
import sys
import unittest


PROTON_ROOT = pathlib.Path(__file__).resolve().parents[2]

argument_parser = argparse.ArgumentParser(add_help=False)
argument_parser.add_argument("--monado-src")
arguments, unittest_arguments = argument_parser.parse_known_args()
sys.argv[1:] = unittest_arguments
monado_source = arguments.monado_src or os.environ.get("MONADO_SRC")
if not monado_source:
    raise SystemExit("set MONADO_SRC or pass --monado-src PATH")
MONADO_ROOT = pathlib.Path(monado_source).resolve()

attribute_spec = importlib.util.spec_from_file_location(
    "attribute_fence_log",
    pathlib.Path(__file__).with_name("attribute_fence_log.py"),
)
attribute_fence_log = importlib.util.module_from_spec(attribute_spec)
attribute_spec.loader.exec_module(attribute_fence_log)

ATTACHMENT = "DEPTH_STENCIL_ATTACHMENT_OPTIMAL"
READ_ONLY = "DEPTH_STENCIL_READ_ONLY_OPTIMAL"
UNDEFINED = "UNDEFINED"


class LayoutState:
    """Small model of the Monado acquire/release and DXVK layout contract."""

    def __init__(self):
        self.actual = UNDEFINED
        self.tracked = ATTACHMENT

    def monado_acquire(self):
        # UNDEFINED is a discard transition; it need not equal the old contents'
        # layout. Monado completes this queue submission before the app uses it.
        self.actual = ATTACHMENT

    def dxvk_transition(self, destination):
        self.assert_tracking_matches_actual()
        self.actual = destination
        self.tracked = destination

    def dxvk_restore(self):
        self.dxvk_transition(ATTACHMENT)

    def monado_release(self):
        # Under synchronization2 this equal-layout barrier is a memory dependency,
        # not a layout transition.
        self.assert_tracking_matches_actual()
        self.assertEqual(self.actual, ATTACHMENT)

    def assert_tracking_matches_actual(self):
        if self.actual != self.tracked:
            raise AssertionError(f"actual {self.actual} != tracked {self.tracked}")

    def assertEqual(self, left, right):
        if left != right:
            raise AssertionError(f"{left} != {right}")


def model_frame_pump(initial_events, frames):
    """Model READY/begin followed by interleaved frame and event pumping."""
    if "READY" not in initial_events:
        return "not-begun"
    state = initial_events[-1]
    for should_render, events_after_end in frames:
        if any(state in ("STOPPING", "EXITING", "LOSS_PENDING")
               for state in events_after_end):
            return "stopped"
        if events_after_end:
            state = events_after_end[-1]
        if should_render and state in ("VISIBLE", "FOCUSED"):
            return "rendered"
    return "timeout"


class D32S8ImportContractTest(unittest.TestCase):
    def test_session_lifecycle_interleaves_frames_and_event_polls(self):
        self.assertEqual(model_frame_pump(["IDLE"], []), "not-begun")
        self.assertEqual(
            model_frame_pump(
                ["IDLE", "READY"],
                [
                    (False, ["SYNCHRONIZED"]),
                    (False, ["VISIBLE", "FOCUSED"]),
                    (True, []),
                ],
            ),
            "rendered",
        )
        self.assertEqual(
            model_frame_pump(["READY"], [(False, ["STOPPING"])]),
            "stopped",
        )
        self.assertEqual(
            model_frame_pump(["READY"], [(False, [])] * 3),
            "timeout",
        )
        self.assertEqual(
            model_frame_pump(["READY"], [(True, ["SYNCHRONIZED"])]),
            "timeout",
        )

    def test_fence_attribution_ignores_other_clients(self):
        log = """
 id: 1
 application_name: 'Hyprland'
 id: 13
 application_name: 'd3d11-openxr-depth-probe'
 Client fence attribution enabled: client=13 pid=42 app='d3d11-openxr-depth-probe'
 Waiting on client fence timed out > 100ms! client=1 pid=1847 app='Hyprland' frame=4 timeout_count=20
"""
        self.assertEqual(attribute_fence_log.classify(log, "d3d11-openxr-depth-probe", 5), 0)

    def test_fence_attribution_rejects_persistent_target_wait(self):
        log = """
 id: 13
 application_name: 'd3d11-openxr-depth-probe'
 Client fence attribution enabled: client=13 pid=42 app='d3d11-openxr-depth-probe'
 Waiting on client fence timed out > 100ms! client=13 pid=42 app='d3d11-openxr-depth-probe' frame=7 timeout_count=5
"""
        self.assertEqual(attribute_fence_log.classify(log, "d3d11-openxr-depth-probe", 5), 1)

    def test_fence_attribution_rejects_terminal_target_wait(self):
        log = """
 id: 13
 application_name: 'd3d11-openxr-depth-probe'
 Client fence attribution enabled: client=13 pid=42 app='d3d11-openxr-depth-probe'
 Fence waiting failed! client=13 pid=42 app='d3d11-openxr-depth-probe' frame=7 result=-2
"""
        self.assertEqual(attribute_fence_log.classify(log, "d3d11-openxr-depth-probe", 5), 1)

    def test_fence_attribution_flags_legacy_untagged_log(self):
        log = """
 id: 13
 application_name: 'd3d11-openxr-depth-probe'
 Waiting on client fence timed out > 100ms!
"""
        self.assertEqual(attribute_fence_log.classify(log, "d3d11-openxr-depth-probe", 5), 2)

    def test_fence_attribution_accepts_capable_runtime_without_stage_waits(self):
        log = """
 id: 13
 application_name: 'd3d11-openxr-depth-probe'
"""
        self.assertEqual(
            attribute_fence_log.classify(log, "d3d11-openxr-depth-probe", 5, True),
            0,
        )

    def test_fence_attribution_selects_exact_application_pid(self):
        log = """
 id: 20
 application_name: 'Unity'
 pid: 100
 Client fence attribution enabled: client=20 pid=100 app='Unity'
 Waiting on client fence timed out > 100ms! client=20 pid=100 app='Unity' frame=7 timeout_count=8
 id: 21
 application_name: 'Unity'
 pid: 200
 Client fence attribution enabled: client=21 pid=200 app='Unity'
"""
        self.assertEqual(
            attribute_fence_log.classify(log, "Unity", 5, False, 200),
            0,
        )
        self.assertEqual(
            attribute_fence_log.classify(log, "Unity", 5, False, 100),
            1,
        )

    def test_client_id_resolver_parses_timestamped_journal_pid(self):
        log = """
Aug 13 21:46:59.837998 framework wivrn-server[790066]:         id: 20
Aug 13 21:46:59.837998 framework wivrn-server[790066]:         application_name: 'Unity'
Aug 13 21:46:59.837998 framework wivrn-server[790066]:         pid: 1913667
"""
        self.assertEqual(
            attribute_fence_log.find_target_client_id(log, "Unity", 1913667),
            20,
        )

    def test_two_dsv_srv_cycles_keep_actual_and_tracked_layouts_equal(self):
        state = LayoutState()

        for _ in range(2):
            state.monado_acquire()
            state.assert_tracking_matches_actual()

            # The typed DSV write stays in the persistent attachment layout.
            state.assert_tracking_matches_actual()

            # A depth-plane SRV temporarily uses the read-only layout, then DXVK
            # restores borrowed/shared images before command submission ends.
            state.dxvk_transition(READ_ONLY)
            state.dxvk_restore()
            state.monado_release()

    def test_source_implements_contract(self):
        wineopenxr = (PROTON_ROOT / "wineopenxr/openxr_loader.c").read_text()
        dxvk = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_texture.cpp").read_text()
        probe = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/main.cpp").read_text()
        monado = (MONADO_ROOT / "src/xrt/compositor/client/comp_vk_client.c").read_text()
        multi = (MONADO_ROOT / "src/xrt/compositor/multi/comp_multi_compositor.c").read_text()
        ipc_server = (MONADO_ROOT / "src/xrt/ipc/server/ipc_server_handler.c").read_text()
        support = (MONADO_ROOT / "src/xrt/auxiliary/vk/vk_compositor_flags.c").read_text()
        allocator = (MONADO_ROOT / "src/xrt/auxiliary/vk/vk_image_allocator.c").read_text()

        self.assertIn("return DXGI_FORMAT_R32G8X24_TYPELESS;", wineopenxr)
        self.assertIn("desc.Format = dxgi_swapchain_resource_format", wineopenxr)
        self.assertIn("our_d3d11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;", wineopenxr)
        self.assertIn("our_d3d11[i].next = NULL;", wineopenxr)
        self.assertIn("our_d3d11[j].texture->lpVtbl->Release", wineopenxr)

        destroy = wineopenxr.split("XrResult WINAPI xrDestroySwapchain", 1)[1].split(
            "static D3D11_USAGE", 1
        )[0]
        self.assertLess(destroy.index("WaitForGpuIdle"), destroy.index("release_d3d11_resources"))
        self.assertLess(destroy.index("release_d3d11_resources"), destroy.index("UNIX_CALL(xrDestroySwapchain"))
        enter = destroy.index("EnterCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)")
        wait = destroy.index("WaitForGpuIdle")
        leave = destroy.index("LeaveCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)")
        release = destroy.index("release_d3d11_resources")
        self.assertLess(enter, wait)
        self.assertLess(wait, leave)
        self.assertLess(leave, release)
        self.assertEqual(destroy.count("EnterCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)"), 1)
        self.assertEqual(destroy.count("LeaveCriticalSection(&wine_swapchain->session->swapchain_destroy_lock)"), 1)
        self.assertIn("swapchain->images = NULL;", wineopenxr)
        self.assertIn("swapchain->image_count = 0;", wineopenxr)

        wine_interop = (PROTON_ROOT / "wineopenxr/dxvk-interop.h").read_text()
        dxvk_interop = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_interop.cpp").read_text()
        dxvk_interfaces = (PROTON_ROOT / "dxvk/src/dxgi/dxgi_interfaces.h").read_text()
        dxvk_device = (PROTON_ROOT / "dxvk/src/d3d11/d3d11_device.cpp").read_text()
        self.assertIn("0xa0,0x93,0x25", wine_interop)
        self.assertIn("IDXGIVkInteropDevice2 : public IDXGIVkInteropDevice1", dxvk_interfaces)
        self.assertIn("0xa0,0x93,0x25", dxvk_interfaces)
        self.assertIn("immediateContext->SynchronizeCsThread", dxvk_interop)
        self.assertIn("m_device->GetDXVKDevice()->waitForIdle", dxvk_interop)
        wait_for_idle = dxvk_interop.split("D3D11VkInterop::WaitForGpuIdle", 1)[1]
        self.assertLess(wait_for_idle.index("immediateContext->Flush"),
                        wait_for_idle.index("immediateContext->SynchronizeCsThread"))
        self.assertLess(wait_for_idle.index("immediateContext->SynchronizeCsThread"),
                        wait_for_idle.index("m_device->GetDXVKDevice()->waitForIdle"))
        self.assertIn("riid == __uuidof(IDXGIVkInteropDevice1)", dxvk_device)
        self.assertIn("riid == __uuidof(IDXGIVkInteropDevice2)", dxvk_device)

        self.assertIn("vkImage != VK_NULL_HANDLE", dxvk)
        self.assertIn("VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL", dxvk)
        self.assertIn("trackedLayout = imageInfo.layout;", dxvk)
        self.assertIn("Client fence attribution enabled: client=%u", multi)
        self.assertIn("timeout_count=%u", multi)
        self.assertIn("Client fence recovered after %u timeout(s)", multi)
        self.assertIn("server_xsi.debug_client_id = ics->client_state.id", ipc_server)
        self.assertIn("importImage(imageInfo, vkImage, trackedLayout", dxvk)
        self.assertIn("dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;", probe)
        self.assertIn("srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;", probe)
        self.assertIn("for (uint32_t cycle = 0; cycle < 2; cycle++)", probe)
        self.assertIn("constexpr uint32_t swapchainCount = 6;", probe)
        self.assertIn("context1->DiscardView", probe)
        self.assertIn("device->CreateRenderTargetView", probe)
        self.assertIn("D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE", probe)
        self.assertIn("context->OMSetRenderTargets", probe)
        self.assertIn("context->Draw(3, 0)", probe)
        self.assertIn("XrCompositionLayerDepthInfoKHR", probe)
        self.assertIn("xr.endFrame(session, &endInfo)", probe)
        self.assertIn("end_aborted_frame", probe)
        self.assertIn('xrEndFrame(abort recovery)', probe)
        self.assertIn("cleanup_projection_resources", probe)
        self.assertIn("constexpr uint32_t maxRenderedFrames = 120;", probe)
        self.assertIn('std::strcmp(stage, "unity")', probe)
        self.assertIn("colorImages != 6 || depthImages != 6", probe)
        self.assertIn("destroyed Unity-style first generation before rendering", probe)
        self.assertIn("std::chrono::seconds(3)", probe)
        self.assertIn("bool colorAcquired = false;", probe)
        self.assertIn("bool depthAcquired = false;", probe)
        self.assertIn("cleanup: destroying OpenXR session", probe)
        self.assertIn("cleanup_xr_d3d(xr, session, context, device, instance, loader)", probe)
        self.assertIn('log_phase("draw-depth acquire/wait begin")', probe)
        self.assertIn('"draw-depth event fence complete"', probe)
        self.assertIn('"draw-depth release complete"', probe)
        frame_loop = probe.split("static bool run_projection_frame", 1)[1].split(
            'std::printf("completed %u rendered projection frames', 1
        )[0]
        self.assertGreaterEqual(frame_loop.count("end_aborted_frame("), 3)
        self.assertIn('std::strcmp(stage, "draw")', probe)
        self.assertIn('std::strcmp(stage, "draw-color")', probe)
        self.assertIn('std::strcmp(stage, "draw-depth")', probe)
        self.assertIn('std::strcmp(stage, "draw-native-pair")', probe)
        self.assertIn('std::strcmp(stage, "draw-bind")', probe)
        self.assertIn('std::strcmp(stage, "draw-dsv-disabled")', probe)
        self.assertIn("bind_mixed_pair_without_draw", probe)
        self.assertIn("draw_mixed_pair_with_depth_disabled", probe)
        self.assertIn('std::strcmp(stage, "frame")', probe)
        self.assertIn('std::strcmp(stage, "teardown")', probe)
        self.assertIn('std::strcmp(stage, "recreate")', probe)
        self.assertIn('d3d11-openxr-depth-probe.stdout.log', probe)
        self.assertIn('d3d11-openxr-depth-probe.stderr.log', probe)
        self.assertIn('apiVersion = XR_MAKE_VERSION(1, 0, 0);', probe)
        self.assertNotIn('LoadLibraryA("vrclient_x64.dll")', probe)
        self.assertNotIn('vrclient_init_registry', probe)
        self.assertIn('LoadLibraryA("wineopenxr.dll")', probe)
        self.assertIn('GetProcAddress(wineOpenXr, "wineopenxr_init_registry")', probe)
        self.assertIn('RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\\\Wine\\\\XR"', probe)
        self.assertIn('state == 1', probe)
        self.assertIn('Wine OpenXR registry did not reach state 1', probe)
        self.assertIn('"openxr_vulkan_instance_extensions"', probe)
        self.assertIn('"openxr_vulkan_device_extensions"', probe)
        self.assertIn('"openxr_vulkan_device_vid"', probe)
        self.assertIn('"openxr_vulkan_device_pid"', probe)

        runner = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/run-guarded.sh").read_text()
        self.assertIn("expected='PASS: create and enumerate only'", runner)
        self.assertIn("draw) expected='PASS: paired color RTV and D32S8 DSV draw", runner)
        self.assertIn("draw-color) expected='PASS: imported cross-GPU color-only draw", runner)
        self.assertIn("draw-depth) expected='PASS: client-local D32S8 depth-only draw", runner)
        self.assertIn("draw-native-pair) expected='PASS: native NVIDIA color plus client-local D32S8 draw", runner)
        self.assertIn("draw-bind) expected='PASS: mixed imported color and local D32S8 bind without Draw", runner)
        self.assertIn("draw-dsv-disabled) expected='PASS: mixed pair color draw with DSV bound and depth disabled", runner)
        self.assertIn("frame) expected='PASS: stereo color/depth draw", runner)
        self.assertIn("unity) expected='PASS: Unity-style color/depth rebuild", runner)
        self.assertIn("UMU_USE_STEAM=0", runner)
        self.assertIn("PROTON_USE_XALIA=0", runner)
        self.assertIn('umu_env+=(WINEOPENXR_STAGED_CROSS_GPU_COLOR="$WINEOPENXR_STAGED_CROSS_GPU_COLOR")', runner)
        self.assertIn("umu_env+=(WINEDEBUG=-all,+openxr)", runner)
        self.assertIn("GAMEID=999999999", runner)
        self.assertIn("PROTON_VERB=runinprefix", runner)
        self.assertIn("probe_dll_overrides=${WINEDLLOVERRIDES:-}", runner)
        self.assertIn("probe_dll_overrides+='d3d11=n;dxgi=n'", runner)
        self.assertIn('umu_env+=(WINEDLLOVERRIDES="$probe_dll_overrides")', runner)
        self.assertNotIn("PROTON_VERB=run ", runner)
        self.assertIn("compat_data_dir=/home/ajg/Games/Heroic/Prefixes/MetaHorizon", runner)
        self.assertIn('WINEPREFIX="$compat_data_dir"', runner)
        self.assertNotIn("MetaHorizon/pfx", runner)
        self.assertIn("bootstrap) expected='PASS: Proton and Wine OpenXR bootstrap completed without creating an XR session'", runner)
        self.assertIn("timeout --kill-after=5s", runner)
        self.assertIn("env -u HYPRLAND_INSTANCE_SIGNATURE hyprctl instances -j", runner)
        self.assertIn("HYPRLAND_INSTANCE_SIGNATURE=\"$instance_signature\" hyprctl openxr status", runner)
        self.assertIn("umu_env=(env -u HYPRLAND_INSTANCE_SIGNATURE)", runner)
        self.assertIn('umu_env+=(HYPRLAND_INSTANCE_SIGNATURE="$instance_signature")', runner)
        self.assertIn('"${umu_env[@]}" WINEPREFIX="$compat_data_dir"', runner)
        self.assertLess(
            runner.index('umu_env+=(HYPRLAND_INSTANCE_SIGNATURE="$instance_signature")'),
            runner.index('"${umu_env[@]}" WINEPREFIX="$compat_data_dir"'),
        )
        self.assertIn("expected exactly one live Hyprland instance", runner)
        self.assertNotIn("timeout --foreground", runner)
        self.assertIn("frame|unity) stage_timeout=90s", runner)
        self.assertLess(runner.index('status=$?'), runner.index('after=$(journalctl'))
        self.assertLess(runner.index('after=$(journalctl'), runner.index('if (( status != 0 ))'))
        self.assertIn("tr -d '\\r'", runner)
        self.assertIn('grep -Fqx "$expected"', runner)
        self.assertIn("--show-cursor", runner)
        self.assertIn("sleep 2", runner)
        self.assertIn("attribute_fence_log.py", runner)
        self.assertIn("/proc/$server_pid/exe", runner)
        self.assertIn("--runtime-capable", runner)
        self.assertNotIn("grep -Fq 'Waiting on client fence", runner)
        self.assertIn("cleanup: complete", runner)
        self.assertIn('runner_stderr_log="$probe_dir/runner.stderr.log"', runner)
        self.assertIn('>"$runner_stdout_log" 2>"$runner_stderr_log"', runner)
        self.assertIn("unity requires WINEOPENXR_STAGED_CROSS_GPU_COLOR=1", runner)
        self.assertIn("Experimental D3D11 staged cross-GPU color is enabled", runner)
        self.assertIn("copy_count=$(grep -Fc 'Staged-color copy:'", runner)
        self.assertIn("required_copy_count=2", runner)
        self.assertIn("staged-color proxy-to-transport copies", runner)
        self.assertIn("Staged-color copy submission failed", runner)
        self.assertIn("Not staging unsupported color swapchain", runner)

        self.assertIn("verify_dxvk_interop_device(device)", probe)
        self.assertIn("0x93, 0x25", probe)
        self.assertIn("verified DXVK IDXGIVkInteropDevice2 support", probe)
        self.assertIn("DXVK interop preflight complete", probe)
        self.assertIn("PFN_xrEndSession endSession", probe)
        self.assertIn('load_xr(xr, instance, "xrEndSession", xr.endSession)', probe)
        running_events = probe.split("static bool poll_running_session_events", 1)[1].split(
            "static bool end_aborted_frame", 1
        )[0]
        self.assertIn("XR_SESSION_STATE_STOPPING", running_events)
        self.assertIn("xr.endSession(session)", running_events)
        self.assertIn("XR_SESSION_STATE_EXITING", running_events)
        self.assertIn("XR_SESSION_STATE_LOSS_PENDING", running_events)
        self.assertIn("session reached SYNCHRONIZED", running_events)
        self.assertIn("session reached render-visible state", running_events)
        projection_frame = probe.split("static bool run_projection_frame", 1)[1].split(
            "static bool\nclear_depth_swapchain", 1
        )[0]
        self.assertIn("std::chrono::seconds(10)", projection_frame)
        self.assertIn("std::chrono::seconds(3)", projection_frame)
        self.assertIn("readinessDeadline", projection_frame)
        self.assertIn("renderDeadline", projection_frame)
        self.assertIn("XR_SESSION_STATE_VISIBLE", projection_frame)
        self.assertIn("XR_SESSION_STATE_FOCUSED", projection_frame)
        self.assertIn("poll_running_session_events", projection_frame)
        self.assertIn("std::this_thread::sleep_for(std::chrono::milliseconds(10))", projection_frame)
        self.assertIn("xr.endFrame(session, &endInfo)", projection_frame)
        self.assertLess(
            projection_frame.index('xr.endFrame(session, &endInfo), "xrEndFrame(no layers)"'),
            projection_frame.index("poll_running_session_events", projection_frame.index("if (!frameState.shouldRender")),
        )
        self.assertIn("maxRenderedFrames", projection_frame)
        self.assertNotIn("maxFrameAttempts", projection_frame)

        readme = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/README.md").read_text()
        attended_commands = readme.split("OPENXR_LOADER_DLL=", 1)[1].split("```", 1)[0]
        self.assertLess(attended_commands.index("run-guarded.sh draw-depth"),
                        attended_commands.index("run-guarded.sh draw-native-pair"))
        self.assertLess(attended_commands.index("run-guarded.sh draw-native-pair"),
                        attended_commands.index("run-guarded.sh draw-color"))

        build = (PROTON_ROOT / "tools/d3d11-openxr-depth-probe/build.sh").read_text()
        self.assertIn("OPENXR_LOADER_DLL", build)
        self.assertNotIn("BONELAB", build)
        self.assertNotIn("/tmp/codex-proton", build)
        self.assertIn("-Wpedantic -Werror", build)
        self.assertIn("-Wno-missing-field-initializers", build)
        self.assertIn("-Wno-cast-function-type -static", build)
        self.assertIn("attribute_fence_log.py", build)

        walkabout_runner = (
            PROTON_ROOT
            / "tools/d3d11-openxr-depth-probe/run-walkabout-guarded.sh"
        ).read_text()
        self.assertIn("ALLOW_LIVE_WALKABOUT", walkabout_runner)
        self.assertIn("no established WiVRn headset transport", walkabout_runner)
        self.assertIn("headset presence is not active", walkabout_runner)
        self.assertIn("before_xids=", walkabout_runner)
        self.assertIn("--application Unity", walkabout_runner)
        self.assertIn('--application Unity --pid "$walkabout_pid"', walkabout_runner)
        self.assertIn("--print-client-id", walkabout_runner)
        self.assertIn("WINEOPENXR_STAGED_CROSS_GPU_COLOR=1", walkabout_runner)
        self.assertIn("WINEDLLOVERRIDES='d3d11=n;dxgi=n'", walkabout_runner)
        self.assertIn("walkabout.xr-environ", walkabout_runner)
        self.assertNotIn('>"$artifact_dir/walkabout.environ"', walkabout_runner)
        self.assertIn("Shared Depth Buffer: Yes", walkabout_runner)
        self.assertIn("XRTextureManager::SetupRenderTextureFromXRRequest", walkabout_runner)
        self.assertIn("walkabout.mapped-artifacts.sha256", walkabout_runner)
        self.assertIn("WalkaboutMiniGolf_dxgi.log", walkabout_runner)
        self.assertIn("WalkaboutMiniGolf_d3d11.log", walkabout_runner)
        self.assertIn('expected_heroic_build=', walkabout_runner)
        self.assertIn('expected_heroic_electron=', walkabout_runner)
        self.assertIn('find_heroic_lock_holders', walkabout_runner)
        self.assertIn('ELECTRON_RENDERER_URL="file://$heroic_build/index.html"', walkabout_runner)
        self.assertIn('"$heroic_electron" . --no-gui "$game_url"', walkabout_runner)
        self.assertIn('[[ "$heroic_pgid" == "$heroic_pid" ]]', walkabout_runner)
        self.assertIn('Client $unity_client_id disconnected', walkabout_runner)
        self.assertIn("SECONDS - workload_ready_at >= healthy_seconds", walkabout_runner)
        self.assertIn('WINEPREFIX="$prefix_dir" "$wineserver" -k', walkabout_runner)
        self.assertIn("status=99", walkabout_runner)
        self.assertIn("--after-cursor", walkabout_runner)
        self.assertNotIn("strace", walkabout_runner)

        for usage in (
            "XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL",
            "XRT_SWAPCHAIN_USAGE_SAMPLED",
            "XRT_SWAPCHAIN_USAGE_TRANSFER_SRC",
            "XRT_SWAPCHAIN_USAGE_TRANSFER_DST",
            "XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT",
        ):
            self.assertIn(usage, monado)
        self.assertIn("client_vk_local_depth_required_usage", monado)
        self.assertIn("client_vk_prepare_local_depth_create_info", monado)
        self.assertIn("vk_csci_is_local_image_format_supported", monado)
        self.assertIn("vk_ic_allocate_local", monado)
        self.assertIn("create_image(vk, xscci, false", allocator)
        self.assertIn("if (usage == 0)", support)
        local_helper = support.split("vk_csci_is_local_image_format_supported", 1)[1].split(
            "vk_csci_is_format_supported", 1
        )[0]
        self.assertNotIn("VkPhysicalDeviceExternalImageFormatInfo", local_helper)
        self.assertIn("VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT", support)
        self.assertIn(".flags = get_image_create_flags", support)


if __name__ == "__main__":
    unittest.main()
