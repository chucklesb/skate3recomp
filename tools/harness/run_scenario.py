#!/usr/bin/env python3
"""Run scenario-harness scenarios locally or on a DroidDeck handheld.

    run_scenario.py local  <scenario.toml> --exe <skate3 or AppImage> [-- args...]
    run_scenario.py odin2  <scenario.toml> [--config release|relwithdebinfo] [--no-build] [--no-deploy] [-- args...]
    run_scenario.py live   [--config ...] [--no-deploy] [-- args...]  # live mode (inbox + recording)
    run_scenario.py windows <scenario.toml> [--config ...] [--backend d3d12|vulkan] [--no-build]
                           [--renderdoc [DIR]] [--timeout s] [-- args...]
    run_scenario.py ab     <scenario.toml> [--config ...] [--backends d3d12,vulkan] [--fps-cap 30]
                           [--no-build] [--no-deploy] [--no-odin2]   # Odin 2 vs Windows, compared
    run_scenario.py live --target windows [--config ...] [--backend ...]; send --target windows ...
    run_scenario.py symbolize [--target odin2|windows] [--config ...] <module+0xoff | 0xaddr>...
    run_scenario.py send   <verb> [key=value ...]          # one live command, prints the reply
    run_scenario.py fetch                                  # copy the device's latest run folder
    run_scenario.py pull   <guest path> [--to dir]         # copy any guest file/folder off the device
    run_scenario.py reels  [--pull [DIR]] [--install-windows]  # list / copy the device's reels
    run_scenario.py install-renderdoc                      # push an arm64 RenderDoc to the device
    run_scenario.py summary <run-folder>

Run it with `python3 -I` from WSL or Linux. The `odin2` commands drive
DroidDeck through its droiddeckctl (DROIDDECK_DIR, default ~/dev/DroidDeck)
over adb. On a Windows+WSL host whose WSL networking hijacks adb's port,
point ADB at the Windows adb.exe and set ANDROID_ADB_SERVER_PORT; both are
passed through. Fetched run folders land in --out (default
out/harness-runs/).

The DroidDeck flow mirrors an AppImage import: the testing-variant AppImage
is pushed to shared storage, extracted inside a desktop session to
/root/skate3-harness/app, then started with DroidDeck's own
droiddeck-appimage-run. When the game exits, DroidDeck's "The session
ended" dialog is dismissed with Back. Run folders live in the guest under
/root/.local/share/skate3/harness/ (AppRun's working directory).
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path, PureWindowsPath

REPO = Path(__file__).resolve().parents[2]
GUEST_APP_DIR = "/root/skate3-harness"
GUEST_HARNESS = "/root/.local/share/skate3/harness"
GUEST_RENDERDOC = "/root/renderdoc"
SHARED = "/storage/emulated/0/Download/skate3-harness"
DEFAULT_ADB_PORT_HINT = "ANDROID_ADB_SERVER_PORT"
# DroidDeck's guest root filesystem as Android sees it (guest /root is
# <GUEST_ROOTFS>/root). adb can read it only as root: the app is not
# debuggable, so `run-as` fails. On a rooted device (the Odin 2 has Magisk
# with shell root granted) guest files are read without a DroidDeck session,
# in well under a second instead of ~20 s for a desktop session. Writes still
# go through the guest (`guest`), so files keep the app's owner and SELinux
# label. SKATE3_ADB_ROOT=0 forces the session path.
DROIDDECK_PACKAGE = os.environ.get("DROIDDECK_PACKAGE", "com.droiddeck.launcher")
GUEST_ROOTFS = f"/data/data/{DROIDDECK_PACKAGE}/files/linuxfs"
GUEST_DATA = "/root/.local/share/skate3"


def log(msg: str) -> None:
    print(f"[run_scenario] {msg}", flush=True)


def die(msg: str) -> "None":
    print(f"run_scenario: {msg}", file=sys.stderr)
    sys.exit(2)


# ------------------------------------------------------------------ device


class Device:
    """droiddeckctl + adb, with Windows adb.exe path handling."""

    def __init__(self) -> None:
        self.dd_dir = Path(os.environ.get("DROIDDECK_DIR", str(Path.home() / "dev/DroidDeck")))
        self.ctl = self.dd_dir / "tools/droiddeckctl"
        if not self.ctl.exists():
            die(f"droiddeckctl not found at {self.ctl} (set DROIDDECK_DIR)")
        self.adb = os.environ.get("ADB", "adb")
        self.env = dict(os.environ)
        # droiddeckctl reads ADB too; WSLENV carries the port to adb.exe.
        if self.adb.endswith(".exe") and DEFAULT_ADB_PORT_HINT in self.env:
            wslenv = self.env.get("WSLENV", "")
            if DEFAULT_ADB_PORT_HINT not in wslenv.split(":"):
                self.env["WSLENV"] = f"{DEFAULT_ADB_PORT_HINT}:{wslenv}" if wslenv else DEFAULT_ADB_PORT_HINT

    def host_path(self, path: Path) -> str:
        """A path adb can read or write (Windows form for adb.exe)."""
        if self.adb.endswith(".exe"):
            out = subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True)
            return out.stdout.strip()
        return str(path)

    def ctl_json(self, *args: str, timeout: float = 120) -> dict:
        proc = subprocess.run([str(self.ctl), *args], capture_output=True, text=True, env=self.env,
                              cwd=self.dd_dir, timeout=timeout)
        try:
            return json.loads(proc.stdout)
        except json.JSONDecodeError:
            return {"ok": False, "error": (proc.stdout + proc.stderr).strip()[-500:]}

    def guest(self, script: str, timeout: int = 60, stdin: str | None = None) -> str:
        args = ["guest", "--timeout", str(timeout)]
        if stdin is not None:
            args += ["--stdin", stdin]
        d = self.ctl_json(*args, "--", "sh", "-c", script, timeout=timeout + 30)
        if not d.get("ok", False) or d.get("timedOut"):
            raise RuntimeError(f"guest command failed: {d.get('error') or d}")
        return (d.get("stdout") or {}).get("text", "")

    def phase(self) -> tuple[str, bool]:
        s = self.ctl_json("state").get("session", {})
        return s.get("phase", "?"), bool(s.get("running"))

    def stop(self) -> None:
        self.ctl_json("stop", timeout=180)

    def start_desktop(self) -> None:
        d = self.ctl_json("start", "desktop", "--wait", timeout=240)
        if d.get("ok", False):
            return
        # droiddeckctl can report ACTIVITY_START_FAILED ("no new session
        # appeared; phase is READY") although the session did start.
        phase, running = self.phase()
        if phase == "READY" and running:
            return
        raise RuntimeError(f"could not start a desktop session: {d.get('error')}")

    def set_session_env(self, pairs: list[str]) -> None:
        """KEY=VALUE pairs for the next session only (droiddeckctl env set)."""
        for kv in pairs:
            if "=" not in kv:
                die(f"--env expects KEY=VALUE, got '{kv}'")
            d = self.ctl_json("env", "set", kv)
            if not d.get("ok", False):
                die(f"env set {kv} failed: {d.get('error')}")

    def run(self, program: str, *args: str) -> None:
        # `run` sometimes reports ACTIVITY_START_FAILED although the session
        # started; the caller verifies through the guest.
        self.ctl_json("run", program, *args, timeout=180)

    def dismiss_session_ended(self) -> bool:
        dump = self.ctl_json("ui", "dump")
        if "session ended" not in json.dumps(dump).lower():
            return False
        self.ctl_json("ui", "click", "--match", "Back")
        return True

    def adb_push(self, src: Path, dst: str) -> None:
        subprocess.run([self.adb, "push", self.host_path(src), dst], env=self.env, check=True,
                       stdout=subprocess.DEVNULL)

    def adb_pull(self, src: str, dst: Path) -> None:
        dst.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([self.adb, "pull", src, self.host_path(dst)], env=self.env, check=True,
                       stdout=subprocess.DEVNULL)

    def adb_shell(self, cmd: str, timeout: float = 60) -> str:
        proc = subprocess.run([self.adb, "shell", cmd], capture_output=True, text=True,
                              env=self.env, timeout=timeout)
        if proc.returncode != 0:
            raise RuntimeError(f"adb shell failed ({proc.returncode}): "
                               f"{(proc.stdout + proc.stderr).strip()[-300:]}")
        return proc.stdout

    _root: bool | None = None

    def has_root(self) -> bool:
        """Whether `adb shell su` works (see GUEST_ROOTFS). Checked once."""
        if self._root is None:
            self._root = False
            if os.environ.get("SKATE3_ADB_ROOT", "1") != "0":
                try:
                    self._root = "uid=0(" in self.adb_shell("su -c id", timeout=20)
                except (RuntimeError, subprocess.TimeoutExpired):
                    pass
        return self._root

    @contextlib.contextmanager
    def guest_files(self):
        """Read-only access to guest files. Yields run(script, timeout=120) ->
        stdout, where the script writes guest paths as {R}/root/... . Through
        adb root when available (an Android toybox shell: cp, ls, cat, find,
        od, stat; no single quotes in the script), else inside the guest,
        starting and stopping a desktop session if none is running."""
        if self.has_root():
            def run_root(script: str, timeout: float = 120) -> str:
                s = script.replace("{R}", GUEST_ROOTFS)
                if "'" in s:
                    raise ValueError("guest_files scripts cannot contain single quotes")
                return self.adb_shell(f"su -c '{s}'", timeout=timeout)
            yield run_root
            return
        started = False
        if not self.phase()[1]:
            self.start_desktop()
            started = True
        try:
            yield lambda script, timeout=120: self.guest(script.replace("{R}", ""),
                                                         timeout=int(timeout))
        finally:
            if started:
                self.stop()

    def pull_guest(self, guest_path: str, dst: Path) -> Path:
        """Copy a guest file or folder to dst (replaced) via shared storage."""
        stage = f"{SHARED}/pull/{Path(guest_path).name}"
        with self.guest_files() as run:
            run(f"rm -rf {SHARED}/pull && mkdir -p {SHARED}/pull && cp -r {{R}}{guest_path} {stage}")
        if dst.exists():
            shutil.rmtree(dst) if dst.is_dir() else dst.unlink()
        tmp = Path(tempfile.mkdtemp(prefix="harness-pull-", dir=dst.parent if dst.parent.exists() else None))
        try:
            self.adb_pull(stage, tmp / "x")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(tmp / "x"), str(dst))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            self.adb_shell(f"rm -rf {SHARED}/pull")
        return dst


CONFIGS = ("release", "relwithdebinfo")
DEPLOYED_CONFIG = REPO / "out/harness/deployed-config"


def nix_cmd(shell: str, argv: list[str], **kw) -> subprocess.CompletedProcess:
    """Run argv inside `nix develop .#<shell>` from the repository root."""
    script = (". ~/.nix-profile/etc/profile.d/nix.sh 2>/dev/null; "
              f"exec nix develop .#{shell} --command " + " ".join(shlex.quote(x) for x in argv))
    return subprocess.run(["bash", "-c", script], cwd=REPO, **kw)


def build_testing_appimage(config: str) -> Path:
    build_dir = f"out/build/linux-aarch64-{config}"
    if not (REPO / build_dir / "skate3").exists():
        die(f"{build_dir}/skate3 is missing; run tools/cross-aarch64/build.sh {config} build")
    out = REPO / "out/harness"
    for old in out.glob("*.AppImage*"):
        old.unlink()
    nix_cmd("cross", ["env", "SKATE3_APPIMAGE_SKIP_VALIDATE=1", "SKATE3_APPIMAGE_VARIANT=testing",
                      "tools/appimage/build-appimage.sh", build_dir, "out/harness"],
            check=True, stdout=subprocess.DEVNULL)
    images = sorted(out.glob("skate3recomp-testing-*-aarch64.AppImage"))
    if not images:
        die("build-appimage.sh produced no testing AppImage")
    return images[-1]


def deployed_config() -> str:
    try:
        return DEPLOYED_CONFIG.read_text().strip() or "unknown"
    except OSError:
        return "unknown"


def deploy(dev: Device, config: str) -> None:
    image = build_testing_appimage(config)
    log(f"deploying {image.name} ({config})")
    if dev.phase()[1]:
        dev.stop()
    dev.adb_push(image, f"{SHARED}/skate3-harness.AppImage")
    dev.start_desktop()
    out = dev.guest(
        f"mkdir -p {GUEST_APP_DIR} && cd {GUEST_APP_DIR} && rm -rf app squashfs-root && "
        f"cp {SHARED}/skate3-harness.AppImage image.AppImage && chmod +x image.AppImage && "
        "./image.AppImage --appimage-extract >/dev/null 2>&1 && mv squashfs-root app && "
        "echo skate3-harness > name && echo arm64 > arch && echo extracted",
        timeout=300)
    if "extracted" not in out:
        die(f"extraction failed: {out}")
    dev.stop()
    DEPLOYED_CONFIG.write_text(config + "\n")


def launch(dev: Device, args: list[str], env: list[str] | None = None) -> None:
    if dev.phase()[1]:
        dev.stop()
    # Session environment applies to the next session, so it is set only now,
    # after any deploy's desktop session has come and gone.
    dev.set_session_env(env or [])
    dev.run("/usr/local/bin/droiddeck-appimage-run", GUEST_APP_DIR, *args)
    for _ in range(30):
        time.sleep(2)
        try:
            if "skate3" in dev.guest("pgrep -x skate3 >/dev/null && echo skate3 || true"):
                return
        except RuntimeError:
            pass
    die("the game did not start (check the device screen)")


def wait_for_exit(dev: Device, timeout_s: float) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        phase, running = dev.phase()
        if not running:
            log(f"game exited (session {phase})")
            time.sleep(2)
            if dev.dismiss_session_ended():
                log("dismissed DroidDeck's 'The session ended' dialog (Back)")
            return
        time.sleep(5)
    die(f"the game still runs after {timeout_s:.0f} s")


def fetch(dev: Device, out_root: Path) -> Path:
    """Copy the guest's latest run folder to out_root; works after exit
    (through adb root without a session when possible, see guest_files)."""
    with dev.guest_files() as run:
        run_dir = run(f"cat {{R}}{GUEST_HARNESS}/current_run.txt").strip()
        if not run_dir or "'" in run_dir or " " in run_dir:
            die(f"no usable harness run on the device ({run_dir!r})")
        run(f"rm -rf {SHARED}/run && mkdir -p {SHARED} && cp -r {{R}}{run_dir} {SHARED}/run",
            timeout=120)
    name = Path(run_dir).name
    local = out_root / name
    if local.exists():
        shutil.rmtree(local)
    tmp = Path(tempfile.mkdtemp(prefix="harness-pull-", dir=out_root.parent if out_root.parent.exists() else None))
    try:
        dev.adb_pull(f"{SHARED}/run", tmp / "run")
        out_root.mkdir(parents=True, exist_ok=True)
        shutil.move(str(tmp / "run"), str(local))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return local


# ------------------------------------------------------------------ scenario files


def scenario_settings(path: Path) -> dict:
    try:
        import tomllib  # Python 3.11+
        with path.open("rb") as f:
            return tomllib.load(f)
    except Exception:
        return {}


def scenario_timeout(path: Path) -> float:
    return float(scenario_settings(path).get("timeout_s", 600))


def scenario_flags(path: Path, backend: str = "vulkan") -> list[str]:
    """Init-time flags a scenario needs before the harness starts."""
    flags = []
    if scenario_settings(path).get("vulkan_validation"):
        # The graphics device exists before the harness loads the scenario.
        # On D3D12 the same request turns on the D3D12 debug layer, whose
        # messages the SDK logs and the harness counts the same way.
        if backend == "d3d12":
            flags += ["--d3d12_debug=true"]
        else:
            flags += ["--vulkan_validation_enabled=true", "--vulkan_log_debug_messages=true"]
    return flags


def summary(run: Path) -> int:
    result_path = run / "result.json"
    if not result_path.exists():
        crash = parse_crash(run)
        if crash is None:
            print(f"{run}: no result.json and no crash.txt (still running, or killed)")
            return 4
        build = run_build(run)
        print(f"{run.name}: crash (code 5) - {crash['what']}")
        if build:
            print(f"  build {build[0]} {build[1]}")
        if crash["address"]:
            print(f"  address {crash['address']}")
        for line in crash_frames(run, crash):
            print(f"  at {line}")
        if crash["dump"]:
            print(f"  minidump: {run / 'crash.dmp'} (open in WinDbg or Visual Studio with "
                  f"the staged PDBs)")
        analysis = run / "crash_analysis.txt"
        if analysis.exists():
            frames = [ln.strip() for ln in analysis.read_text(errors="replace").splitlines()
                      if "skate3!" in ln or "rexruntime!" in ln][:8]
            print(f"  cdb ({analysis.name}):")
            for ln in frames:
                print(f"    {ln[:160]}")
        return 5
    r = json.loads(result_path.read_text())
    print(f"{run.name}: {r['status']} (code {r['code']}) - {r['reason']}")
    if "build_config" in r:
        print(f"  build v{r['version']} {r['platform']} {r['build_config']}")
    print(f"  {r['frames']} frames, {r['seconds']:.1f} s, {r['screenshots']} screenshots, "
          f"{r['warnings']} warnings, {r['errors']} errors")
    if r.get("validation_enabled"):
        layer = {"d3d12": "D3D12 debug layer"}.get(r.get("validation_layers", ""),
                                                   "Vulkan validation")
        print(f"  {layer}: {r.get('validation_messages', 0)} messages (validation.txt)")
    for c in r.get("captures", []):
        print(f"  RenderDoc capture: {c}")
    for f in r.get("failures", []):
        print(f"  FAIL {f}")
    state = run / "state.jsonl"
    if state.exists():
        for line in state.read_text().splitlines():
            if '"tag"' in line:
                s = json.loads(line)
                sk = s.get("skater")
                pos = f" skater {sk['pos']} {sk['speed']:.2f} m/s" if sk else ""
                print(f"  [{s['tag']}] frame {s['frame']} fe {s.get('fe')}{pos}")
    shots = sorted((run / "shots").glob("*.png")) if (run / "shots").exists() else []
    if shots:
        print(f"  shots: {shots[0].name} .. {shots[-1].name}")
    renderer_diffs(run, shots)
    perf_summary(run)
    return int(r["code"])


def perf_summary(run: Path) -> None:
    """CPU stage and GPU pass timings from the log -> perf.json (only when the
    run logged them, e.g. scenarios/perf_reel.toml); see perf_report.py."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import perf_report  # noqa: E402
    perf = perf_report.write(run)
    if perf and perf["segments"]:
        perf_report.print_summary(run, perf)


