#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Andrew Spode
# Host-side tool for the X9S camera's USB mass-storage interface. Its main use is `cc-dump`: reading the camera's whole 4 MiB flash over
# USB, with no programmer (see docs/experiments/flash-read-without-t48.md for how it works).
#
# Linux only (it uses the raw SCSI interface, SG_IO) and needs sudo. Pure standard library; run it as `python3 -I`.
#
# SAFETY / STATUS
#   - It uses addresses that belong to ONE firmware build (3030B-20A1-20A1 V1.1) and has only been tried on one camera. On another firmware it
#     may simply fail, or hang or reset the camera (power-cycle it if so). It never writes flash: the only thing it writes is 72 bytes of RAM.
#   - Make sure DEV is the camera and not one of your own disks: run `inquiry` first; the product string should contain "Generic Mass-Storage".
#   - `inquiry`, `read10`, `probe` issue only standard SCSI reads.
#   - `cc-probe` and `cc-read` use the 0xCC direct-memory sender. RAM reads work; direct flash reads return zeros. A bad address can hang/reset
#     the camera.
#   - `cc-install-stub` writes 72 bytes to SRAM (not flash/SD) and verifies them. `cc-stub-*` reads through that stub, capped at 256 bytes.
#   - `cc` is an arbitrary-address call and remains GUARDED OFF: it only prints a CDB.
#   - Another device (even another HX3302B camera) needs its own analysis first; do not point these addresses at it blindly.
#
# Typical use (camera plugged in with its USB cable; DEV is its SCSI generic device, e.g. /dev/sg5):
#   sudo python3 -I tools/usb_flash_read.py inquiry /dev/sgN                    # check it is the camera
#   sudo python3 -I tools/usb_flash_read.py cc-dump /dev/sgN firmware/dump.bin   # read the whole flash (takes a few minutes)
# Find DEV with `lsblk -o NAME,VENDOR,MODEL` and `ls /sys/block/<disk>/device/scsi_generic/`, or `sg_scan -i`.
# cc-dump also writes a log to out/results/cc-dump.log under the repository folder.

import ctypes, fcntl, os, sys

SG_IO = 0x2285
SG_DXFER_NONE = -1
SG_DXFER_TO_DEV = -2
SG_DXFER_FROM_DEV = -3


class sg_io_hdr(ctypes.Structure):
    # Matches the Linux kernel struct sg_io_hdr (natural alignment, 64-bit).
    _fields_ = [
        ("interface_id", ctypes.c_int),
        ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte),
        ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort),
        ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p),
        ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p),
        ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint),
        ("info", ctypes.c_uint),
    ]


def sgio(fd, cdb, direction, dxfer_len, data_out=None, timeout_ms=10000):
    """Send one SCSI command via SG_IO. Returns (sense_bytes, data_bytes)."""
    cdb_buf = (ctypes.c_ubyte * len(cdb))(*cdb)
    sense = (ctypes.c_ubyte * 64)()
    if direction == SG_DXFER_FROM_DEV:
        data = (ctypes.c_ubyte * dxfer_len)()
    elif direction == SG_DXFER_TO_DEV:
        data = (ctypes.c_ubyte * dxfer_len)(*(data_out or b""))
    else:
        data = None

    h = sg_io_hdr()
    h.interface_id = ord("S")
    h.dxfer_direction = direction
    h.cmd_len = len(cdb)
    h.mx_sb_len = 64
    h.dxfer_len = dxfer_len
    h.dxferp = ctypes.cast(data, ctypes.c_void_p) if data is not None else None
    h.cmdp = ctypes.cast(cdb_buf, ctypes.c_void_p)
    h.sbp = ctypes.cast(sense, ctypes.c_void_p)
    h.timeout = timeout_ms

    fcntl.ioctl(fd, SG_IO, h)

    sense_out = bytes(sense[: h.sb_len_wr]) if h.sb_len_wr else b""
    if h.status != 0 or h.host_status != 0 or h.driver_status != 0:
        raise RuntimeError(
            "SCSI command failed: status=0x%02x host=0x%04x driver=0x%04x sense=%s"
            % (h.status, h.host_status, h.driver_status, sense_out.hex())
        )
    data_out_b = bytes(data[: dxfer_len - (h.resid or 0)]) if data is not None else b""
    return sense_out, data_out_b


