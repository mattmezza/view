"""Run with Xvfb (xvfb-run -a python3 scripts/smoke.py build/view)."""
import json
import ctypes as c
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def tiny_png(color, metadata=False):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    exif = b""
    if metadata:
        # One TIFF IFD points at the EXIF directory. The latter contains exposure and lens.
        exif = (b"II" + struct.pack("<HI", 42, 8) + struct.pack("<H", 2)
                + struct.pack("<HHII", 0x8769, 4, 1, 38)
                + struct.pack("<HHII", 0x8825, 4, 1, 68) + bytes(4)
                + struct.pack("<H", 2) + struct.pack("<HHII", 0x829A, 5, 1, 122)
                + struct.pack("<HHII", 0xA434, 2, 10, 130) + bytes(4)
                + struct.pack("<H", 4)
                + struct.pack("<HHI4s", 1, 2, 2, b"N\0")
                + struct.pack("<HHII", 2, 5, 3, 140)
                + struct.pack("<HHI4s", 3, 2, 2, b"E\0")
                + struct.pack("<HHII", 4, 5, 3, 164) + bytes(4)
                + struct.pack("<II", 1, 320) + b"Test lens\0"
                + struct.pack("<12I", 46, 1, 30, 1, 0, 1, 8, 1, 0, 1, 0, 1))
        exif = chunk(b"eXIf", exif) + chunk(b"tEXt", b"Comment\0Alpine test")
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 80, 60, 8, 2, 0, 0, 0))
            + exif
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