def renderer_diffs(run: Path, shots: list[Path]) -> None:
    """Native vs emulated: diff each still shot on both renderers
    (play_reel stills_renderers; tags <tag>_native / <tag>_emulated)."""
    pairs = {}
    for png in shots:
        m = SHOT_RE.match(png.name)
        if m and m.group(1).endswith(("_native", "_emulated")):
            base, rend = m.group(1).rsplit("_", 1)
            pairs.setdefault(base, {})[rend] = png
    pairs = {k: v for k, v in pairs.items() if len(v) == 2}
    if not pairs:
        return
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import imgdiff  # noqa: E402
    out = {}
    print("  native vs emulated (ssim 1.0 = identical):")
    for base, v in sorted(pairs.items()):
        m = imgdiff.diff(v["native"], v["emulated"], run / "diffs" / f"{base}_native_vs_emulated.png")
        out[base] = m
        print(f"    {base:<24} ssim {m['ssim']:.4f} changed {m['changed']:5.2f}% "
              f"mae {m['mae']:6.2f}  {m['verdict']}")
    (run / "renderer_diffs.json").write_text(json.dumps(out, indent=2) + "\n")


def command_toml(verb: str, pairs: list[str], cid: str) -> str:
    lines = [f'do = "{verb}"', f'id = "{cid}"']
    for p in pairs:
        if "=" not in p:
            die(f"expected key=value, got '{p}'")
        k, v = p.split("=", 1)
        try:
            float(v)
            lit = v
        except ValueError:
            # Arrays and inline tables (pos=[x,y,z], stills=[6.0]) pass
            # through as TOML literals; everything else is a string.
            literal = v in ("true", "false") or v.strip().startswith(("[", "{"))
            lit = v if literal else json.dumps(v)
        lines.append(f"{k} = {lit}")
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ builds


