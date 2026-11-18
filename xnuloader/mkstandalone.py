#!/usr/bin/env python3
"""Build standalone payload packaging kernel + root26.apfs + rdfile (LZ4 compressed).

Packages kernel, root26.apfs, and --rdfile darwin:5917904896:0 into an XNUP container,
then compresses the payload with LZ4 to produce ~26 MB standalone boot payload fitting
within the 96 MiB boot_a partition.

Root-disk gate (tranche 13, item 7): the packed root disk must be the one
tools/k90auto/package.py made, carrying the tools/userland-driver build.
find_default_ramdisk() still lists the old candidates, but main() refuses to
pack any of them except the package.py output, and verifies it against the
pack manifest (root hash + driver hash). If package.py has not run, the run
fails loudly here instead of silently packing userland/root26.apfs -- that
image carries the userland/src (16.5 line) driver, and booting it would run
the wrong driver with no warning at all.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

DEFAULT_RDFILE = "darwin:5917904896:0"
DEFAULT_IDENTITY = "v57"
DEFAULT_MEMSIZE = "300000000"
# Default boot-args for a WARSAW (REDMI K90 Ultra) standalone payload.
#
# This file's previous default was the annibale (REDMI K90) one: panel geometry
# 1206x2622, pcie_bdf=bd_p11.elf (annibale's board data) and a one-step
# pcie=10 + wifi/ufs-write enable set. None of that is verified on warsaw, and
# the geometry/board-data values are simply wrong for this device, so the
# default is now the warsaw L0 baseline: the cmdline of the v92 handover runs
# (docs/STATUS-2026-11-09.md; staged plan in the fork's PHASE2-UW-PREP /
# PHASE3-HARDWARE-KIT notes). Per-value sources:
#   debug=0x10e rootdev=md0   v92 baseline (RAM-disk root, DSU persistence)
#   cpus=7 bootcpu=7          v92 evidence-run convention (production runs
#                             must still carry these; loader_return_after
#                             must NOT be added -- it is forensic-only)
#   epoch=<now>               required, else the clock is broken (README trap)
#   fbwidth=1280 fbheight=2772 fbpitch=460
#                             warsaw panel, native 1:1 (common/devices/
#                             warsaw.conf: FB_BASE splash region 1280x2772)
#   adreno8=23 touchscreen=2 buttons=1 battery=1 gpu_coherent=1 metal=1
#   cpufreq=1                 v92 baseline feature set (touchscreen=2 is the
#                             Goodix path, same as warsaw.conf BOOTARGS)
# Deliberately NOT in the default (staged bring-up, one variable at a time;
# pass via --cmdline when a stage is actually being run):
#   PCIe/Wi-Fi stages use pcie_bdf=bd_p12x.elf -- warsaw's board data from
#   its own kiwi/ firmware (bd_p11.elf is annibale's; do not use it here).
#   L1 adds pcie=1..6 stepwise, L2 pcie=7 pcie_power=1 pcie_fw=2, L4 adds
#   pcie=10 wifi=1. UFS probing starts at ufs=2 alone (read-only).
DEFAULT_CMDLINE = (
    f"debug=0x10e rootdev=md0 cpus=7 bootcpu=7 epoch={int(time.time())} "
    "fbwidth=1280 fbheight=2772 fbpitch=460 adreno8=23 touchscreen=2 "
    "buttons=1 battery=1 gpu_coherent=1 metal=1 cpufreq=1"
)


def find_default_kernel(root_dir: Path) -> Path:
    candidates = [
        root_dir / "out/xnu12377-st4.bin",
        root_dir / "out/ios26-resume/k90-ready/inputs/kernel26",
        root_dir / "out/ios26-resume/k90-ready/sources/kernel",
        root_dir / "out/ios26-resume/input26-formal-inputs/kernel26",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def find_default_ramdisk(root_dir: Path) -> Path:
    candidates = [
        root_dir / "out/ios26-resume/bbmetal-device/root.apfs",
        root_dir / "userland/root26.apfs",
        root_dir / "out/ios26-resume/k90-ready/inputs/root26.apfs",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def assert_root_disk_driver(ramdisk: Path, root_dir: Path) -> None:
    """Loud-fail unless the ramdisk is the package.py output carrying the tools/ driver.

    The check is three links, each of which fails with the fix attached:
      1. out/ios26-resume/bbmetal-device/root.apfs exists -- i.e. package.py ran.
         If it does not, there is no verified iOS 26 root disk at all, and the
         old silent fallback to userland/root26.apfs is refused: that image
         carries the userland/src (16.5 line) driver.
      2. The ramdisk IS that file. Anything else (an explicit -r at
         userland/root26.apfs, a k90-ready staging, a stale copy) is refused:
         its driver is unverified, and the 16.5-line one is the known wrong
         answer for an iOS 26 boot.
      3. The pack manifest binds the image to the tools/ driver: the image
         hash equals manifest root_sha256, and the manifest driver_sha256
         equals the staged tools-built driver (out/.../libredwoodmetal.dylib).
         package.py asserts the same binding at pack time when it copies the
         driver into the image; this re-checks it at pack-the-payload time, so
         a replaced image or driver cannot slip through.
    Deliberately no escape hatch: a payload that boots the wrong driver fails
    silently on the device, 400 seconds into a run, looking like a registration
    problem. Failing here is the whole point.
    """
    packaged = root_dir / "out/ios26-resume/bbmetal-device/root.apfs"
    tools_driver = root_dir / "out/ios26-resume/bbmetal-device/libredwoodmetal.dylib"
    if not packaged.is_file():
        raise SystemExit(
            "mkstandalone: package.py has not run -- "
            "out/ios26-resume/bbmetal-device/root.apfs does not exist, so there is no "
            "verified iOS 26 root disk to pack. Refusing to fall back to "
            "userland/root26.apfs: that image carries the userland/src (16.5 line) "
            "driver, not the tools/userland-driver build. "
            "Run: python3 tools/k90auto/package.py")
    if ramdisk.resolve() != packaged.resolve():
        raise SystemExit(
            f"mkstandalone: refusing to pack {ramdisk}: it is not the package.py output "
            f"({packaged}). Only the package.py output is verified to carry the "
            "tools/userland-driver build; anything else (e.g. userland/root26.apfs) "
            "carries the 16.5-line driver. Run: python3 tools/k90auto/package.py")
    manifest_path = packaged.parent / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(
            f"mkstandalone: {manifest_path} is missing: the package.py output has no "
            "pack manifest, so the image cannot be tied to the tools/ driver. "
            "Re-run: python3 tools/k90auto/package.py")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("root_sha256") != sha256_file(packaged):
        raise SystemExit(
            "mkstandalone: the root image is not the one its manifest describes "
            f"(manifest root_sha256 {str(manifest.get('root_sha256'))[:16]}, image hashes "
            f"to {sha256_file(packaged)[:16]}): stale or replaced since packing. "
            "Re-run: python3 tools/k90auto/package.py")
    if not tools_driver.is_file():
        raise SystemExit(
            f"mkstandalone: the staged tools-built driver {tools_driver} is missing, so "
            "the manifest driver hash has nothing to check against. "
            "Re-run: python3 tools/k90auto/package.py")
    if manifest.get("driver_sha256") != sha256_file(tools_driver):
        raise SystemExit(
            "mkstandalone: the manifest driver hash does not match the staged "
            "tools-built driver: the driver was replaced after packing, or the image "
            "was packed from a different driver. "
            "Re-run: python3 tools/k90auto/package.py")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1048576):
            h.update(chunk)
    return h.hexdigest()


def compress_lz4(input_path: Path, output_path: Path) -> Path:
    lz4_bin = shutil.which("lz4")
    if lz4_bin:
        cmd = [lz4_bin, "-f", "-9", str(input_path), str(output_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"lz4 compression failed: {res.stderr}")
    else:
        # Fallback pure-python basic lz4 frame or subprocess python -m
        raise RuntimeError("lz4 command not found in PATH")
    return output_path


def build_standalone_payload(
    kernel: Path,
    ramdisk: Path,
    output: Path,
    lz4_output: Path | None = None,
    rdfile: str = DEFAULT_RDFILE,
    identity: str = DEFAULT_IDENTITY,
    memsize: str = DEFAULT_MEMSIZE,
    cmdline: str = DEFAULT_CMDLINE,
) -> tuple[Path, Path]:
    here = Path(__file__).resolve().parent
    mkpayload_py = here / "mkpayload.py"
    if not mkpayload_py.is_file():
        raise FileNotFoundError(f"mkpayload.py not found at {mkpayload_py}")

    if not kernel.is_file():
        raise FileNotFoundError(f"kernel not found at {kernel}")
    if not ramdisk.is_file():
        raise FileNotFoundError(f"ramdisk not found at {ramdisk}")

    output.parent.mkdir(parents=True, exist_ok=True)
    if lz4_output is None:
        lz4_output = output.with_name(output.name + ".lz4")
    lz4_output.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(mkpayload_py),
        "-o",
        str(output),
    ]
    loader_efi = here / "loader-phone.efi"
    if loader_efi.is_file():
        cmd.append(f"loader.efi={loader_efi}")
    cmd.extend([
        f"kernel={kernel}",
        f"ramdisk={ramdisk}",
        "--rdfile",
        rdfile,
        "--identity",
        identity,
        "--memsize",
        memsize,
        "--cmdline",
        cmdline,
    ])

    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"mkpayload.py failed:\n{res.stderr}\n{res.stdout}")

    compress_lz4(output, lz4_output)

    return output, lz4_output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = Path(__file__).resolve().parent
    kernel_root = here.parent

    parser.add_argument("-k", "--kernel", type=Path, default=find_default_kernel(kernel_root),
                        help="Path to kernel Mach-O")
    parser.add_argument("-r", "--ramdisk", type=Path, default=find_default_ramdisk(kernel_root),
                        help="Path to root ramdisk (root26.apfs)")
    parser.add_argument("-o", "--output", type=Path,
                        default=kernel_root / "out/standalone-payload.bin",
                        help="Path to output uncompressed payload")
    parser.add_argument("--lz4-output", type=Path, default=None,
                        help="Path to output LZ4 compressed payload")
    parser.add_argument("--rdfile", default=DEFAULT_RDFILE,
                        help="rdfile partition:size:offset specification")
    parser.add_argument("--identity", default=DEFAULT_IDENTITY,
                        help="identity (v57 for iOS 26)")
    parser.add_argument("--memsize", default=DEFAULT_MEMSIZE,
                        help="memsize in hex")
    parser.add_argument("--cmdline", default=DEFAULT_CMDLINE,
                        help="boot arguments cmdline")
    parser.add_argument("--test", action="store_true",
                        help="Run self-test with dummy kernel/ramdisk")

    args = parser.parse_args()

    if args.test:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            dummy_k = tmp / "kernel"
            dummy_r = tmp / "root.apfs"
            dummy_out = tmp / "payload.bin"
            dummy_lz4 = tmp / "payload.bin.lz4"

            # Create dummy Mach-O and apfs bytes
            dummy_k.write_bytes(b"\xcf\xfa\xed\xfe" + b"\x00" * 1024)
            dummy_r.write_bytes(b"NXSB" + b"\x00" * 1024)

            out_bin, out_lz = build_standalone_payload(
                kernel=dummy_k,
                ramdisk=dummy_r,
                output=dummy_out,
                lz4_output=dummy_lz4,
                rdfile="darwin:1048576:0",
            )
            assert out_bin.is_file() and out_bin.stat().st_size > 0
            assert out_lz.is_file() and out_lz.stat().st_size > 0
            print("mkstandalone self-test: PASS")
            return 0

    # Root-disk gate (item 7): refuse to pack anything but the verified
    # package.py output. The --test branch above returns before this point,
    # so the self-test (dummy bytes, no APFS image) is unaffected.
    assert_root_disk_driver(args.ramdisk, kernel_root)

    out_bin, out_lz = build_standalone_payload(
        kernel=args.kernel,
        ramdisk=args.ramdisk,
        output=args.output,
        lz4_output=args.lz4_output,
        rdfile=args.rdfile,
        identity=args.identity,
        memsize=args.memsize,
        cmdline=args.cmdline,
    )

    bin_size = out_bin.stat().st_size
    lz_size = out_lz.stat().st_size
    bin_sha = sha256_file(out_bin)
    lz_sha = sha256_file(out_lz)

    print(f"Standalone payload generated:")
    print(f"  Raw: {out_bin} ({bin_size} bytes, {bin_size / 1048576:.2f} MiB), sha256: {bin_sha}")
    print(f"  LZ4: {out_lz} ({lz_size} bytes, {lz_size / 1048576:.2f} MiB), sha256: {lz_sha}")
    print(f"  Target partition: boot_a (96 MiB). Payload fits within limit: {lz_size < 96 * 1048576}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