def cmd_inquiry(dev):
    fd = os.open(dev, os.O_RDWR)
    try:
        cdb = [0x12, 0, 0, 0, 96, 0]  # INQUIRY, alloc 96
        _, data = sgio(fd, cdb, SG_DXFER_FROM_DEV, 96)
    finally:
        os.close(fd)
    vendor = data[8:36].decode("latin-1", "replace") if len(data) >= 36 else ""
    print("INQUIRY raw:", data.hex())
    print("vendor/product:", repr(vendor))
    if "Generic Mass-Storage" in vendor or "Beacon" in vendor:
        print("  -> matches the firmware's INQUIRY string; this is the HX3302B stack.")


def read10(fd, lba, count):
    cdb = [
        0x28, 0,
        (lba >> 24) & 0xFF, (lba >> 16) & 0xFF, (lba >> 8) & 0xFF, lba & 0xFF,
        0,
        (count >> 8) & 0xFF, count & 0xFF,
        0,
    ]
    _, data = sgio(fd, cdb, SG_DXFER_FROM_DEV, 512 * count)
    return data


def cmd_read10(dev, lba, count, outfile):
    fd = os.open(dev, os.O_RDWR)
    try:
        data = read10(fd, lba, count)
    finally:
        os.close(fd)
    if outfile:
        with open(outfile, "wb") as f:
            f.write(data)
        print("wrote %d bytes to %s" % (len(data), outfile))
    else:
        sys.stdout.buffer.write(data)


def cmd_probe(dev):
    """Read sector 0 over USB and compare against dump.bin to tell
    empirically whether the USB disk is the internal flash or the SD card."""
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dump = os.path.join(repo, "firmware", "dump.bin")
    fd = os.open(dev, os.O_RDWR)
    try:
        sec0 = read10(fd, 0, 1)
    finally:
        os.close(fd)
    print("USB sector 0 (first 64 bytes):", sec0[:64].hex())
    if os.path.exists(dump):
        with open(dump, "rb") as f:
            flash0 = f.read(512)
        if sec0 == flash0:
            print("MATCH: USB sector 0 == dump.bin sector 0.")
            print("  -> the USB disk is the INTERNAL FLASH. (Would be a surprise; see usb-modes.md.)")
        else:
            print("NO MATCH vs flash sector 0 (expected: 'BLDR' at offset 4 -> %s)."
                  % flash0[4:8])
            print("  -> consistent with the USB disk being the SD CARD, not internal flash.")
            print("     Disassembly says READ(10) is SD/DMA-backed; internal flash needs the")
            print("     0xCC path (see docs/experiments/flash-read-without-t48.md).")
    else:
        print("(dump.bin not found; cannot compare. Run from the repo, or copy it over.)")


def cmd_cc(dev, addr):
    """Vendor command 0xCC: firmware CALLs the 32-bit addr (CDB bytes 1-4, little-endian).
    GUARDED OFF. This just prints the CDB for an arbitrary call."""
    cdb = [0xCC, addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF,
           (addr >> 24) & 0xFF, 0, 0, 0, 0, 0]
    print("0xCC CDB that WOULD be sent (call addr 0x%08X):" % addr, bytes(cdb).hex())
    print("NOT SENDING. An arbitrary call address can hang the camera.")