def build_step(script: str, config: str, steps: list[str], log_name: str,
               env: dict | None = None) -> None:
    """Run tools/cross-*/build.sh <config> <steps> with nix on PATH; log to out/harness/."""
    log_path = REPO / "out/harness" / log_name
    log_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = (". ~/.nix-profile/etc/profile.d/nix.sh 2>/dev/null; exec "
           + " ".join(shlex.quote(x) for x in [script, config, *steps]))
    log(f"{script} {config} {' '.join(steps)} (log: {log_path.relative_to(REPO)})")
    with log_path.open("w") as f:
        code = subprocess.run(["bash", "-c", cmd], cwd=REPO, stdout=f, stderr=subprocess.STDOUT,
                              env={**os.environ, **(env or {})}).returncode
    if code != 0:
        tail = log_path.read_text(errors="replace").splitlines()[-15:]
        die(f"{script} failed ({code}):\n" + "\n".join(tail))


def build_aarch64(config: str) -> None:
    # build.sh reconfigures, so the version string matches HEAD.
    build_step("tools/cross-aarch64/build.sh", config, ["build"], f"aarch64-{config}-build.log")


# ------------------------------------------------------------------ windows


WIN_STAGE_ROOT = Path(os.environ.get("SKATE3_WINDOWS_STAGE_ROOT",
                                     str(REPO / "out/stage/windows-x64")))
WIN_HARNESS_OUT = "harness"  # relative to the stage dir (the game's working directory)
BACKENDS = ("d3d12", "vulkan")
WIN_LIVE_CONFIG = REPO / "out/harness/windows-live-config"


def win_stage(config: str) -> Path:
    return WIN_STAGE_ROOT / config


def win_path(path: Path) -> str:
    out = subprocess.run(["wslpath", "-w", str(path)], capture_output=True, text=True, check=True)
    return out.stdout.strip()


def win_game_running() -> bool:
    out = subprocess.run(["tasklist.exe", "/FI", "IMAGENAME eq skate3.exe", "/NH"],
                         capture_output=True, text=True)
    return "skate3.exe" in out.stdout.lower()


def build_windows(config: str) -> None:
    build_step("tools/cross-windows/build.sh", config, ["build", "stage"],
               f"windows-{config}-build.log",
               env={"SKATE3_WINDOWS_STAGE_ROOT": str(WIN_STAGE_ROOT)})


