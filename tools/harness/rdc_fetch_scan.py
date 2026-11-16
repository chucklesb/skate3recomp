"""Scan a RenderDoc capture of the emulated renderer for guest vertex fetches.

Runs inside qrenderdoc's embedded Python (Windows, RenderDoc 1.46 portable):

    qrenderdoc.exe --python tools/harness/rdc_fetch_scan.py

with the arguments in the environment (qrenderdoc's --python takes no
script arguments):

    RDC_SCAN_CAPTURE  the .rdc
    RDC_SCAN_OUT      where to write the report (text)
    RDC_SCAN_LO/HI    guest address range to look for (hex, e.g. BC6C0000 /
                      BC6F0000); a draw is listed when one of its bound
                      constant blocks holds a Xenos vertex fetch constant
                      (dword0 = physical address | type 3) inside the range

For each draw it prints the event id, index count, and every matching
fetch (guest address). Used for KI-125: which vertex buffer the game's own
hair / shirt draws read on the frame after a reel seek. The emulated
renderer's draws carry no labels, so the vertex buffer address is the way
to find them.
"""
import os
import struct
import sys

import renderdoc as rd

CAP = os.environ["RDC_SCAN_CAPTURE"]
OUT = os.environ["RDC_SCAN_OUT"]
LO = int(os.environ.get("RDC_SCAN_LO", "BC6C0000"), 16)
HI = int(os.environ.get("RDC_SCAN_HI", "BC6F0000"), 16)


def phys(g):
    return g & 0x1FFFFFFF


def main(out):
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(CAP, "", None)
    if st != rd.ResultCode.Succeeded:
        out.write("open failed: %s\n" % st)
        return
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if st != rd.ResultCode.Succeeded:
        out.write("replay failed: %s\n" % st)
        return
    plo, phi = phys(LO), phys(HI)
    actions = []

    def walk(lst):
        for a in lst:
            actions.append(a)
            walk(a.children)

    walk(ctrl.GetRootActions())
    draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
    presents = [a.eventId for a in actions if a.flags & rd.ActionFlags.Present]
    out.write("actions %d draws %d presents %s\n" % (len(actions), len(draws), presents))
    out.flush()
    cache = {}
    for k, a in enumerate(draws):
        if k % 100 == 0:
            out.write("progress %d/%d eid %d\n" % (k, len(draws), a.eventId))
            out.flush()
        ctrl.SetFrameEvent(a.eventId, False)
        pipe = ctrl.GetPipelineState()
        hits = []
        for stage in (rd.ShaderStage.Vertex,):
            try:
                blocks = pipe.GetConstantBlocks(stage, False)
            except TypeError:
                blocks = pipe.GetConstantBlocks(stage)
            for b in blocks:
                d = b.descriptor
                rid = d.resource
                if rid == rd.ResourceId.Null():
                    continue
                key = (int(rid), d.byteOffset, d.byteSize)
                if key not in cache:
                    size = d.byteSize if d.byteSize and d.byteSize < (1 << 20) else 65536
                    cache[key] = bytes(ctrl.GetBufferData(rid, d.byteOffset, size))
                data = cache[key]
                n = len(data) // 4
                for i in range(n):
                    w = struct.unpack_from("<I", data, i * 4)[0]
                    if (w & 3) == 3 and plo <= (w & ~3) < phi:
                        hits.append("0x%08X@cb%d+%d" % ((w & ~3) | (LO & 0xE0000000), int(rid), i * 4))
        if hits:
            out.write("eid %d idx %d inst %d  %s\n" % (a.eventId, a.numIndices, a.numInstances,
                                                     " ".join(sorted(set(hits)))))
            out.flush()
        # Cached constant data changes per draw for push / dynamic buffers.
        cache.clear()
    ctrl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


with open(OUT, "w") as f:
    try:
        main(f)
    except Exception as e:  # report, then quit the UI either way
        import traceback
        f.write("error: %s\n%s" % (e, traceback.format_exc()))
os._exit(0)