CC_READ_SENDER = 0x06024868
CC_WRITE_RECEIVER = 0x060248D8
CC_RESPONSE_BUFFER = 0x8040
CC_READ_STUB = 0x81F0
CC_STUB_MAX_READ = 0x100
NO_FILL_CALLBACK = 0xFFFFFFFF
# Generated from tools/usb_read_stub.c by tools/build_usb_read_stub.sh.
CC_READ_STUB_BYTES = bytes.fromhex(
    "f4ff219c008001d4049001d40800048624004386041823e20490b2e0048070e00848"
    "01d438009184a1ce8105049092e0048070e00400418600000186080021850c00219c"
    "84718001"
)


def cc_cdb(target, source):
    return ([0xCC]
            + list(target.to_bytes(4, "little"))
            + list(source.to_bytes(4, "little"))
            + list(NO_FILL_CALLBACK.to_bytes(4, "little"))
            + [0, 0, 0])


def cc_read(fd, source, length, target=CC_READ_SENDER):
    """Read through the generic sender or the staged copy-and-send stub."""
    if not 1 <= length <= 0xFFFF:
        raise ValueError("0xCC reads are limited to 1..65535 bytes per command")
    if target == CC_READ_STUB and length > CC_STUB_MAX_READ:
        raise ValueError("stub reads are limited to 256 bytes to keep code and data separate")
    _, data = sgio(fd, cc_cdb(target, source), SG_DXFER_FROM_DEV, length)
    if len(data) != length:
        raise RuntimeError("short SCSI read: requested %d bytes, received %d" % (length, len(data)))
    return data


def cc_write(fd, dest, data):
    """Write bytes to a RAM address through the generic 0xCC data-out receiver."""
    if not 1 <= len(data) <= 0xFFFF:
        raise ValueError("0xCC writes are limited to 1..65535 bytes per command")
    sgio(fd, cc_cdb(CC_WRITE_RECEIVER, dest), SG_DXFER_TO_DEV, len(data), data)


def cmd_cc_read(dev, source, length, outfile, target=CC_READ_SENDER):
    fd = os.open(dev, os.O_RDWR)
    try:
        data = cc_read(fd, source, length, target)
    finally:
        os.close(fd)
    if outfile:
        with open(outfile, "wb") as f:
            f.write(data)
        print("wrote %d bytes to %s" % (len(data), outfile))
    else:
        sys.stdout.buffer.write(data)


def install_cc_stub(fd, log=print):
    stub = CC_READ_STUB_BYTES
    if not stub or CC_READ_STUB + len(stub) > 0x8244:
        raise ValueError("stub is empty or overruns the USB response buffer")
    if CC_READ_STUB < CC_RESPONSE_BUFFER + CC_STUB_MAX_READ:
        raise ValueError("stub overlaps the maximum-size response data")
    cc_write(fd, CC_READ_STUB, stub)
    returned = cc_read(fd, CC_READ_STUB, len(stub))
    if returned != stub:
        raise RuntimeError("staged stub did not read back correctly; do not execute it")
    log("Staged and verified %d-byte read stub at SRAM 0x%04X." % (len(stub), CC_READ_STUB))


def cmd_cc_install_stub(dev):
    fd = os.open(dev, os.O_RDWR)
    try:
        install_cc_stub(fd)
    finally:
        os.close(fd)


def cc_probe(dev, target):
    source = 0x06000000
    length = CC_STUB_MAX_READ if target == CC_READ_STUB else 512
    fd = os.open(dev, os.O_RDWR)
    try:
        data = cc_read(fd, source, length, target)
    finally:
        os.close(fd)
    print("0xCC flash offset 0 (first 64 bytes):", data[:64].hex())
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dump = os.path.join(repo, "firmware", "dump.bin")
    with open(dump, "rb") as f:
        expected = f.read(length)
    if data == expected:
        print("MATCH: all %d bytes read over 0xCC equal the verified flash dump." % length)
        return
    differences = [i for i, (a, b) in enumerate(zip(data, expected)) if a != b]
    if all(0x14 <= i < 0x18 for i in differences):
        print("MATCH except for the header timestamp at 0x14-0x17:")
        print("  verified dump: %s; current flash: %s" % (expected[0x14:0x18].hex(), data[0x14:0x18].hex()))
    else:
        print("NO MATCH: %d of %d compared bytes differ at offsets %s."
              % (len(differences), len(expected), ", ".join(hex(i) for i in differences[:16])))