WIN_RENDERDOC = os.environ.get("SKATE3_WINDOWS_RENDERDOC", "")
# Optional user-data root for Windows runs (saves, replays, settings): a test
# copy, so harness runs never touch the player's %APPDATA%\skate3.
WIN_USER_DATA = os.environ.get("SKATE3_WINDOWS_USER_DATA", "")
# The Windows console debugger, for crash.dmp triage: SKATE3_WINDOWS_CDB, else
# cdbX64.exe / cdb.exe on PATH (the WinDbg app installs a cdbX64.exe alias).
WIN_CDB = (os.environ.get("SKATE3_WINDOWS_CDB")
           or shutil.which("cdbX64.exe") or shutil.which("cdb.exe") or "")


def cdb_triage(run: Path, stage: Path) -> Path | None:
    """crash.dmp -> crash_analysis.txt: exception context, stack, !analyze.
    Symbols come from the staged build (skate3.pdb for relwithdebinfo); no
    network symbol server, so it stays fast."""
    dmp = run / "crash.dmp"
    if not dmp.exists() or not WIN_CDB:
        return None
    out = run / "crash_analysis.txt"
    cmds = ".ecxr; kv 40; .echo ----; !analyze -v; q"
    try:
        res = subprocess.run([WIN_CDB, "-z", win_path(dmp), "-y", win_path(stage),
                              "-i", win_path(stage), "-c", cmds],
                             capture_output=True, text=True, errors="replace", timeout=180)
        out.write_text(res.stdout + res.stderr)
    except (OSError, subprocess.TimeoutExpired) as e:
        out.write_text(f"cdb failed: {e}\n")
    return out


def win_launch(config: str, backend: str, game_args: list[str],
               renderdoc: Path | None = None) -> subprocess.Popen:
    stage = win_stage(config)
    exe = stage / "skate3.exe"
    if not exe.exists():
        die(f"{exe} is missing; run tools/cross-windows/build.sh {config} build stage "
            f"(SKATE3_WINDOWS_STAGE_ROOT={WIN_STAGE_ROOT})")
    game = WIN_STAGE_ROOT / "game"
    if not (game / "default.xex").exists():
        die(f"no game dump in {game} (build.sh stage copies it once)")
    if win_game_running():
        die("skate3.exe is already running on Windows; close it first")
    # A command left in the inbox (e.g. an `exit` sent after the last game had
    # already closed) would run as soon as the new game starts.
    inbox = stage / WIN_HARNESS_OUT / "inbox"
    if inbox.is_dir():
        stale = [p for p in inbox.iterdir() if p.is_file()]
        for p in stale:
            p.unlink(missing_ok=True)
        if stale:
            log(f"cleared {len(stale)} stale inbox command(s)")
    game_argv = [f"--gpu_backend={backend}", f"--game_data_root={win_path(game)}",
                 f"--skate3_harness_out={WIN_HARNESS_OUT}", *game_args]
    # Test saves (profile, replays) kept apart from the player's %APPDATA%.
    if WIN_USER_DATA:
        game_argv.insert(2, f"--user_data_root={win_path(Path(WIN_USER_DATA))}")
    log(f"windows {config}/{backend}: " + " ".join(shlex.quote(x) for x in game_argv))
    args = [str(exe), *game_argv]
    env = dict(os.environ)
    if renderdoc is not None:
        cmd = renderdoc / "renderdoccmd.exe"
        if not cmd.exists():
            die(f"{cmd} is missing (--renderdoc / SKATE3_WINDOWS_RENDERDOC: a RenderDoc folder)")
        # renderdoccmd injects RenderDoc at startup (D3D12 directly, Vulkan
        # through its implicit layer). A portable RenderDoc's layer isn't
        # registered, so point the loader at it, like AppRun does on Linux;
        # WSLENV carries the variable to the Windows processes.
        args = [str(cmd), "capture", "--wait-for-exit", "--working-dir", win_path(stage),
                win_path(exe), *game_argv]
        env["VK_ADD_IMPLICIT_LAYER_PATH"] = win_path(renderdoc)
        env["WSLENV"] = ":".join(filter(None, [env.get("WSLENV", ""),
                                               "VK_ADD_IMPLICIT_LAYER_PATH"]))
        log(f"through RenderDoc: {cmd}")
    # A /mnt/c working directory becomes the Windows process's working directory.
    # Own session: `live` returns at once and the game must outlive this script.
    return subprocess.Popen(args, cwd=stage, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True, env=env)


def run_windows(scenario: Path, config: str, backend: str, extra: list[str],
                out_root: Path, timeout_s: float | None = None,
                renderdoc: Path | None = None) -> Path | None:
    """Run a scenario on this PC; return the run folder moved under out_root."""
    stage = win_stage(config)
    (stage / "scenarios").mkdir(parents=True, exist_ok=True)
    shutil.copy2(scenario, stage / "scenarios" / scenario.name)
    for rec in scenario_recordings(scenario):
        shutil.copy2(rec, stage / "scenarios" / rec.name)
    current = stage / WIN_HARNESS_OUT / "current_run.txt"
    current.unlink(missing_ok=True)
    proc = win_launch(config, backend, [f"--skate3_harness_scenario=scenarios/{scenario.name}",
                                        *scenario_flags(scenario, backend), *extra],
                      renderdoc=renderdoc)
    log(f"running {scenario.name}")
    deadline = time.time() + (timeout_s or scenario_timeout(scenario) + 120)
    try:
        code = proc.wait(timeout=max(1.0, deadline - time.time()))
        log(f"{'renderdoccmd' if renderdoc else 'game'} exited ({code})")
    except subprocess.TimeoutExpired:
        pass
    # A launcher (renderdoccmd) can return while the game still runs, e.g.
    # hung after a device loss: wait for skate3.exe itself, up to the deadline.
    while win_game_running() and time.time() < deadline:
        time.sleep(2)
    if win_game_running():
        log("timeout: killing skate3.exe")
        subprocess.run(["taskkill.exe", "/F", "/IM", "skate3.exe"], capture_output=True)
        time.sleep(2)
    if proc.poll() is None:
        proc.kill()
        proc.wait(timeout=30)
    if not current.exists():
        log("the game wrote no current_run.txt (it failed before the harness started)")
        return None
    name = PureWindowsPath(current.read_text().strip()).name
    src = stage / WIN_HARNESS_OUT / name
    out_root.mkdir(parents=True, exist_ok=True)
    dst = out_root / name
    if dst.exists():
        shutil.rmtree(dst)
    try:
        shutil.move(str(src), str(dst))
    except OSError as e:
        # A file still held open on the Windows side: copy what's there.
        log(f"could not move the run folder ({e}); copying it instead")
        shutil.copytree(src, dst, dirs_exist_ok=True)
    if (dst / "crash.dmp").exists() and cdb_triage(dst, stage):
        log(f"crash triage: {(dst / 'crash_analysis.txt').relative_to(REPO)}")
    return dst


# ------------------------------------------------------------------ odin2 / A-B


def scenario_recordings(scenario: Path) -> list[Path]:
    """Input recordings a scenario's `play` steps name (file = "x.s3rec"),
    resolved next to the scenario: the game resolves them relative to the
    scenario first, so they travel with it."""
    names = re.findall(r'^\s*file\s*=\s*"([^"]+\.s3rec)"', scenario.read_text(), re.M)
    return [p for p in (scenario.parent / n for n in names) if p.is_file()]


def run_odin2(dev: Device, scenario: Path, extra: list[str], env: list[str],
              out_root: Path) -> Path:
    dev.adb_push(scenario.resolve(), f"{SHARED}/scenarios/{scenario.name}")
    for rec in scenario_recordings(scenario):
        dev.adb_push(rec.resolve(), f"{SHARED}/scenarios/{rec.name}")
    launch(dev, [f"--skate3_harness_scenario={SHARED}/scenarios/{scenario.name}",
                 *scenario_flags(scenario), *extra], env=env)
    log(f"running {scenario.name}")
    wait_for_exit(dev, scenario_timeout(scenario) + 120)
    run = fetch(dev, out_root)
    log(f"run folder: {run}")
    return run


