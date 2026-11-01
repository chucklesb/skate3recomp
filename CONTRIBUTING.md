# Contributing

Thanks for helping improve the Skate 3 recompilation. This fork continues
[mchughalex/skate3recomp](https://github.com/mchughalex/skate3recomp); bug
reports and pull requests go to
[rubensteezy/skate3recomp](https://github.com/rubensteezy/skate3recomp).

**Never upload or commit Skate 3 game files**: no ISOs, no `default.xex` /
`.xexp`, no extracted assets, and no title update package. Logs and
screenshots are fine.

## Reporting Bugs

Known bugs are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md); check there
first. A pull request that fixes a tracked bug names its id (for example
`Fixes KI-16`) and updates its entry in the same change.

Open an issue with:

- **What happened and how to reproduce it:** the mode or map, what you were
  doing, and whether it happens every time.
- **Your setup:** OS, GPU and driver version, and whether you use a release
  build or your own build (with the commit).
- **Renderer settings:** Native or Emulated, DirectX 12 or Vulkan, and any
  non-default video settings (MSAA, render scale, shadows, ultrawide).
- **Whether the emulated renderer shows it too:** press F5 to toggle between
  the native and emulated renderer. For rendering bugs, a screenshot of each
  is the most useful thing you can attach.
- **Logs:** the `logs` folder next to the executable. Each run writes
  `skate3_NNN.log`, and long runs rotate into `skate3_NNN.1.log`,
  `skate3_NNN.2.log` and so on, oldest with the highest number. Zip the whole
  set for the run that went wrong, not just the newest file: a flood of one
  error can push the useful start of the run into the rotated files.

For crashes, also say what was on screen and roughly how long you had been
playing.

## Development Setup

Follow [Building from Source](README.md#building-from-source) in the README.
A few things that save time:

- Clone recursively, and if you update submodules later, don't use
  `--depth 1`: shallow updates can leave SDK dependencies on the wrong commits.
- Codegen only needs to re-run (`generate-all`, then reconfigure) when the
  game executable, the files in `config/`, the codegen patch scripts in
  `cmake/`, or the SDK's code generator change. Everyday edits to `src/` only
  need the normal build.
- To run a development build against an installed game, start it with the
  install folder as the working directory (the game folder is resolved from
  `./game`), or pass `--game_data_root`. Saves and settings are shared with
  release builds; logs go next to the development executable.

### Repository Layout

- `src/`: the game application: startup, settings, the ISO and title update
  installers, and the native renderer. The renderer is split into capture
  (`skate3_native_scene.cpp`, reading the game's draw stream and material
  constants) and GPU translation (`skate3_native_scene_gpu.cpp`,
  `skate3_native_scene_post.cpp`).
- `src/native/shaders/`: the native renderer's HLSL, embedded into the build
  automatically, and `spirv/`, the precompiled Vulkan SPIR-V.
- `config/`: function configuration for the recompiler.
- `cmake/`: build helpers, including the codegen patch scripts.
- `tests/` and `tools/`: standalone tests and the SPIR-V generator scripts.
- `third_party/rexglue-sdk/`: the SDK submodule: runtime, codegen, input,
  audio, and the Direct3D 12 and Vulkan backends.
- `generated/`: recompiled game code produced by codegen. It is gitignored;
  never commit it.

## Making Changes

### Shaders

Direct3D 12 compiles the embedded HLSL at runtime, but Vulkan uses the
precompiled SPIR-V in `src/native/shaders/spirv/skate3_native_shaders_spirv.h`.
**If you edit HLSL, regenerate the matching SPIR-V and commit it in the same
change**, otherwise the Vulkan backend keeps running the old shader. The header
comment records the DXC version and flags used. `tools/generate_hom_spirv.py`
and `tools/generate_screen_fx_spirv.py` regenerate the Hall of Meat and
screen-effects blobs, and both have a `--check` mode.

### SDK Changes

Changes to the runtime or the D3D12/Vulkan backends go in the SDK fork,
[rubensteezy/rexglue-skate3](https://github.com/rubensteezy/rexglue-skate3)
(branch `skate3-sdk-clean`). Land the SDK commit there first, then bump the
submodule in this repository with a separate commit (`sdk: bump for ...`).
A pull request here must never pin an SDK commit that isn't on the SDK fork's
`skate3-sdk-clean` branch.

The SDK fork's CI checks formatting on every push and pull request with the
clang-format version pinned in `scripts/requirements-dev.txt` (20.1.8). Format
your SDK changes with that exact version before pushing; other versions format
some constructs differently.

The fork follows upstream rexglue releases by merging them into
`skate3-sdk-clean` (add `https://github.com/rexglue/rexglue-sdk` as a remote and
merge its release tag), so the SDK keeps a shared history with upstream.

### Commit Messages

Start the subject with the area it touches, in lower case, followed by a short
summary of the change:

```text
render: serve the shadow color floor whenever the world rows are valid
capture: place sort-list batched piece clones with their instance matrix
sdk: bump for the d3d12 descriptor range reuse fix
```

Common areas: `render`, `capture`, `native-scene`, `sdk`, `build`,
`installer`, `app`, `docs`. Use the body to explain **why**: the symptom, the
cause, and anything a later reader needs to know, such as what you measured or
what you tried and rejected.

## Testing

- Build the `relwithdebinfo` preset and play through the area your change
  affects.
- For rendering changes, compare against the emulated renderer with F5. Test
  both DirectX 12 and Vulkan when you touch shaders or the graphics backends,
  if you can.
- Run the Python tests:

  ```sh
  python -m unittest discover -s tests -p "test_*.py"
  ```

- Run the C++ unit tests in `tests/`. They need no SDK checkout, game data or
  codegen, so they configure on their own:

  ```sh
  cmake -S . -B out/build/tests -DSKATE3_TESTS_ONLY=ON
  cmake --build out/build/tests --config Release --target skate3_tests
  ctest --test-dir out/build/tests -C Release --output-on-failure
  ```

  `-DSKATE3_BUILD_TESTS=ON` builds them next to the game in a normal
  configure instead. Use `CHECK` from `tests/test_check.h`, not `assert`,
  which release builds compile out. A new test must only include the
  standard library and headers like `src/native/screen_fx.h`; move the logic
  it checks out of the game code into such a header first.
- The `tests` GitHub Actions workflow runs both sets on every pull request and
  push to `main`: Python on Ubuntu, C++ with MSVC and clang on Windows and with
  clang and GCC on Ubuntu.

## Pull Requests

- Target `main` on
  [rubensteezy/skate3recomp](https://github.com/rubensteezy/skate3recomp).
- Keep each pull request to one topic. Unrelated fixes are easier to review and
  merge as separate pull requests.
- In the description, say what changed and why, what you tested (OS, backend,
  where in the game), and what you could not test.
- Don't include `generated/`, build output, or local config such as
  `skate3.toml`.
