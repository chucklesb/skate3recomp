#!/usr/bin/env python3
"""Run scenario-harness scenarios locally or on a DroidDeck handheld.

    run_scenario.py local  <scenario.toml> --exe <skate3 or AppImage> [-- args...]
    run_scenario.py odin2  <scenario.toml> [--no-deploy] [-- args...]
    run_scenario.py live   [--no-deploy] [-- args...]     # live mode (inbox + recording)
    run_scenario.py send   <verb> [key=value ...]          # one live command, prints the reply
    run_scenario.py fetch                                  # copy the device's latest run folder
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
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GUEST_APP_DIR = "/root/skate3-harness"
GUEST_HARNESS = "/root/.local/share/skate3/harness"
SHARED = "/storage/emulated/0/Download/skate3-harness"
DEFAULT_ADB_PORT_HINT = "ANDROID_ADB_SERVER_PORT"


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
        if not d.get("ok", False):
            raise RuntimeError(f"could not start a desktop session: {d.get('error')}")

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


def build_testing_appimage() -> Path:
    out = REPO / "out/harness"
    for old in out.glob("*.AppImage*"):
        old.unlink()
    script = (
        ". ~/.nix-profile/etc/profile.d/nix.sh 2>/dev/null; "
        "nix develop .#cross --command bash -c "
        "'SKATE3_APPIMAGE_SKIP_VALIDATE=1 SKATE3_APPIMAGE_VARIANT=testing "
        "tools/appimage/build-appimage.sh out/build/linux-aarch64-release out/harness'"
    )
    subprocess.run(["bash", "-c", script], cwd=REPO, check=True, stdout=subprocess.DEVNULL)
    images = sorted(out.glob("skate3recomp-testing-*-aarch64.AppImage"))
    if not images:
        die("build-appimage.sh produced no testing AppImage")
    return images[-1]


def deploy(dev: Device) -> None:
    image = build_testing_appimage()
    log(f"deploying {image.name}")
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


def launch(dev: Device, args: list[str]) -> None:
    if dev.phase()[1]:
        dev.stop()
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
    """Copy the guest's latest run folder to out_root; works after exit."""
    if not dev.phase()[1]:
        dev.start_desktop()
        started = True
    else:
        started = False
    run_dir = dev.guest(f"cat {GUEST_HARNESS}/current_run.txt").strip()
    if not run_dir:
        die("no harness run on the device")
    name = Path(run_dir).name
    dev.guest(f"rm -rf {SHARED}/run && mkdir -p {SHARED} && cp -r '{run_dir}' {SHARED}/run", timeout=120)
    if started:
        dev.stop()
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


def scenario_flags(path: Path) -> list[str]:
    """Init-time flags a scenario needs before the harness starts."""
    flags = []
    if scenario_settings(path).get("vulkan_validation"):
        # The Vulkan instance exists before the harness loads the scenario.
        flags += ["--vulkan_validation_enabled=true", "--vulkan_log_debug_messages=true"]
    return flags


def summary(run: Path) -> int:
    result_path = run / "result.json"
    if not result_path.exists():
        print(f"{run}: no result.json (still running, or the game crashed)")
        return 4
    r = json.loads(result_path.read_text())
    print(f"{run.name}: {r['status']} (code {r['code']}) - {r['reason']}")
    print(f"  {r['frames']} frames, {r['seconds']:.1f} s, {r['screenshots']} screenshots, "
          f"{r['warnings']} warnings, {r['errors']} errors")
    if r.get("validation_enabled"):
        print(f"  Vulkan validation: {r.get('validation_messages', 0)} messages "
              f"(validation.txt)")
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
    return int(r["code"])


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
            lit = v if v in ("true", "false") else json.dumps(v)
        lines.append(f"{k} = {lit}")
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_local = sub.add_parser("local")
    p_local.add_argument("scenario", type=Path)
    p_local.add_argument("--exe", required=True, type=Path)
    p_local.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_local.add_argument("extra", nargs=argparse.REMAINDER)
    p_odin = sub.add_parser("odin2")
    p_odin.add_argument("scenario", type=Path)
    p_odin.add_argument("--no-deploy", action="store_true")
    p_odin.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_odin.add_argument("extra", nargs=argparse.REMAINDER)
    p_live = sub.add_parser("live")
    p_live.add_argument("--no-deploy", action="store_true")
    p_live.add_argument("extra", nargs=argparse.REMAINDER)
    p_send = sub.add_parser("send")
    p_send.add_argument("verb")
    p_send.add_argument("pairs", nargs="*")
    p_send.add_argument("--wait", type=float, default=120)
    p_fetch = sub.add_parser("fetch")
    p_fetch.add_argument("--out", type=Path, default=REPO / "out/harness-runs")
    p_sum = sub.add_parser("summary")
    p_sum.add_argument("run", type=Path)
    a = ap.parse_args()
    extra = [x for x in getattr(a, "extra", []) if x != "--"]

    if a.cmd == "summary":
        return summary(a.run)

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

    dev = Device()
    if a.cmd in ("odin2", "live") and not a.no_deploy:
        deploy(dev)

    if a.cmd == "odin2":
        dev.adb_push(a.scenario.resolve(), f"{SHARED}/scenarios/{a.scenario.name}")
        launch(dev, [f"--skate3_harness_scenario={SHARED}/scenarios/{a.scenario.name}",
                     *scenario_flags(a.scenario), *extra])
        log(f"running {a.scenario.name}")
        wait_for_exit(dev, scenario_timeout(a.scenario) + 120)
        run = fetch(dev, a.out.resolve())
        log(f"run folder: {run}")
        return summary(run)

    if a.cmd == "live":
        launch(dev, ["--skate3_harness=true", *extra])
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

    if a.cmd == "fetch":
        run = fetch(dev, a.out.resolve())
        log(f"run folder: {run}")
        return summary(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