SHOT_RE = re.compile(r"^f\d+_(.+)_(\d+)\.png$")
STILL_TAG_RE = re.compile(r"_t\d+\.\d+$")


def run_record(label: str, run: Path | None) -> dict:
    rec: dict = {"target": label, "dir": str(run) if run else None}
    result = run / "result.json" if run else None
    if not result or not result.exists():
        crash = parse_crash(run) if run else None
        if crash is None:
            rec.update(status="no-result", code=4)
        else:
            frames = crash_frames(run, crash, limit=6)
            rec.update(status="crash", code=5, reason=crash["what"], crash_frames=frames,
                       failures=[f"crash: {crash['what']}"] + [f"  at {f}" for f in frames[:3]])
        return rec
    r = json.loads(result.read_text())
    last = r.get("last_state") or {}
    rec.update({k: r.get(k) for k in ("status", "code", "reason", "version", "platform",
                                      "build_config", "frames", "seconds", "warnings",
                                      "errors", "screenshots", "validation_messages",
                                      "failures")})
    rec["fps"] = last.get("fps")
    tags = {}
    state = run / "state.jsonl"
    if state.exists():
        for line in state.read_text().splitlines():
            if '"tag"' in line:
                s = json.loads(line)
                tags[s["tag"]] = {k: s.get(k) for k in ("frame", "t", "fps", "fe", "skater", "cam")}
    rec["tags"] = tags
    return rec


def ab_compare(scenario: Path, runs: list[tuple[str, Path | None]], ab_dir: Path) -> int:
    ab_dir.mkdir(parents=True, exist_ok=True)
    recs = [run_record(label, run) for label, run in runs]
    # Screenshots side by side: shots/<tag>/<target>_<n>.png
    for label, run in runs:
        shots = run / "shots" if run else None
        if not shots or not shots.is_dir():
            continue
        for png in sorted(shots.glob("*.png")):
            m = SHOT_RE.match(png.name)
            if m:
                dst = ab_dir / "shots" / m.group(1)
                dst.mkdir(parents=True, exist_ok=True)
                shutil.copy2(png, dst / f"{label.replace('/', '-')}_{m.group(2)}.png")
    # Image diffs per tag, each target against the first (tools/harness/imgdiff.py).
    # Reel stills (tag reel<N>_t<s>) show the same recorded frame everywhere,
    # so they are the meaningful ones; other shots are informational.
    diffs = {}
    shots_root = ab_dir / "shots"
    if shots_root.is_dir():
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import imgdiff  # noqa: E402
        for tag_dir in sorted(p for p in shots_root.iterdir() if p.is_dir()):
            if len(list(tag_dir.glob("*.png"))) >= 2:
                diffs[tag_dir.name] = imgdiff.diff_dir(tag_dir, ab_dir / "diffs")
    (ab_dir / "compare.json").write_text(
        json.dumps({"scenario": scenario.name, "runs": recs, "diffs": diffs}, indent=2) + "\n")

    print(f"\nA/B {scenario.name}  ->  {ab_dir.relative_to(REPO)}")
    print(f"{'target':<18} {'status':<10} {'build':<16} {'frames':>6} {'sec':>6} {'fps':>6} "
          f"{'warn':>5} {'err':>4}")
    for rec in recs:
        fps = f"{rec['fps']:.0f}" if isinstance(rec.get("fps"), (int, float)) else "-"
        sec = f"{rec['seconds']:.1f}" if isinstance(rec.get("seconds"), (int, float)) else "-"
        print(f"{rec['target']:<18} {rec.get('status', '?'):<10} "
              f"{(rec.get('build_config') or '-'):<16} {rec.get('frames', '-')!s:>6} {sec:>6} "
              f"{fps:>6} {rec.get('warnings', '-')!s:>5} {rec.get('errors', '-')!s:>4}")
        for f in rec.get("failures") or []:
            print(f"{'':<18} FAIL {f}")
    tags = sorted({t for rec in recs for t in rec.get("tags", {})},
                  key=lambda t: min(rec["tags"][t]["frame"] for rec in recs if t in rec.get("tags", {})))
    for tag in tags:
        print(f"  [{tag}]")
        for rec in recs:
            s = rec.get("tags", {}).get(tag)
            if not s:
                print(f"    {rec['target']:<16} -")
                continue
            sk = s.get("skater")
            sk_s = f"pos {sk['pos']} speed {sk['speed']:.2f}" if sk else "no skater"
            print(f"    {rec['target']:<16} frame {s['frame']:>5} fe {s.get('fe')} {sk_s}")
    versions = {rec.get("version") for rec in recs if rec.get("version")}
    if len(versions) > 1:
        print(f"  WARNING: the targets ran different versions: {', '.join(sorted(versions))}")
    if diffs:
        print("  image diffs (vs the first target; ssim 1.0 = identical):")
        for tag, items in diffs.items():
            for m in items:
                a_name, b_name = Path(m["a"]).stem, Path(m["b"]).stem
                still = "  still" if STILL_TAG_RE.search(tag) else ""
                print(f"    {tag:<24} {b_name:<20} vs {a_name:<20} ssim {m['ssim']:.4f} "
                      f"changed {m['changed']:5.2f}% mae {m['mae']:6.2f}  {m['verdict']}{still}")
    if (ab_dir / "shots").is_dir():
        print(f"  paired screenshots: {(ab_dir / 'shots').relative_to(REPO)}/<tag>/")
    ab_perf(runs)
    return 0 if all(rec.get("code") == 0 for rec in recs) else 1


