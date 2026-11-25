"""Run with Xvfb (xvfb-run -a python3 scripts/smoke.py build/view)."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def tiny_png(color):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 80, 60, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress((b"\0" + bytes(color) * 80) * 60))
            + chunk(b"IEND", b""))


def tiny_pdf():
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Count 3 /Kids [4 0 R 6 0 R 8 0 R] >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    for number in range(3):
        content = f"BT /F1 30 Tf 54 650 Td (Glacier report {number + 1}) Tj ET".encode()
        objects.append(
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
            b"<< /Font << /F1 3 0 R >> >> /Contents %d 0 R >>" % (5 + number * 2))
        objects.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream")
    out = bytearray(b"%PDF-1.4\n")
    positions = []
    for number, obj in enumerate(objects, 1):
        positions.append(len(out))
        out.extend(f"{number} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(out)
    out.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode())
    for offset in positions:
        out.extend(f"{offset:010d} 00000 n \n".encode())
    out.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    return out


def wait_for(check):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.025)
    raise AssertionError("viewer did not reach the expected state")


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="view-") as temporary:
        root = Path(temporary)
        config = root / "view"
        config.mkdir()
        (config / "config").write_text("check_updates = false\nwidth = 880\nheight = 700\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=temporary, XDG_CONFIG_HOME=temporary)
        gallery = root / "Gallery with spaces"
        gallery.mkdir()
        one = gallery / "scene2.png"
        two = gallery / "scene10.png"
        one.write_bytes(tiny_png((210, 67, 150)))
        two.write_bytes(tiny_png((38, 90, 210)))
        pdf = root / "three pages.pdf"
        pdf.write_bytes(tiny_pdf())
        bad = root / "broken.png"
        bad.write_text("this file is corrupt")

        def cli(*args, check=True):
            return subprocess.run(
                [str(binary), *map(str, args)], check=check, env=env,
                cwd=root, capture_output=True, text=True, timeout=15)

        def status():
            return json.loads(cli("status").stdout)

        viewer = subprocess.Popen([binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            def ready():
                result = cli("status", check=False)
                return result.returncode == 0

            wait_for(ready)
            assert status()["kind"] == "empty"
            cli("open", one.relative_to(root))
            assert (status()["index"], status()["count"]) == (1, 2)
            cli("next")
            assert status()["path"] == str(two)
            cli("next")
            assert status()["path"] == str(one)
            assert cli("open", bad, check=False).returncode
            assert status()["path"] == str(one)
            cli("zoom", 225)
            assert status()["zoom"] == 225
            cli("gallery", "off")
            assert not status()["gallery"]
            cli("fullscreen", "on")
            assert status()["fullscreen"]
            cli("fullscreen", "off")
            assert not status()["fullscreen"]
            cli("open", pdf)
            assert (status()["kind"], status()["count"]) == ("pdf", 3)
            cli("goto", "3")
            assert status()["index"] == 3
            cli("find", "glacier")
            wait_for(lambda: not status()["searching"])
            assert (status()["matches"], status()["active_match"]) == (3, 1)
            cli("find-next")
            assert status()["active_match"] == 2
            cli("clear-search")
            assert status()["matches"] == 0
            path = cli("--socket").stdout.strip()
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(3)
                client.connect(path)
                client.sendall(b"status\0")
                reply = client.recv(16384)
                assert reply[:1] == b"0" and json.loads(reply[1:])["kind"] == "pdf"
            cli("quit")
            assert viewer.wait(timeout=8) == 0
            assert viewer.stderr.read() == b""
            assert not Path(path).exists()
            cli("open", one)
            assert status()["path"] == str(one)
            cli("quit")
            wait_for(lambda: not Path(path).exists())
        finally:
            if viewer.poll() is None:
                viewer.terminate()
                viewer.wait(timeout=5)
        print("X11 viewer, PDF search, gallery, and socket smoke test passed.")


if __name__ == "__main__":
    main()
