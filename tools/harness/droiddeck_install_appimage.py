#!/usr/bin/env python3
"""Replace an imported AppImage app in DroidDeck with a new AppImage, through
DroidDeck's own UI (droiddeckctl has no import/remove command).

    droiddeck_install_appimage.py APPIMAGE [--name "Skate3Recomp Testing"]
                                           [--smoke] [--dry-run]

Steps (each waits for the UI element it needs instead of sleeping):
  1. refuses unless the session is IDLE (Remove/Add are disabled while a
     session runs, and a run would be killed);
  2. Desktop -> the app named --name (default: the testing app) -> Remove ->
     confirm; skipped if no such app is installed;
  3. adb push to /storage/emulated/0/Download/ and checks the SHA-256;
  4. Desktop -> Add -> AppImage -> Choose file -> Search -> filter -> pick
     the file -> Add (the name comes from the AppImage);
  5. waits for /opt/appimages/user/<id>/app and checks the embedded version
     (adb root reads the guest);
  6. --smoke: launches it with droiddeck-appimage-run, screenshots after 20 s,
     stops the session;
  7. deletes the Download copy.

Speed: every droiddeckctl call is an `adb shell content call`, which starts
a Java process on the device (~1 s). So the script dumps the UI once per
screen and taps node centres with `adb shell input tap` (~80 ms) instead of
`droiddeckctl ui click` (another ~1 s lookup per click).

--dry-run: only reads the UI and the guest (which app would be removed),
changes nothing. Traps and the manual recipe: docs/linux-aarch64.md
("A local testing build").
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

DDCTL = Path(os.environ.get("DROIDDECKCTL", Path.home() / "dev/DroidDeck/tools/droiddeckctl"))
ADB = os.environ.get("ADB", "adb")
GUEST = "/data/data/com.droiddeck.launcher/files/linuxfs"
DOWNLOAD = "/storage/emulated/0/Download"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def ddctl(*args: str, check: bool = True) -> dict:
    r = subprocess.run([sys.executable, str(DDCTL), *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit(f"droiddeckctl {' '.join(args)} failed: {r.stderr.strip() or r.stdout}")
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {}


def adb(*args: str, check: bool = True) -> str:
    r = subprocess.run([ADB, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit(f"adb {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


def root(cmd: str) -> str:
    return adb("shell", f"su -c '{cmd}'", check=False)


def nodes() -> list[dict]:
    out = []
    for w in ddctl("ui", "dump").get("windows", []):
        for n in w.get("nodes", []):
            n["label"] = n.get("text") or n.get("description") or ""
            x0, y0, x1, y1 = n.get("bounds") or (0, 0, 0, 0)
            if x1 > x0 and y1 > y0:  # on screen (off-screen rows report zero width)
                out.append(n)
    return out


def wait_for(pred, what: str, timeout: float = 15.0) -> list[dict]:
    """Dumps until pred(nodes) holds (each dump ~1 s, so no extra sleep)."""
    end = time.time() + timeout
    while True:
        ns = nodes()
        if pred(ns):
            return ns
        if time.time() > end:
            raise SystemExit(f"timed out waiting for {what}")


def has(label: str, exact: bool = False):
    return lambda ns: any((n["label"] == label) if exact else (label in n["label"]) for n in ns)


def find(ns: list[dict], pred, what: str) -> dict:
    for n in ns:
        if pred(n):
            return n
    raise SystemExit(f"no {what} on screen")


def tap(n: dict) -> None:
    """Taps a node's centre (screen coordinates of display 0)."""
    x0, y0, x1, y1 = n["bounds"]
    adb("shell", "input", "tap", str((x0 + x1) // 2), str((y0 + y1) // 2))


def by_label(label: str, exact: bool = True):
    return lambda n: (n["label"] == label) if exact else (label in n["label"])


def by_tag(tag: str):
    return lambda n: n.get("tag") == tag


def back_to_main(tries: int = 8) -> list[dict]:
    """Presses Back until no dialog or picker is open (the first Back may only
    hide the keyboard; one Back per layer after that)."""
    for _ in range(tries):
        ns = nodes()
        if not any(n["label"] in ("Choose an AppImage", "Add an app") or
                   n["label"].startswith("Remove ") and n["label"].endswith("?") for n in ns):
            return ns
        adb("shell", "input", "keyevent", "KEYCODE_BACK")
        time.sleep(0.4)
    raise SystemExit("couldn't close DroidDeck's dialogs with Back")


def picker_path(ns: list[dict]) -> str:
    for n in ns:
        if n["label"].startswith("/storage/"):
            return n["label"]
    return ""


def filter_picker(ns: list[dict], text: str) -> list[dict]:
    """Types `text` into the picker's folder filter (opening it, focusing it
    and clearing what's there first)."""
    if not has("Filter this folder")(ns) and not any(
            n.get("focused") and 350 < n["bounds"][3] < 500 for n in ns):
        tap(find(ns, by_label("Search"), "the picker's Search"))
        ns = wait_for(lambda ns: any(350 < n["bounds"][1] < 380 and n["bounds"][3] < 500
                                     and n.get("clickable") for n in ns), "the picker's filter")
    field = find(ns, lambda n: 350 < n["bounds"][1] < 380 and n["bounds"][3] < 500
                 and n.get("clickable"), "the filter field")
    tap(field)
    # Type only once the field has focus, or the text is lost.
    wait_for(lambda ns: any(n.get("focused") and 350 < n["bounds"][1] < 380 for n in ns),
             "the filter field's focus")
    # The field's label doesn't report its text: clear generously.
    adb("shell", "input", "keyevent", "KEYCODE_MOVE_END", *(["KEYCODE_DEL"] * 48))
    adb("shell", "input", "text", re.sub(r"[^A-Za-z0-9._]", "", text))
    # In landscape the keyboard goes full screen (extract mode) and covers
    # the whole display, while the UI dump still lists the app's nodes under
    # it: a tap would hit a key. One Back hides it.
    adb("shell", "input", "keyevent", "KEYCODE_BACK")
    time.sleep(0.3)
    return nodes()


def picker_to_download(ns: list[dict]) -> list[dict]:
    """The picker reopens in the last folder it showed: walk to Download."""
    for _ in range(8):
        path = picker_path(ns)
        if path == DOWNLOAD:
            return ns
        if path == "/storage/emulated/0":
            ns = filter_picker(ns, "Download")
            ns = wait_for(lambda ns: any(n["label"].startswith("Download ") for n in ns),
                          "the Download folder")
            tap(find(ns, lambda n: n["label"].startswith("Download "), "Download"))
        else:  # somewhere else: one level up with the picker's own Back
            tap(find(ns, by_label("Back"), "the picker's Back"))
        ns = wait_for(lambda ns, p=path: picker_path(ns) not in ("", p), "the next folder")
    raise SystemExit("couldn't reach Download in the file picker")


def installed(name: str) -> list[str]:
    ids = []
    for d in root(f"ls {GUEST}/opt/appimages/user/").split():
        if root(f"cat {GUEST}/opt/appimages/user/{d}/name").strip() == name:
            ids.append(d)
    return ids


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("appimage", type=Path)
    ap.add_argument("--name", default="Skate3Recomp Testing",
                    help="the installed app to replace (its DroidDeck name)")
    ap.add_argument("--smoke", action="store_true", help="launch it once and screenshot")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    image = a.appimage.resolve()
    if not image.is_file():
        raise SystemExit(f"{image} not found")
    m = re.search(r"-(\d+\.\d+\.\d+[^-]*(?:-dev\.g[0-9a-f]+)?|\d+\.\d+\.\d+-testing\.\d+)-aarch64",
                  image.name)
    version = m.group(1) if m else None

    phase = ddctl("state").get("session", {}).get("phase")
    log(f"session phase {phase}; image {image.name} (version {version or '?'})")
    if phase != "IDLE":
        raise SystemExit("a session is running: stop it first (droiddeckctl stop) - "
                         "Remove/Add are disabled and a run would be killed")

    back_to_main()
    old = installed(a.name)
    log(f"installed as '{a.name}': {old or 'none'}")
    if a.dry_run:
        tap(find(nodes(), by_tag("rail-desktop"), "the Desktop rail"))
        ns = wait_for(has("Add", exact=True), "the Desktop page")
        log("dry run: Desktop page has " + ", ".join(sorted({n['label'] for n in ns
                                                            if 'AppImage' in n['label']})))
        return 0

    # 2. Remove the old app.
    if old:
        tap(find(nodes(), by_tag("rail-desktop"), "the Desktop rail"))
        ns = wait_for(has(a.name), f"'{a.name}' on the Desktop page")
        tap(find(ns, by_label(a.name, exact=False), a.name))
        ns = wait_for(has("Remove", exact=True), "the app page's Remove")
        tap(find(ns, lambda n: n["label"] == "Remove" and n.get("clickable"), "Remove"))
        ns = wait_for(has(f"Remove {a.name}?"), "the remove confirmation")
        title = find(ns, by_label(f"Remove {a.name}?"), "the confirmation title")
        # The sheet's danger button: the clickable "Remove" below its title.
        tap(find(ns, lambda n: n["label"] == "Remove" and n.get("clickable")
                 and n["bounds"][1] > title["bounds"][1], "the confirm button"))
        end = time.time() + 30
        while installed(a.name):
            if time.time() > end:
                raise SystemExit("the old app is still installed after Remove")
            time.sleep(0.3)
        log(f"removed {old}")

    # 3. Push.
    remote = f"{DOWNLOAD}/{image.name}"
    log("pushing to Download")
    adb("push", str(image), remote)
    local_sha = hashlib.sha256(image.read_bytes()).hexdigest()
    remote_sha = adb("shell", f"sha256sum {remote}").split()[0]
    if local_sha != remote_sha:
        raise SystemExit(f"checksum mismatch after push ({remote_sha} != {local_sha})")

    # 4. Add through the dialog.
    ns = nodes()
    if not has("Add", exact=True)(ns):
        tap(find(ns, by_tag("rail-desktop"), "the Desktop rail"))
        ns = wait_for(has("Add", exact=True), "the Desktop page's Add")
    tap(find(ns, by_label("Add"), "the Desktop page's Add"))
    ns = wait_for(lambda ns: any(n.get("tag") == "tab-1" for n in ns), "the Add dialog")
    tap(find(ns, by_tag("tab-1"), "the AppImage tab"))
    ns = wait_for(has("An ARM64"), "the AppImage tab's text")
    tap(find(ns, by_label("Choose file", exact=False), "Choose file"))
    ns = wait_for(has("Choose an AppImage"), "the file picker")
    ns = picker_to_download(ns)
    key = (version or image.stem).split("-")[-1][:24]
    filter_picker(ns, key)
    ns = wait_for(has(image.name), f"{image.name} in the picker")
    tap(find(ns, by_label(image.name, exact=False), image.name))
    ns = wait_for(lambda ns: sum(1 for n in ns if n["label"] == "Add") >= 2, "the dialog's Add")
    tap([n for n in ns if n["label"] == "Add"][-1])  # the dialog's, after the page's

    # 5. Wait for the import and check it.
    end = time.time() + 120
    new = []
    while time.time() < end:
        new = [d for d in installed(a.name) if root(
            f"test -d {GUEST}/opt/appimages/user/{d}/app/usr && echo ok").strip() == "ok"]
        if new:
            break
        time.sleep(0.5)
    if not new:
        raise SystemExit("the import didn't finish within 2 minutes")
    app = f"/opt/appimages/user/{new[0]}"
    banner = root(f"strings {GUEST}{app}/app/usr/bin/skate3 | grep -m1 \"Skate 3 \\[v\"").strip()
    log(f"installed {new[0]}: {banner}")
    if version and version not in banner:
        raise SystemExit(f"installed binary doesn't report {version}")

    # 6. Smoke launch.
    if a.smoke:
        ddctl("run", "/usr/local/bin/droiddeck-appimage-run", app, check=False)
        end = time.time() + 60
        while time.time() < end and not ddctl("state").get("session", {}).get("firstFrame"):
            pass
        time.sleep(3)  # a few frames in
        shot = Path(os.environ.get("TMPDIR", "/tmp")) / "droiddeck_smoke.png"
        ddctl("screenshot", str(shot), check=False)
        st = ddctl("state").get("session", {})
        log(f"smoke: phase {st.get('phase')}, first frame {st.get('firstFrame')}; screenshot {shot}")
        ddctl("stop", check=False)

    # 7. Clean up.
    adb("shell", "rm", remote, check=False)
    log("done; the Download copy is deleted")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as e:
        if e.code not in (0, None):
            log(f"failed: {e.code}; closing DroidDeck's dialogs")
            try:
                back_to_main()
            except SystemExit:
                pass
        raise