def ab_perf(runs: list[tuple[str, Path | None]]) -> None:
    """Per-segment CPU/GPU timings side by side, for runs that logged them
    (scenarios/perf_reel.toml); details: run_scenario.py perf <a> <b>."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import perf_report  # noqa: E402
    perfs = []
    for label, run in runs:
        if run is None:
            continue
        p = perf_report.write(run)
        if p and p["segments"]:
            perfs.append((label, p))
    if not perfs:
        return
    segs = sorted({s for _, p in perfs for s in p["segments"]}, key=lambda s: (s == "all", s))
    print("  perf (fps | cpu render ms | gpu native ms: main ao shadow sun vol bloom):")
    for seg in segs:
        print(f"    [{seg}]")
        for label, p in perfs:
            d = p["segments"].get(seg)
            if not d:
                continue
            c, g = d["cpu"], d["gpu"]
            gpu = (f"{g.get('nat_total', 0) / 1000:5.2f}: " + " ".join(
                f"{g.get(k, 0) / 1000:.2f}" for k in
                ("NatMain", "NatAO", "NatShadow", "NatSun", "NatVol", "NatBloom"))) if g else "-"
            print(f"      {label:<16} {c.get('guest_fps', 0):4.0f} | {c.get('render_ms', 0):5.2f} | {gpu}")


# ------------------------------------------------------------------ symbolize


def module_files(target: str, config: str) -> dict[str, Path]:
    """Unstripped binaries per module key (no lib prefix, extension or rd postfix)."""
    p = "rd" if config == "relwithdebinfo" else ""
    if target == "odin2":
        b = REPO / f"out/build/linux-aarch64-{config}"
        return {"skate3": b / "skate3", "rexruntime": b / f"librexruntime{p}.so",
                "rexgpu-xenos": b / f"librexgpu-xenos{p}.so"}
    # The SDK links its DLLs (and their PDBs) into its own output folder.
    b = REPO / f"out/build/windows-x64-{config}"
    sdk = REPO / "third_party/rexglue-sdk/out/win-amd64"
    return {"skate3": b / "skate3.exe", "rexruntime": sdk / f"rexruntime{p}.dll",
            "rexgpu-xenos": sdk / f"rexgpu-xenos{p}.dll"}


def module_key(name: str, files: dict[str, Path]) -> str | None:
    key = Path(name.replace("\\", "/")).name.lower()
    for ext in (".so", ".dll", ".exe"):
        key = key.removesuffix(ext)
    key = key.removeprefix("lib")
    if key not in files and key.endswith("rd") and key[:-2] in files:
        key = key[:-2]
    return key if key in files else None


def image_base(path: Path) -> int:
    """Address a module offset is relative to: the lowest PT_LOAD for ELF, ImageBase for PE."""
    with path.open("rb") as f:
        head = f.read(1 << 16)
    if head[:4] == b"\x7fELF":
        phoff, = struct.unpack_from("<Q", head, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", head, 0x36)
        loads = []
        for i in range(phnum):
            off = phoff + i * phentsize
            p_type, = struct.unpack_from("<I", head, off)
            if p_type == 1:  # PT_LOAD
                loads.append(struct.unpack_from("<Q", head, off + 16)[0] & ~0xFFF)
        return min(loads) if loads else 0
    if head[:2] == b"MZ":
        pe, = struct.unpack_from("<I", head, 0x3C)
        opt = pe + 24
        magic, = struct.unpack_from("<H", head, opt)
        if magic != 0x20B:
            die(f"{path}: not a PE32+ image")
        return struct.unpack_from("<Q", head, opt + 24)[0]
    die(f"{path}: neither ELF nor PE")


def resolve(target: str, config: str, addrs: list[str]) -> tuple[dict[str, list[str]], list[str]]:
    """Map each address spec to its frames ("function  file:line", innermost
    first, inlined frames included). Returns (resolved, error messages)."""
    files = module_files(target, config)
    resolved: dict[str, list[str]] = {}
    errors: list[str] = []
    # (spec, value, value_is_offset_from_module_base)
    by_module: dict[str, list[tuple[str, int, bool]]] = {}
    for spec in addrs:
        if "+" in spec:
            mod, off = spec.rsplit("+", 1)
            key = module_key(mod, files)
            if key is None:
                errors.append(f"{spec}: unknown module (one of {', '.join(files)})")
                continue
            by_module.setdefault(key, []).append((spec, int(off, 16), True))
        else:
            # A bare address is already in skate3's own address space.
            by_module.setdefault("skate3", []).append((spec, int(spec, 16), False))
    shell = "cross" if target == "odin2" else "windows"
    for key, items in by_module.items():
        obj = files[key]
        if not obj.exists():
            errors.append(f"{obj.relative_to(REPO)} is missing; build {target} {config} first")
            continue
        base = image_base(obj)
        vas = [base + v if is_off else v for _, v, is_off in items]
        proc = nix_cmd(shell, ["llvm-symbolizer", "--output-style=JSON", f"--obj={obj}",
                               *(hex(v) for v in vas)],
                       capture_output=True, text=True)
        try:
            results = json.loads(proc.stdout)
        except json.JSONDecodeError:
            errors.append(f"llvm-symbolizer failed for {obj.name}: {proc.stderr.strip()[-300:]}")
            continue
        for (spec, _, _), res in zip(items, results):
            out = []
            for fr in res.get("Symbol") or []:
                if not fr.get("FunctionName"):
                    continue
                path = (fr.get("FileName") or "").removeprefix(str(REPO) + "/")
                line = fr.get("Line", 0)
                # Release builds: a symbol name but no line table.
                where = f"{path}:{line}" if path and line else (path or "")
                out.append(f"{fr['FunctionName']}  {where}".rstrip())
            resolved[spec] = out
    return resolved, errors


def symbolize(target: str, config: str, addrs: list[str]) -> int:
    """Print function and file:line for module+offset or bare skate3 addresses."""
    resolved, errors = resolve(target, config, addrs)
    for e in errors:
        print(e)
    for spec in addrs:
        if spec not in resolved:
            continue
        frames = resolved[spec] or ["??"]
        for i, fr in enumerate(frames):
            print(f"{spec if i == 0 else ' ' * len(spec)}  {'(inlined) ' if i else ''}{fr}")
    return 1 if errors else 0


# ------------------------------------------------------------------ crashes


def parse_crash(run: Path) -> dict | None:
    """crash.txt (written by src/harness/harness_crash.cpp), or None."""
    path = run / "crash.txt"
    if not path.exists():
        return None
    crash: dict = {"what": "", "address": None, "frames": [], "maps": [], "dump": None}
    in_maps = False
    for line in path.read_text(errors="replace").splitlines():
        if in_maps:
            crash["maps"].append(line)
        elif line == "maps:":
            in_maps = True
        elif line.startswith("crash: "):
            crash["what"] = line[7:]
        elif line.startswith("address: "):
            crash["address"] = line[9:]
        elif line.startswith("frame: "):
            crash["frames"].append(line[7:].strip())
        elif line.startswith("dump: "):
            crash["dump"] = line[6:]
    return crash


def frame_specs(frames: list[str], maps: list[str]) -> list[str]:
    """Absolute PCs to module+offset through a /proc/self/maps copy; the load
    base is the start of a module's offset-0 mapping. Drops repeats (the
    aarch64 link register often duplicates the first frame-record return)."""
    bases: dict[str, int] = {}
    ranges: list[tuple[int, int, str]] = []
    for line in maps:
        parts = line.split(maxsplit=5)
        if len(parts) < 6 or not parts[5].startswith("/"):
            continue
        start, end = (int(x, 16) for x in parts[0].split("-"))
        name = parts[5].strip()
        ranges.append((start, end, name))
        if int(parts[2], 16) == 0:
            bases.setdefault(name, start)
    out: list[str] = []
    for f in frames:
        spec = f
        if f.startswith("0x"):
            pc = int(f, 16)
            for start, end, name in ranges:
                if start <= pc < end and name in bases:
                    spec = f"{Path(name).name}+{pc - bases[name]:#x}"
                    break
            else:
                if ranges:
                    # The frame-pointer walk ran past the thread's entry
                    # into data that isn't code; nothing after it is real.
                    break
        if not out or out[-1] != spec:
            out.append(spec)
    return out


def run_build(run: Path) -> tuple[str, str] | None:
    """(target, config) from the run's build.json, for symbolizing."""
    try:
        b = json.loads((run / "build.json").read_text())
    except (OSError, json.JSONDecodeError):
        return None
    target = {"linux-arm64": "odin2", "win-amd64": "windows"}.get(b.get("platform", ""))
    config = (b.get("build_config") or "").lower()
    return (target, config) if target and config in CONFIGS else None


def crash_frames(run: Path, crash: dict, limit: int = 12) -> list[str]:
    """`spec  function  file:line` lines for the innermost frames."""
    specs = frame_specs(crash["frames"], crash["maps"])[:limit]
    build = run_build(run)
    resolved: dict[str, list[str]] = {}
    if build:
        target, config = build
        files = module_files(target, config)
        ours = [s for s in specs if "+" in s and module_key(s.rsplit("+", 1)[0], files)]
        if ours:
            resolved, _ = resolve(target, config, ours)
    lines = []
    for s in specs:
        frs = resolved.get(s)
        if not frs:
            lines.append(s)
            continue
        # frs runs innermost-inlined first; the last entry is the function
        # that really owns the frame.
        text = frs[-1]
        if len(frs) > 1:
            text += f"  [inlined: {frs[0].split('  ')[0]} {frs[0].split('  ')[-1]}]"
        lines.append(f"{s}  {text}")
    return lines


# ------------------------------------------------------------------ main


