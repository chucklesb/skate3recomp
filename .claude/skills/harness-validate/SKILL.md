---
name: harness-validate
description: Run skate3recomp with the Vulkan validation layers on a DroidDeck device (or the Windows build) through the scenario harness and triage the messages by VUID. Use when checking a renderer change for Vulkan API misuse.
argument-hint: "[scenario.toml with vulkan_validation = true] (default scenarios/validation_smoke.toml)"
---

# Vulkan validation run

Reference: `docs/scenario-harness.md`, section "Validation layers and RenderDoc". Arguments: `$ARGUMENTS`.

1. **Run the scenario.** It needs `vulkan_validation = true` at its top level, and `run_scenario.py` passes the init-time flags. The layers are already installed in the DroidDeck guest (`vulkan-validationlayers` 1.4.341). With `run_in_background`:
   ```
   tools/harness/rs.sh device scenarios/validation_smoke.toml
   ```
2. **Group the messages.** The summary line reports the count, and every message is in `out/harness-runs/<run>/validation.txt`. Group by VUID:
   ```
   python3 -c 'import re,sys,collections; c=collections.Counter(m.group(1) for l in open(sys.argv[1]) for m in [re.search(r"ID -?\d+: ([^)]*)\)",l)] if m); [print(n,v) for v,n in c.most_common()]' <run>/validation.txt
   ```
3. **Triage.** The layer stops repeating a message after about 10, so counts are capped. Messages mention `command buffer label: native: <pass>` and object names, which point at the native renderer pass and resource involved.
4. **Known baseline noise on DroidDeck:** `VUID-VkSwapchainPresentModeInfoKHR-swapchainCount-07760` and `-arraylength` on every present. These come from a DroidDeck/gamescope layer; the SDK presents with `pNext = nullptr`. Ignore them, and compare everything else against that baseline.

## On Windows

- **Vulkan:** the LunarG Vulkan SDK 1.4.363 registers `VK_LAYER_KHRONOS_validation` system-wide. Run:
  ```
  tools/harness/rs.sh windows scenarios/validation_smoke.toml --backend vulkan
  ```
  To confirm the layer loaded, add `-- --log_file=x.log`; the instance lists it under "Enabled layers". The baseline (an NVIDIA RTX 4070, 2026-11) is **0 messages**, so anything that appears is new.
- **D3D12:** `--backend d3d12` turns the same scenario request into `--d3d12_debug=true`. The SDK logs the debug layer's warnings and errors as `D3D12 <severity> (debug layer, ID <n>): …` into `validation.txt` (`validation_layers: "d3d12"` in result.json). Group them by `ID <n>` instead of VUID:
  ```
  sed 's/^\[[^]]*\] //' <run>/validation.txt | cut -c1-200 | sort | uniq -c | sort -rn
  ```
  - **Baseline** (release, `validation_smoke`): 19× ID 1422 (`ExecuteCommandLists`: a resource on a `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED` heap…) and 1× ID 1328 (`CreateCommittedResource` ignores `InitialState UNORDERED_ACCESS` for buffers). Both come from the SDK's D3D12 backend.
  - **Ignore** the "Live Object … Refcount" lines at exit: that's the leak report when the harness ends the process.