def cursor_hotspot():
    class Cursor(c.Structure):
        _fields_ = [("x", c.c_short), ("y", c.c_short), ("w", c.c_ushort), ("h", c.c_ushort),
                    ("hot_x", c.c_ushort), ("hot_y", c.c_ushort)]

    x11 = c.CDLL("libX11.so.6")
    xfixes = c.CDLL("libXfixes.so.3")
    x11.XOpenDisplay.restype = c.c_void_p
    x11.XOpenDisplay.argtypes = [c.c_char_p]
    x11.XCloseDisplay.argtypes = [c.c_void_p]
    x11.XFree.argtypes = [c.c_void_p]
    xfixes.XFixesGetCursorImage.argtypes = [c.c_void_p]
    xfixes.XFixesGetCursorImage.restype = c.POINTER(Cursor)
    display = x11.XOpenDisplay(None)
    image = xfixes.XFixesGetCursorImage(display)
    try:
        return image.contents.hot_x, image.contents.hot_y
    finally:
        x11.XFree(image)
        x11.XCloseDisplay(display)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="view-") as temporary:
        root = Path(temporary)
        config = root / "view"
        config.mkdir()
        (config / "config").write_text("check_updates = false\nwidth = 880\nheight = 700\n")
        env = dict(os.environ, XDG_RUNTIME_DIR=temporary, XDG_CONFIG_HOME=temporary,
                   XDG_CURRENT_DESKTOP="", DE="generic", XDG_DATA_HOME=str(root / "share"))
        mock = root / "commands"
        mock.mkdir()
        lpstat = mock / "lpstat"
        lpstat.write_text("#!/bin/sh\nprintf 'Test-printer\\n'\n")
        lpstat.chmod(0o755)
        lp = mock / "lp"
        lp.write_text('#!/bin/sh\nset -eu\nprintf "%s\\n" "$@" > "$VIEW_TEST_JOB.args"\n'
                      'cat > "$VIEW_TEST_JOB.tmp"\nmv "$VIEW_TEST_JOB.tmp" "$VIEW_TEST_JOB.pdf"\n')
        lp.chmod(0o755)
        env["PATH"] = f'{mock}:{env["PATH"]}'
        env["VIEW_TEST_JOB"] = str(root / "print")
        gallery = root / "Gallery with spaces"
        gallery.mkdir()
        one = gallery / "scene2.png"
        two = gallery / "scene10.png"
        one.write_bytes(tiny_png((210, 67, 150), metadata=True))
        two.write_bytes(tiny_png((38, 90, 210)))
        pdf = root / "three pages.pdf"
        pdf.write_bytes(tiny_pdf())
        bad = root / "broken.png"
        bad.write_text("this file is corrupt")

        def cli(*args, check=True):
            result = subprocess.run(
                [str(binary), *map(str, args)], env=env,
                cwd=root, capture_output=True, text=True, timeout=15)
            if check and result.returncode:
                raise AssertionError(f"{args}: {result.stderr.strip()}")
            return result

        def status():
            return json.loads(cli("status").stdout)

        def xdo(*args):
            return subprocess.check_output(["xdotool", *map(str, args)], env=env, text=True, timeout=10)

        def inspect(*args):
            return subprocess.check_output(list(map(str, args)), env=env, text=True, timeout=10)

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
            win = xdo("search", "--onlyvisible", "--class", "^view$").strip()
            xdo("windowfocus", win, "mousemove", "--window", win, 350, 300)
            xdo("keydown", "Control_L")
            wait_for(lambda: cursor_hotspot() == (5, 5))
            xdo("click", 4)
            wait_for(lambda: status()["zoom"] > 225)
            xdo("keyup", "Control_L")
            wait_for(lambda: cursor_hotspot() != (5, 5))
            xdo("mousedown", 1)
            wait_for(lambda: cursor_hotspot() == (7, 6))
            xdo("mousemove", "--window", win, 395, 328, "mouseup", 1)
            wait_for(lambda: cursor_hotspot() != (7, 6))
            cli("gallery", "on")
            initial = status()["thumbnail_height"]
            xdo("mousemove", "--window", win, 440, 700 - 40 - initial - 42 + 5,
                "mousedown", 1, "mousemove", "--window", win, 440, 475, "mouseup", 1)
            wait_for(lambda: status()["thumbnail_height"] > initial + 20)
            cli("info", "on")
            wait_for(lambda: status()["metadata_fields"] >= 13 and not status()["metadata_loading"])
            values = set()
            for position in range(363, 610, 24):
                xdo("mousemove", "--window", win, 700, position, "click", 1)
                result = subprocess.run(["xclip", "-o", "-selection", "clipboard"], env=env,
                                        capture_output=True, text=True, timeout=5)
                values.add(result.stdout)
            assert {"Test lens", "46.500000, 8.000000"} <= values, values
            assert any("1/320" in value for value in values), values
            cli("info", "off")
            cli("present", "on")
            wait_for(lambda: status()["presenting"] and status()["fullscreen"])
            xdo("mousemove", "--window", win, 350, 300, "click", 1)
            wait_for(lambda: status()["index"] == 2)
            xdo("click", 1)
            assert status()["index"] == 2
            xdo("click", 3)
            wait_for(lambda: status()["index"] == 1)
            xdo("key", "Escape")
            wait_for(lambda: not status()["presenting"] and not status()["fullscreen"])
            cli("print")
            time.sleep(.5)

            def print_option(row, direction=1):
                xdo("mousemove", "--window", win, 407 if direction > 0 else 192, 182 + 33 * row,
                    "click", 1)

            def print_job():
                xdo("mousemove", "--window", win, 751, 574, "click", 1)
                job = root / "print.pdf"
                return wait_for(lambda: job if job.exists() else None)

            print_option(0)
            print_option(1)
            print_option(3)
            print_option(4)
            print_option(4)
            print_option(6, -1)
            print_option(7)
            print_option(8)
            print_option(9)
            job = print_job()
            report = inspect("pdfinfo", job)
            assert "Pages:           1" in report and "841.89 x 595.276 pts" in report, report
            raster = inspect("pdfimages", "-list", job)
            assert sum(" gray " in line for line in raster.splitlines()) == 2, raster
            args = (root / "print.args").read_text().splitlines()
            assert {"Test-printer", "2", "sides=two-sided-long-edge", "print-color-mode=monochrome"} <= set(args), args
            job.unlink()
            applications = Path(env["XDG_DATA_HOME"]) / "applications"
            applications.mkdir(parents=True)
            (applications / "view.desktop").write_text(cli("--desktop-entry", binary).stdout)
            subprocess.run(["xdg-mime", "default", "view.desktop", "application/pdf"],
                           env=env, check=True, timeout=8)
            subprocess.run(["xdg-open", str(pdf)], env=env, check=True, timeout=8,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            assert (status()["kind"], status()["count"]) == ("pdf", 3)
            cli("goto", "3")
            assert status()["index"] == 3
            initial = status()["sidebar_width"]
            xdo("mousemove", "--window", win, initial - 2, 360, "mousedown", 1,
                "mousemove", "--window", win, initial + 78, 360, "mouseup", 1)
            wait_for(lambda: status()["sidebar_width"] >= initial + 70)
            assert status()["index"] == 3
            xdo("key", "F5")
            wait_for(lambda: status()["presenting"])
            xdo("mousemove", "--window", win, 400, 300, "click", 3)
            wait_for(lambda: status()["index"] == 2)
            xdo("key", "space")
            wait_for(lambda: status()["index"] == 3)
            xdo("key", "Escape")
            wait_for(lambda: not status()["presenting"])
            cli("print")
            time.sleep(.3)
            print_option(1)
            xdo("type", "2-3")
            print_option(5)
            text = inspect("pdftotext", print_job(), "-")
            assert "Glacier report 1" not in text and "Glacier report 2" in text and "Glacier report 3" in text, text
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
        print("X11 cursors, dock sizing, metadata, presentation, CUPS jobs, PDF search, and socket passed.")


if __name__ == "__main__":
    main()