def cmd_cc_dump(dev, outfile, offset=0, length=4 * 1024 * 1024):
    if offset < 0 or length < 0 or offset + length > 4 * 1024 * 1024:
        raise ValueError("dump range must lie within the 4 MiB flash")
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    log_path = os.path.join(repo, "out", "results", "cc-dump.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    end = offset + length
    written = 0
    with open(log_path, "w") as log_file:
        def log(message):
            print(message, flush=True)
            log_file.write(message + "\n")
            log_file.flush()

        fd = os.open(dev, os.O_RDWR)
        try:
            install_cc_stub(fd, log)
            log("Dumping flash offset 0x%X, length 0x%X to %s" % (offset, length, outfile))
            with open(outfile, "wb") as f:
                for pos in range(offset, end, CC_STUB_MAX_READ):
                    count = min(CC_STUB_MAX_READ, end - pos)
                    data = cc_read(fd, 0x06000000 + pos, count, CC_READ_STUB)
                    f.write(data)
                    written += len(data)
                    if written % (256 * 1024) == 0 or written == length:
                        log("read %d of %d bytes" % (written, length))
            log("wrote %d bytes to %s" % (written, outfile))
        except Exception as exc:
            log("ERROR: %s" % exc)
            raise
        finally:
            os.close(fd)
    print("log written to %s" % log_path)


def _main(argv):
    if len(argv) < 2:
        print(__doc__ or "", file=sys.stderr)
        print("usage: usb_flash_read.py {inquiry|probe|read10|cc|cc-probe|cc-read|cc-install-stub|cc-stub-probe|cc-stub-read|cc-dump} DEV [args]", file=sys.stderr)
        return 2
    sub, dev = argv[0], argv[1]
    if sub == "inquiry":
        cmd_inquiry(dev)
    elif sub == "probe":
        cmd_probe(dev)
    elif sub == "read10":
        lba = int(argv[2], 0)
        count = int(argv[3], 0) if len(argv) > 3 else 1
        outfile = argv[4] if len(argv) > 4 else None
        cmd_read10(dev, lba, count, outfile)
    elif sub == "cc":
        cmd_cc(dev, int(argv[2], 0))
    elif sub == "cc-probe":
        cc_probe(dev, CC_READ_SENDER)
    elif sub == "cc-read":
        source = int(argv[2], 0)
        length = int(argv[3], 0)
        outfile = argv[4] if len(argv) > 4 else None
        cmd_cc_read(dev, source, length, outfile)
    elif sub == "cc-install-stub":
        cmd_cc_install_stub(dev)
    elif sub == "cc-stub-probe":
        cc_probe(dev, CC_READ_STUB)
    elif sub == "cc-stub-read":
        source = int(argv[2], 0)
        length = int(argv[3], 0)
        outfile = argv[4] if len(argv) > 4 else None
        cmd_cc_read(dev, source, length, outfile, CC_READ_STUB)
    elif sub == "cc-dump":
        offset = int(argv[3], 0) if len(argv) > 3 else 0
        length = int(argv[4], 0) if len(argv) > 4 else 4 * 1024 * 1024
        cmd_cc_dump(dev, argv[2], offset, length)
    else:
        print("unknown subcommand %r" % sub, file=sys.stderr)
        return 2
    return 0


def main(argv):
    try:
        return _main(argv)
    except PermissionError:
        print("error: permission denied - run this with sudo", file=sys.stderr)
    except OSError as e:
        print("error: %s\n       Is the first argument the camera's SCSI generic device (e.g. /dev/sg5), and is the camera plugged in as a USB disk?\n"
              "       Check with the `inquiry` sub-command; if the camera hangs, unplug it, power-cycle it and try again." % e, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
