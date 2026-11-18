---
name: harness-renderdoc
description: Capture a RenderDoc frame of skate3recomp's native renderer through the scenario harness, on a DroidDeck device or on Windows (Vulkan or D3D12), and inspect the capture's passes, draws and resources. Use for render bugs that need a frame capture.
argument-hint: "[scenario.toml with a capture step] (default scenarios/renderdoc_capture.toml)"
---

# RenderDoc capture

Reference: `docs/scenario-harness.md`, section "Validation layers and RenderDoc". Arguments: `$ARGUMENTS`.

Run commands from the repo root in the project's shell (`docs/dev-setup.md`); use `run_in_background` for builds and runs.

## On the device

1. **Check RenderDoc is in the DroidDeck guest:** `/root/renderdoc/renderdoccmd` (a guest path; no device root needed) should exist. If it's missing, build and install it:
   ```
   tools/harness/build-renderdoc-aarch64.sh
   tools/harness/rs.sh install-renderdoc
   ```
2. **Run a scenario with a `capture` step** (`tag`, `frames = 1`), placed at the moment to capture:
   ```
   tools/harness/rs.sh device <scenario> --env SKATE3_RENDERDOC=/root/renderdoc
   ```
   Add `--no-deploy` if the device already has the current build. `SKATE3_RENDERDOC` makes `AppRun` load RenderDoc into the game process only. Don't set `LD_PRELOAD` at session level: RenderDoc would load into a launcher and clear its variables for the game.
3. **Get the capture:** the `.rdc` is in `out/harness-runs/<run>/captures/<tag>_capture.rdc`, about 200 MB per gameplay frame. The user opens it in desktop RenderDoc **v1.46**.
4. **Inspect it without a GUI** (in `nix develop .#cross`):
   ```
   qemu-aarch64 -L $SKATE3_AARCH64_SYSROOT -E LD_LIBRARY_PATH=$PWD/out/renderdoc-aarch64 \
     out/renderdoc-aarch64/renderdoccmd convert -f <rdc> -o out/cap.xml -c xml
   ```
   Then grep `out/cap.xml`:
   - pass labels: `native: main scene`, `native: shadow atlas`, …
   - resource names: `native: scene depth`, `native pso vs_main / ps_main`
   - API chunk counts: `name="vkCmdDrawIndexed"`

**If no capture appears:** the harness reports either "RenderDoc is not attached" or "did not write the capture". Check `/tmp/RenderDoc/*.log` in the guest (`$DROIDDECK_DIR/tools/droiddeckctl start desktop`, then `guest`) for "Initialised capture layer" and "Created capture device".

## On Windows (WSL hosts)

- **Setup:** a portable RenderDoc v1.46 (the same version as the device build), its folder in `SKATE3_WINDOWS_RENDERDOC` (`.claude/local.env`). Nothing is installed system-wide.
- **Run:** use `run_in_background`:
  ```
  tools/harness/rs.sh windows scenarios/renderdoc_capture.toml --backend vulkan --renderdoc
  ```
  It launches through `renderdoccmd capture` and sets `VK_ADD_IMPLICIT_LAYER_PATH` for the unregistered Vulkan layer. `--backend vulkan` and `--backend d3d12` both work. Add `--no-build` to skip the rebuild.
- **Getting the capture:** the `.rdc` is in `out/harness-runs/<run>/captures/`, about 320 MB for a Vulkan gameplay frame. The user opens it directly in that RenderDoc's `qrenderdoc.exe`, so no XML conversion is needed.
- **Prefer Vulkan for captures** (same API as the device, and it has the labels). A D3D12 capture contains the native renderer's passes and draws, but no `native: …` labels or resource names, because the D3D12 backend (`native_rhi_d3d12.cpp`) sets no markers or object names.

### If a Windows capture run goes wrong
- **Device lost at capture start** (`VulkanPresenter: Failed to submit command buffers (ErrorDeviceLost)` ~130 ms after `step N capture`, or an access violation in `nvoglv64.dll` under `renderdoc.dll`): this was issue #1, fixed 2026-11-07. RenderDoc's explicit Start/EndFrameCapture submits on the app's queues from the calling thread, so the SDK now holds every Vulkan queue mutex across it (`rex::ui::renderdoc::SetQueueGuard`, installed by `VulkanDevice`). If it comes back, check that this guard still covers every queue the app submits to.
- **It's intermittent.** Before the fix it failed about half the time. Never conclude from one run; repeat each variant 4–6 times. (One-off passes once pointed at sparse buffers and then at the screenshot, and both were red herrings.)
- **A stuck `skate3.exe`** (1 thread, `taskkill /F` says "no running instance") stays in the GPU driver until a **reboot**, and it locks the staged exe (`cp: cannot remove ... skate3.exe: Input/output error`). It came from `std::_Exit` right after a capture. With RenderDoc attached, the harness now closes the game normally (`harness: RenderDoc attached, closing the game normally`) and falls back to the hard exit after 30 s. Ask the user to reboot; don't keep retrying the kill.
- **There's no RenderDoc log on Windows:** it removes its `%TEMP%\RenderDoc` log at exit, and `RENDERDOC_DEBUG_LOG_FILE` (through WSLENV) didn't redirect it. Use the game's `log.txt` and `crash.txt`, plus `run_scenario.py symbolize --target windows` on the frames.
- `--gpu_debug_markers=false` can't turn markers off under RenderDoc (`flags.cpp` forces them on when it's attached).