def reels_cmd(dev: Device, pull: Path | None, install_windows: bool) -> int:
    """List the device's saved reels; optionally copy them off and into the
    Windows test profile. Reel id = bytes 20..28 of N.B (what `play_reel
    reel=` matches); byte-identical saves share an id, so `play_reel` needs
    `clip = N` for those."""
    with dev.guest_files() as run:
        found = run(f"find {{R}}{GUEST_DATA} -maxdepth 5 -type d -name REPLY_SKATER").split()
        if not found:
            die(f"no REPLY_SKATER folder under {GUEST_DATA} on the device")
        reel_dir = found[0][len(GUEST_ROOTFS):] if found[0].startswith(GUEST_ROOTFS) else found[0]
        listing = run(f"cd {{R}}{reel_dir} && for f in *.B; do echo $f $(stat -c %Y $f) "
                      f"$(od -An -tx1 -j20 -N8 $f | tr -d \" \\n\"); done")
    rows = []
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0].endswith(".B"):
            rows.append((int(parts[0][:-2]), int(parts[1]), parts[2]))
    rows.sort()
    ids = [r[2] for r in rows]
    print(f"{reel_dir}")
    print("clip  saved                id                note")
    for n, (slot, mtime, rid) in enumerate(rows, 1):
        note = "same id as another clip: use clip = N" if ids.count(rid) > 1 else ""
        print(f"{n:>4}  {time.strftime('%Y-%m-%d %H:%M', time.localtime(mtime))}  {rid}  {note}")
    if pull is None:
        return 0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dst = (REPO / f"out/reels/odin-{stamp}") if str(pull) == "auto" else pull.resolve()
    dev.pull_guest(reel_dir, dst)
    log(f"copied the device's reels to {dst}")
    if install_windows:
        if not WIN_USER_DATA:
            die("--install-windows needs SKATE3_WINDOWS_USER_DATA (rs.sh sets it)")
        rel = Path(reel_dir).relative_to(GUEST_DATA)
        win = Path(WIN_USER_DATA) / rel
        if win.exists():
            backup = REPO / f"out/reels/windows-REPLY_SKATER-backup-{stamp}"
            shutil.copytree(win, backup)
            shutil.rmtree(win)
            log(f"backed up the Windows test profile's reels to {backup}")
        shutil.copytree(dst, win)
        log(f"installed into the Windows test profile: {win}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    # Game arguments go after `--`; everything argparse does not recognise
    # is passed to the game (parse_known_args below).
    p_local = sub.add_parser("local")
    p_local.add_argument("scenario", type=Path)
    p_local.add_argument("--exe", required=True, type=Path)
    p_local.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_odin = sub.add_parser("odin2")
    p_odin.add_argument("scenario", type=Path)
    p_odin.add_argument("--no-deploy", action="store_true")
    p_odin.add_argument("--no-build", action="store_true",
                        help="deploy the existing linux-aarch64 build without rebuilding it")
    p_odin.add_argument("--config", choices=CONFIGS, default="release",
                        help="which linux-aarch64 build to deploy (default release)")
    p_odin.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_odin.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                        help="session environment for this run (repeatable), e.g. "
                             "LD_PRELOAD=/root/renderdoc/librenderdoc.so")
    p_win = sub.add_parser("windows", help="run a scenario on this PC (Windows x64 build)")
    p_win.add_argument("scenario", type=Path)
    p_win.add_argument("--config", choices=CONFIGS, default="release")
    p_win.add_argument("--backend", choices=BACKENDS, default="d3d12")
    p_win.add_argument("--no-build", action="store_true",
                       help="run the staged build as is (default: build.sh <config> build stage)")
    p_win.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_win.add_argument("--renderdoc", nargs="?", const="", metavar="DIR",
                       help="launch through RenderDoc (renderdoccmd capture) so `capture` steps "
                            "work; DIR is a RenderDoc folder (default: $SKATE3_WINDOWS_RENDERDOC)")
    p_win.add_argument("--timeout", type=float,
                       help="kill the game after this many seconds "
                            "(default: the scenario's timeout_s + 120)")
    p_ab = sub.add_parser("ab", help="run a scenario on the Odin 2 and on Windows, then compare")
    p_ab.add_argument("scenario", type=Path)
    p_ab.add_argument("--config", choices=CONFIGS, default="release")
    p_ab.add_argument("--backends", default="d3d12,vulkan",
                      help="Windows backends to run, comma-separated (default d3d12,vulkan)")
    p_ab.add_argument("--no-build", action="store_true",
                      help="skip both builds (the version check still warns on mismatches)")
    p_ab.add_argument("--no-deploy", action="store_true", help="reuse the build on the Odin 2")
    p_ab.add_argument("--no-odin2", action="store_true", help="Windows targets only")
    p_ab.add_argument("--fps-cap", type=float, default=30.0,
                      help="guest frame cap on every target (default 30; 0 = leave uncapped). "
                           "Scenario frame counts (input holds, waits) only compare at equal "
                           "frame rates; this PC runs ~150 fps, the Odin 2 ~30")
    p_ab.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                      help="Odin 2 session environment (repeatable)")
    p_ab.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_live = sub.add_parser("live")
    p_live.add_argument("--target", choices=("odin2", "windows"), default="odin2")
    p_live.add_argument("--no-build", action="store_true",
                        help="odin2: deploy the existing linux-aarch64 build without rebuilding it")
    p_live.add_argument("--no-deploy", action="store_true",
                        help="odin2: reuse the deployed build; windows: skip the build")
    p_live.add_argument("--config", choices=CONFIGS, default="release")
    p_live.add_argument("--backend", choices=BACKENDS, default="d3d12", help="windows only")
    p_live.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    p_sym = sub.add_parser("symbolize",
                           help="function and file:line for module+offset or bare skate3 addresses")
    p_sym.add_argument("addrs", nargs="+", metavar="ADDR",
                       help="module+0xOFFSET (offset from the module's load base / RVA) "
                            "or 0xADDRESS in skate3's own address space")
    p_sym.add_argument("--target", choices=("odin2", "windows"), default="odin2")
    p_sym.add_argument("--config", choices=CONFIGS,
                       help="default: the deployed config for odin2, release for windows")
    p_send = sub.add_parser("send")
    p_send.add_argument("verb")
    p_send.add_argument("pairs", nargs="*")
    p_send.add_argument("--wait", type=float, default=120)
    p_send.add_argument("--target", choices=("odin2", "windows"), default="odin2")
    p_send.add_argument("--config", choices=CONFIGS,
                        help="windows: whose stage to talk to (default: the last `live`)")
    p_rd = sub.add_parser("install-renderdoc",
                          help="copy tools/harness/build-renderdoc-aarch64.sh output to the device")
    p_rd.add_argument("--from", dest="rd_from", type=Path, default=REPO / "out/renderdoc-aarch64")
    p_fetch = sub.add_parser("fetch")
    p_fetch.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_pull = sub.add_parser("pull", help="copy a guest file or folder off the device "
                                         "(read-only; no session needed with adb root)")
    p_pull.add_argument("guest_path", help="absolute guest path, e.g. "
                                           "/root/.local/share/skate3/settings.toml")
    p_pull.add_argument("--to", type=Path, help="destination (default out/device-files/<name>)")
    p_reels = sub.add_parser("reels", help="list the device's saved reels (slot, date, id); "
                                           "optionally copy them off, or into the Windows "
                                           "test profile")
    p_reels.add_argument("--pull", type=Path, nargs="?", const=Path("auto"), metavar="DIR",
                         help="copy the device's REPLY_SKATER folder (default "
                              "out/reels/odin-<stamp>)")
    p_reels.add_argument("--install-windows", action="store_true",
                         help="with --pull: replace the Windows test profile's REPLY_SKATER "
                              "($SKATE3_WINDOWS_USER_DATA) with the device's, after a backup "
                              "under out/reels/")
    p_sum = sub.add_parser("summary")
    p_sum.add_argument("run", type=Path)
    p_perf = sub.add_parser("perf", help="compare two runs' perf.json (CPU stages, GPU passes)")
    p_perf.add_argument("base", type=Path)
    p_perf.add_argument("new", type=Path)
    a, unknown = ap.parse_known_args()
    extra = [x for x in unknown if x != "--"]
    if extra and a.cmd not in ("local", "odin2", "live", "windows", "ab"):
        ap.error(f"unrecognized arguments: {' '.join(extra)}")

    if a.cmd == "summary":
        return summary(a.run)

    if a.cmd == "perf":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import perf_report  # noqa: E402
        return perf_report.compare(a.base, a.new)

    if a.cmd == "symbolize":
        config = a.config
        if config is None:
            deployed = deployed_config() if a.target == "odin2" else "release"
            config = deployed if deployed in CONFIGS else "release"
        return symbolize(a.target, config, a.addrs)

    if a.cmd == "local":
        out = a.out.resolve()
        out.mkdir(parents=True, exist_ok=True)
        args = [str(a.exe.resolve()), f"--skate3_harness_scenario={a.scenario.resolve()}",
                f"--skate3_harness_out={out}", *scenario_flags(a.scenario), *extra]
        log(" ".join(shlex.quote(x) for x in args))
        code = subprocess.run(args).returncode
        current = (out / "current_run.txt")
        if current.exists():
            summary(Path(current.read_text().strip()))
        return code

    if a.cmd == "windows":
        if not a.no_build:
            build_windows(a.config)
        renderdoc = None
        if a.renderdoc is not None:
            rd_dir = a.renderdoc or WIN_RENDERDOC
            if not rd_dir:
                die("--renderdoc needs a RenderDoc folder (or set SKATE3_WINDOWS_RENDERDOC)")
            renderdoc = Path(rd_dir)
        run = run_windows(a.scenario.resolve(), a.config, a.backend, extra, a.out.resolve(),
                          timeout_s=a.timeout, renderdoc=renderdoc)
        if run is None:
            return 4
        log(f"run folder: {run}")
        return summary(run)

    if a.cmd == "live" and a.target == "windows":
        if not a.no_deploy:
            build_windows(a.config)
        WIN_LIVE_CONFIG.parent.mkdir(parents=True, exist_ok=True)
        WIN_LIVE_CONFIG.write_text(a.config + "\n")
        win_launch(a.config, a.backend, ["--skate3_harness=true", *extra])
        log("live mode running on Windows; use `send --target windows`, Ctrl+Shift+R to record")
        return 0

    if a.cmd == "send" and a.target == "windows":
        config = a.config or (WIN_LIVE_CONFIG.read_text().strip()
                              if WIN_LIVE_CONFIG.exists() else "release")
        box = win_stage(config) / WIN_HARNESS_OUT
        cid = f"cli{int(time.time() * 1000)}"
        (box / "inbox").mkdir(parents=True, exist_ok=True)
        part = box / "inbox" / f"{cid}.toml.part"
        part.write_text(command_toml(a.verb, a.pairs, cid))
        part.rename(box / "inbox" / f"{cid}.toml")
        reply_path = box / "outbox" / f"{cid}.json"
        deadline = time.time() + a.wait
        while time.time() < deadline:
            if reply_path.exists() and (reply := reply_path.read_text().strip()):
                print(reply)
                return 0 if json.loads(reply).get("ok") else 1
            if a.verb == "exit" and not win_game_running():
                # The game exits before it writes a reply.
                log("the game exited")
                return 0
            time.sleep(0.5)
        die(f"no reply within {a.wait:.0f} s (is the game running in live mode on Windows, "
            f"{config} stage?)")

    if a.cmd == "ab":
        backends = [b for b in a.backends.split(",") if b]
        bad = [b for b in backends if b not in BACKENDS]
        if bad:
            die(f"unknown backend(s) {', '.join(bad)} (choose from {', '.join(BACKENDS)})")
        scenario, out = a.scenario.resolve(), a.out.resolve()
        if a.fps_cap > 0:
            # Before the user's own flags, so an explicit flag after `--` wins.
            extra = [f"--skate3_guest_fps_cap={a.fps_cap:g}",
                     "--skate3_guest_fps_cap_auto=false", *extra]
        if not a.no_build:
            # Both through build.sh, which reconfigures: the same commit and
            # version string on both sides.
            if not a.no_odin2 and not a.no_deploy:
                build_aarch64(a.config)
            build_windows(a.config)
        runs: list[tuple[str, Path | None]] = []
        if not a.no_odin2:
            dev = Device()
            if not a.no_deploy:
                deploy(dev, a.config)
            runs.append(("odin2/vulkan", run_odin2(dev, scenario, extra, a.env, out)))
        for b in backends:
            runs.append((f"win/{b}", run_windows(scenario, a.config, b, extra, out)))
        stamp = time.strftime("%Y%m%d-%H%M%S")
        return ab_compare(scenario, runs, out / f"ab-{stamp}_{scenario.stem}")

    dev = Device()
    if a.cmd in ("odin2", "live"):
        if not a.no_deploy:
            if not getattr(a, "no_build", False):
                build_aarch64(a.config)  # deploy packages the build dir as it is
            deploy(dev, a.config)
        elif a.config != deployed_config():
            log(f"note: --no-deploy reuses the deployed build ({deployed_config()}), "
                f"not {a.config}")

    if a.cmd == "odin2":
        return summary(run_odin2(dev, a.scenario, extra, a.env, a.out.resolve()))

    if a.cmd == "live":
        launch(dev, ["--skate3_harness=true", *extra], env=a.env)
        log("live mode running; use `send` for commands, Ctrl+Shift+R to record")
        return 0

    if a.cmd == "send":
        cid = f"cli{int(time.time() * 1000)}"
        text = command_toml(a.verb, a.pairs, cid)
        dev.guest(f"mkdir -p {GUEST_HARNESS}/inbox && cat > {GUEST_HARNESS}/inbox/{cid}.toml.part && "
                  f"mv {GUEST_HARNESS}/inbox/{cid}.toml.part {GUEST_HARNESS}/inbox/{cid}.toml", stdin=text)
        deadline = time.time() + a.wait
        while time.time() < deadline:
            reply = dev.guest(f"cat {GUEST_HARNESS}/outbox/{cid}.json 2>/dev/null || true").strip()
            if reply:
                print(reply)
                return 0 if json.loads(reply).get("ok") else 1
            time.sleep(1)
        die(f"no reply within {a.wait:.0f} s (is the game running in live mode?)")

    if a.cmd == "pull":
        dst = (a.to or REPO / "out/device-files" / Path(a.guest_path).name).resolve()
        dev.pull_guest(a.guest_path, dst)
        log(f"pulled {a.guest_path} -> {dst}")
        return 0

    if a.cmd == "reels":
        return reels_cmd(dev, a.pull, a.install_windows)

    if a.cmd == "install-renderdoc":
        src = a.rd_from.resolve()
        if not (src / "librenderdoc.so").exists():
            die(f"{src} has no librenderdoc.so (run tools/harness/build-renderdoc-aarch64.sh)")
        dev.adb_push(src, f"{SHARED}/")
        if dev.phase()[1]:
            dev.stop()
        dev.start_desktop()
        out = dev.guest(
            f"rm -rf {GUEST_RENDERDOC} && cp -r '{SHARED}/{src.name}' {GUEST_RENDERDOC} && "
            f"chmod +x {GUEST_RENDERDOC}/renderdoccmd && {GUEST_RENDERDOC}/renderdoccmd version | head -1",
            timeout=120)
        dev.stop()
        log(f"installed to {GUEST_RENDERDOC}: {out.strip()}")
        log(f"run with: odin2 <scenario> --env SKATE3_RENDERDOC={GUEST_RENDERDOC}")
        return 0

    if a.cmd == "fetch":
        run = fetch(dev, a.out.resolve())
        log(f"run folder: {run}")
        return summary(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
