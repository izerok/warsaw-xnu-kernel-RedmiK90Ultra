#!/usr/bin/env python3
"""Assemble the /redwood root image (iOS 26) for the warsaw port, or just its driver.

This is the warsaw fork's port of iola1999/hackios-redmi-k90-annibale's
tools/k90auto/package.py (1169 lines, port main @ 00c4cce). Annibale's script
assembles the /annibale root image out of three sibling repositories
(hackios-xnu-kernel, hackios-xnu-userland, TrollStore); this fork is a single
repository, so every path below is resolved inside this tree, with the
userland checkout the host lays over it (common/setup.sh, @438e0ca) as the
fallback for the sources this fork does not carry. The renames are
mechanical -- annibale -> redwood, /annibale -> /redwood, Annibale ->
Redwood, ANNIBALE_* -> REDWOOD_* -- except where a comment says otherwise.

Two modes:
  package.py                  the full pack (host only): build.sh stages the
                              base root first (IOS=../ios26 bash build.sh),
                              then this builds every library, installs them,
                              and writes root.apfs + manifest.json into
                              out/ios26-resume/bbmetal-device/.
  package.py --driver-only    CI mode: build only the tools/userland-driver
                              driver (dylib + bundle), verify it against the
                              AIR library and its build record, write
                              manifest.json, and stop. No base root, no
                              mkapfs, no jobs, no root.apfs.

The full pack cannot run in CI, by construction, not by omission: staging
the base root needs ../ios26, the iOS 26 root filesystem taken out of
Apple's IPSW on the host (userland/build.sh:615, :761, :788, :891), and
root.apfs would carry Apple's launchd and system plists, which this fork
never puts in an artifact or a commit. The driver package is the CI
artifact; the full root.apfs assembly stays a Mac-host step, unblocked by
this port.

Source resolution (resolve()): a logical "userland/src/<nested>" path is
looked up fork-flat first (userland/src/<basename>, the files this fork
carries) and overlay-nested second (the userland checkout's copy). The
manifest records which side each source came from, so a pack names its
inputs. The old two-tree sync check (diff -rq between the kernel and the
userland checkouts' userland/src) is gone: there is one tree now.

AIR coupling (the part that changed most): the AIR library is built by
tools/k90auto/build-air.py (host) or the mesa-air workflow, which writes
air-build-record.json. This script checks the library hash against the
record, checks the mesa-side air_metal_copy.h when this workspace has one,
pins the driver-side copy's hash in the manifest, and runs the rwa_*
subset check (the real ABI gate: the five calls mesa_available() refuses
a library without, plus a warning for any other looked-up call the
library lacks). The two air_metal_copy.h copies have diverged -- the mesa
side carries the stitching API, the driver side carries writeback /
thumbnail / mirror -- so the old "driver copy == record's mesa copy"
equality cannot hold and is not asserted; the subset check is what
catches a real mismatch. AIR_REQUIRED defaults to the three strings
verified present in this fork's AIR library (fd6_program, "rwa draw:",
"resolve %llu"); annibale's strings air_allocate_memory,
air_metal_reflection.h and rwa_stitch_node are NOT in it (checked
2026-11-18 against the air-artifacts library), so do not pass them via
PAYLOAD_AIR_STRINGS for this fork.

usage: python3 tools/k90auto/package.py [--driver-only]
env:   AIR_LIB / AIR_RECORD (default: the host build-air.py outputs),
       GPU_SPEED, PROBE, WIFI_FW, BT_FW, IRIS_FW, REST, AUDIO, BLUETOOTH,
       MSAA, REDWOOD_DIAG, JOBS_DROP, JOBS_SOURCE, PAYLOAD_PROGRAMS,
       PAYLOAD_AIR_STRINGS, ROUND_NOTES, IOS (default ../ios26)
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
WORKSPACE = REPO
SRC_ROOT = REPO
# The iOS 26 root filesystem, taken out of Apple's IPSW on the host
# (../ios26/fetch.sh). Same default as userland/build.sh's $IOS.
IOS_DIR = Path(os.environ.get("IOS", str(REPO.parent / "ios26")))
OUT = WORKSPACE / "out/ios26-resume/bbmetal-device"
# PAYLOAD_PROGRAMS=path[:path] installs programs built outside build.sh into /bin of the root image.
# No default: this fork ships no TrollStore helper.
EXTRA_PROGRAMS = [Path(p) for p in os.environ.get("PAYLOAD_PROGRAMS", "").split(":") if p]
# The AIR library and its build record. Defaults are the host build-air.py
# outputs; the packaging workflow points them at the air-artifacts download.
AIR_LIB = Path(os.environ.get("AIR_LIB", str(OUT / "libredwoodair.dylib")))
AIR_RECORD = Path(os.environ.get(
    "AIR_RECORD", str(WORKSPACE / "out/ios26-resume/mesa-arm64e/air-build-record.json")))


def resolve(logical):
    """Resolve a logical source path to the file this tree actually builds.

    userland/src/<nested> is looked up fork-flat first
    (userland/src/<basename>) and overlay-nested second (the userland
    checkout's copy, laid over this tree by common/setup.sh). tools/ paths
    resolve directly. Returns the Path, or None when neither side has it.
    """
    p = Path(logical)
    if p.parts[:2] == ("userland", "src"):
        flat = REPO / "userland/src" / p.name
        if flat.is_file():
            return flat
    direct = REPO / p
    if direct.is_file():
        return direct
    return None


def resolve_origin(logical):
    """Where resolve() found a source: "fork", "overlay", or None."""
    p = Path(logical)
    if p.parts[:2] == ("userland", "src"):
        if (REPO / "userland/src" / p.name).is_file():
            return "fork"
        if (REPO / p).is_file():
            return "overlay"
        return None
    if logical.startswith("tools/mesa/"):
        return "overlay" if (REPO / p).is_file() else None
    return "fork" if (REPO / p).is_file() else None


def resolve_or_fail(logical, what):
    got = resolve(logical)
    if got is None:
        raise SystemExit(
            f"{logical} is missing: {what}. On the host, run common/setup.sh "
            "so the userland checkout (@438e0ca) lays its copy over this tree.")
    return got


# The AIR library is not compiled here; tools/k90auto/build-air.py is the only entry point that
# builds it, because the arm64e build reads its own copy of the sources and an unsynced edit builds
# a valid library that is missing what the round is meant to measure. Its record says which library
# that build produced and which format strings it holds: both are checked here, and
# PAYLOAD_AIR_STRINGS (colon separated) adds the fields this round depends on. The default is the
# three strings verified present in this fork's AIR library (checked 2026-11-18 against the
# air-artifacts library 68fef479...); annibale's air_allocate_memory,
# air_metal_reflection.h and rwa_stitch_node are NOT in it, so do not pass them here.
AIR_REQUIRED_DEFAULT = ["fd6_program", "rwa draw:", "resolve %llu"]
# PAYLOAD_AIR_STRINGS overrides the default, ";"-separated: one of the strings
# contains both a space and a colon ("rwa draw:"), so neither whitespace nor ":"
# can be the separator.
AIR_REQUIRED = [t for t in os.environ.get("PAYLOAD_AIR_STRINGS", "").split(";") if t] \
    or AIR_REQUIRED_DEFAULT
# PROBE=1 builds userland/src/lsprobe.m into /bin/lsprobe and adds lines to rc.jobs that run it
# at 60 s and 300 s. It is read-only: it asks LSApplicationWorkspace and MobileInstallationLookup for
# the application list and prints the count, the bundle identifiers and the extension records each
# proxy carries. The console cannot answer that question on its own -- SpringBoard says six lines in a
# whole boot -- and two icons on a desktop that may have had fifteen is exactly the difference between
# "LaunchServices knows two applications" and "LaunchServices knows them all and none renders".
PROBE = os.environ.get("PROBE") == "1"
PROBE_SOURCES = ["userland/src/apps/lsprobe.m"]
PROBE_FLAGS = ["-Os", "-Wall", "-Wextra", "-framework", "Foundation"]
# Two processes, not one with two offsets: the t=60 side has been observed to disappear before it
# reaches its second offset, and the answers at t=300 do not depend on the earlier ones. Each is
# backgrounded so a slow probe cannot hold the rest of the boot script.
PROBE_LINE = "/redwood/bin/lsprobe 60 &\n/redwood/bin/lsprobe 300 &\n"
# The Wi-Fi card's firmware goes into fw/ of the image, from the directory
# tools/mk-fw-wifi.py extracted out of the phone's own modem partition (not in git).
# By default, when that directory has them, only the four files the driver reads on warsaw go in
# (mkstandalone.py's pcie_bdf=bd_p12x.elf): amss20.bin (the card is hardware 2.0),
# regdb.bin, bd_p12x.elf and phy_ucode20.elf, about 6.9 MB. All of them, about 27 MB, do not fit beside a
# /private/var seed in the 40.9 MB between the kernel and the resident system image (make-payload.py).
# WIFI_FW=1 puts in every file, for the driver's diagnostic rounds; WIFI_FW=0 none.
# The kernel's PCIe step 7 (pcie=7, mhi_pcie_fw.c) boots the card with amss20.bin or amss.bin from there;
# step 9 (mhi_pcie_wlfw.c) sends it the regulatory database, the board data file named by its board id
# and the M3 image. bd_p12x*.elf, regdb_xiaomi.bin and qdss_trace_config_v2.cfg are what Android's cnss2
# sends on this phone (pcie_bdf= picks a board data file by name, pcie_qdss=1 sends the QDSS file).
# lfw-*.bin, when there, are linux-firmware's ath12k/WCN7850/hw2.0 amss.bin and m3.bin, copied by hand
# (pcie_fw=3).
WIFI_FW_DIR = (WORKSPACE / "out/fw-wifi" if (WORKSPACE / "out/fw-wifi").is_dir()
               else REPO.parent / "out/fw-wifi")
WIFI_FW_USED = ["amss20.bin", "regdb.bin", "bd_p12x.elf", "phy_ucode20.elf"]
WIFI_FW_MODE = os.environ.get("WIFI_FW", "used" if all((WIFI_FW_DIR / n).exists() for n in WIFI_FW_USED) else "0")
WIFI_FW = WIFI_FW_MODE != "0"
WIFI_FW_FILES = (WIFI_FW_USED if WIFI_FW_MODE == "used" else
                 ["amss.bin", "amss20.bin", "regdb.bin", "regdb_xiaomi.bin", "phy_ucode20.elf"] +
                 sorted(p.name for p in WIFI_FW_DIR.glob("bdwlan.e*")) +
                 sorted(p.name for p in WIFI_FW_DIR.glob("bd_p12x*.elf")) +
                 sorted(p.name for p in WIFI_FW_DIR.glob("qdss_trace_config*.cfg")) +
                 sorted(p.name for p in WIFI_FW_DIR.glob("lfw-*.bin")) if WIFI_FW else [])
# By default, when tools/mk-fw-bt.py has extracted the phone's bluetooth partition
# into out/fw-bt, the three files WCN7850 ROM 0x20 board 0 uses on warsaw go into fw/ (hmtbtfw20.tlv,
# hmtbtfw20.ver and hmtnv20.bin, 285 KB; kernel-12377-st2-r1). BT_FW=all puts in every hmt* file; BT_FW=0 none.
BT_FW_DIR = (WORKSPACE / "out/fw-bt" if (WORKSPACE / "out/fw-bt").is_dir()
             else REPO.parent / "out/fw-bt")
BT_FW_USED = ["hmtbtfw20.tlv", "hmtbtfw20.ver", "hmtnv20.bin"]
BT_FW_MODE = os.environ.get("BT_FW", "used" if all((BT_FW_DIR / n).exists() for n in BT_FW_USED) else "0")
BT_FW = BT_FW_MODE != "0"
BT_FW_FILES = (BT_FW_USED if BT_FW_MODE in ("used", "1") and all((BT_FW_DIR / n).exists() for n in BT_FW_USED) else
               sorted(p.name for p in BT_FW_DIR.glob("hmt*")) if BT_FW else [])
assert not BT_FW or BT_FW_FILES, f"BT_FW={BT_FW_MODE}, but {BT_FW_DIR} has no hmt* files (tools/mk-fw-bt.py extract)"
IRIS_FW_DIR = (WORKSPACE / "out/fw-iris" if (WORKSPACE / "out/fw-iris").is_dir()
               else REPO.parent / "out/fw-iris" if (REPO.parent / "out/fw-iris").is_dir()
               else WORKSPACE / "fw-iris")
IRIS_FW_FILE = "vpu35_4v.mbn"
IRIS_FW = os.environ.get("IRIS_FW", "1" if (IRIS_FW_DIR / IRIS_FW_FILE).is_file() else "0") == "1"
assert not IRIS_FW or (IRIS_FW_DIR / IRIS_FW_FILE).is_file(), f"IRIS_FW=1, but {IRIS_FW_DIR / IRIS_FW_FILE} is missing"
# configd gives an interface a network service (DHCP and the rest) at the first boot of a data volume;
# for one that appears later, InterfaceNamer adds a service only when its control preference
# ConfigureNewInterfaces is set (configd's ifnamer.c: on iOS it "normally" configures no interface after
# the first boot). en0 appears about 30 s after configd starts, so without the preference it never gets
# an address, and wifid, which waits for the joined network to become routable, gives the join up. With
# WIFI_FW the preference goes into fw/ and two lines of the data volume template copy it to
# /Library/Preferences/SystemConfiguration (a link to /private/var/preferences) before configd starts.
WIFI_CONTROL = "com.apple.InterfaceNamer.control.plist"
WIFI_CONTROL_PLIST = plistlib.dumps({"ConfigureNewInterfaces": True})
WIFI_CONTROL_LINES = ["d\t755\t0\t0\t/private/var/preferences/SystemConfiguration",
                      f"f\t644\t0\t0\t/private/var/preferences/SystemConfiguration/{WIFI_CONTROL}\t/fw/{WIFI_CONTROL}"]

# Application containers are created by MobileContainerManager from the bundle identifier.
# The packer keeps only generic parent directories in userland/var.skel.extra and never assigns
# a process-specific home, AppGroup UUID, or temporary directory.
# userland/build.sh's staging directory; xpc/launchd.plist is there only when it ran with IOS=../ios26.
BASE = SRC_ROOT / "userland/root"
TOOLS = WORKSPACE / "tools/apfs"
# XPCWATCH="NAME ..." inserts libredwood-xpcwatch.dylib (userland/src/xpcwatch.c) into the programs
# with those file names: it puts the named services they connect to, with counts, into the kernel
# log ("say PID NAME: xpcwatch ..."). For finding the clients that reconnect in a loop.
XPCWATCH_PROCESSES = os.environ.get("XPCWATCH", "").split()
# libredwood-ax.dylib goes into these programs unless AX is set to the empty string. userland/src/ax.m:
# on the notification com.redwood.ax each writes what it shows, labels and frames, to /tmp/ax-NAME.txt,
# which tools/k90auto/ui.py reads to tap by name; it does nothing until that notification.
# userland/src/statusbar26.m: the status bar's time and battery 12 points closer to the screen edges.
# Each application lays out its own status bar, so one not named here keeps iOS's place
# (docs/analysis/2026-11-02-status-bar-layout.md). init hands etc/inject.conf to kern.exec_inject;
# the size check below follows the kernel buffer limit.
AX_PROCESSES = os.environ.get("AX", "SpringBoard Preferences MobileSafari Weather MobileSMS MobileNotes MobileMail "
                              "MobileCal Files").split()
# MercuryPosterExtension, the provider of the default wallpaper, gets the Metal device unless
# POSTER_METAL=0: it draws the wallpaper with its own Metal shaders (default.metallib in its bundle),
# and without a device the wallpaper window hosts its context empty. It is an ExtensionKit service
# with no launchd job, so the inserted library is the whole switch (sb_metal_registration_sequence.inc).
POSTER_METAL = os.environ.get("POSTER_METAL", "1") == "1"
# TOUCH_LAT is a 16.5-line facility and is not carried to the 26 pack: it would inject
# libredwood.dylib into backboardd, which build.sh forbids on V=26 (the input26-service job
# owns the input queue), and the shim's hooks install nothing on iOS 26 anyway.
TOUCH_LAT = os.environ.get("TOUCH_LAT") == "1"
if TOUCH_LAT:
    raise SystemExit("TOUCH_LAT=1 is a 16.5-line facility: the 26 pack does not inject "
                     "libredwood.dylib into backboardd (build.sh refuses it on V=26).")
INJECT = (
    "/redwood/lib/libredwood-sbmetal-experiment.dylib SpringBoard backboardd" +
    (" iconservicesagent" if os.environ.get("ICON_METAL", "1") == "1" else "") +
    (" MercuryPosterExtension" if POSTER_METAL else "") + "\n" +
    "/redwood/lib/libredwood-sb26.dylib SpringBoard PosterBoard MercuryPosterExtension WidgetRenderer_Default SharingUIService Preferences lsd wifid\n"
    "/redwood/lib/libredwood-media.dylib cameracaptured mediaserverd\n"
    "/redwood/lib/libredwood-vt.dylib videocodecd mediaserverd com.apple.WebKit.GPU video_decode_test video_player_test\n"
    "/redwood/lib/libredwood-app.dylib Calculator MobileNotes Reminders MobileCal Preferences MobileSMS systemstatusd Weather weatherd Maps geod MobileSafari com.apple.WebKit.Networking com.apple.WebKit.WebContent nsurlsessiond chronod\n" +
    ("/redwood/lib/libredwood-xpcwatch.dylib " + " ".join(XPCWATCH_PROCESSES) + "\n"
     if XPCWATCH_PROCESSES else "") +
    ("/redwood/lib/libredwood-ax.dylib " + " ".join(AX_PROCESSES) + "\n" if AX_PROCESSES else "")
)
JOBS = "etc/rc.jobs"
# JOBS_SOURCE=... runs a different touch script than the acceptance one. A diagnostic that has
# to catch the screen mid-drag needs its own timing, and the acceptance script stays as it is.
DEFAULT_JOBS = HERE / "jobs/rc.jobs.interactive" if (HERE / "jobs/rc.jobs.interactive").exists() else HERE / "rc.jobs.desktop"
JOBS_SOURCE = Path(os.environ.get("JOBS_SOURCE", DEFAULT_JOBS))
# Whether the name above came from the environment or from the default. A round that ran the
# default and a round that asked for that same file are different statements, and the archive has
# to carry which one happened: two rounds with 3550 and 17 frames differ by their gesture script,
# and that was not recoverable from either manifest before this field existed.
JOBS_SOURCE_GIVEN = "JOBS_SOURCE" in os.environ
JOB = "LaunchDaemons/com.apple.backboardd.plist"
CACHE_KEY = "/System/Library/LaunchDaemons/com.apple.backboardd.plist"
# iconservicesagent renders the icons applications ask IconServices for. Without a Metal device it
# renders them in software, and IconRendering then refuses to serialize every one of them
# ("TextureIOError.usingSoftwareRenderer", 230 times in the ffma1 round): the General page of
# Settings showed grey squares. ICON_METAL=1, the default, gives it the same registration as
# backboardd; the GPU stop that kept it off in lock2cpu4 (2026-10-30) has not recurred since render47.
# ICON_METAL=0 leaves iconservicesagent in software.
ICON_METAL = os.environ.get("ICON_METAL", "1") == "1"
# backboardd draws only the dirty regions of a frame by default (DIRTY_REGIONS=1); DIRTY_REGIONS=0 sets
# CA_DISABLE_DIRTY_REGIONS=1 to draw every frame whole for comparison.
DIRTY_REGIONS = os.environ.get("DIRTY_REGIONS", "1") == "1"
ICON_JOB = "LaunchDaemons/com.apple.iconservices.iconservicesagent.plist"
ICON_CACHE_KEY = "/System/Library/LaunchDaemons/com.apple.iconservices.iconservicesagent.plist"
# MSAA=1 asks the driver for four samples on both processes that encode render passes:
# backboardd draws the window server's layers, SpringBoard its own, and a run of one without
# the other compares two different devices. Off by default, because the answer changes what
# every application that asks -supportsSampleCount: does.
MSAA = os.environ.get("MSAA") == "1"
MSAA_JOBS = ["com.apple.backboardd.plist", "com.apple.SpringBoard.plist"]
# JOB_ENV="a.plist:KEY=VALUE b.plist:KEY=VALUE" adds one environment variable per item to a job, in
# its plist and in the launchd cache. The ones in use: OS_ACTIVITY_DT_MODE=YES sends a job's os_log
# to stderr and so to the console (backboardd, iconservicesagent and chronod have it already);
# REDWOOD_SYNC_DRAWS=1 makes the driver wait after every draw and name the one the GPU did not
# finish (redwoodmetal_diag.inc); FD_MESA_DEBUG is freedreno's own debug switch.
JOB_ENV = []
for item in os.environ.get("JOB_ENV", "").split():
    job_name, _, setting = item.partition(":")
    key, _, value = setting.partition("=")
    if not job_name.endswith(".plist") or not key:
        raise SystemExit(f"JOB_ENV item {item!r} is not NAME.plist:KEY=VALUE")
    JOB_ENV.append((job_name, key, value.replace("~", " ")))
# EXT_ENV="MercuryPosterExtension:KEY=VALUE ..." does the same for a process with no launchd job: the
# items for one name go into /redwood/etc/env/NAME, one KEY=VALUE a line, and the inserted library
# sets them and starts the process again once, because libSystem reads OS_ACTIVITY_DT_MODE before any
# inserted library runs (userland/src/inject_posterboard.c, which reads the file for
# MercuryPosterExtension only).
EXT_ENV = {}
for item in os.environ.get("EXT_ENV", "").split():
    ext_name, _, setting = item.partition(":")
    if not ext_name or "=" not in setting or setting.startswith("="):
        raise SystemExit(f"EXT_ENV item {item!r} is not NAME:KEY=VALUE")
    EXT_ENV.setdefault(ext_name, []).append(setting)

# The wallpaper and the widgets are drawn by two services this boot does not start, and each
# lookup for them fails: SpringBoard and iconservicesagent ask for
# com.apple.posterboardservices.services, which is the wallpaper, and the home screen's widget
# stays on "Waiting…" because nothing hosts a widget extension. The plists are the real ones:
# chronod's from the 2026-10-25 QEMU image, PosterBoard's from
# System/Library/LaunchAngels/com.apple.PosterBoard.plist with one change -- the real one marks
# its service _ExperimentalNonLaunching so that RunningBoard, not launchd, starts the angel, and
# nothing here asks RunningBoard for an angel scene.
EXTRA_JOBS = ["com.apple.chronod.plist", "com.apple.PosterBoard.plist",
              "com.apple.replicatord.plist", "com.apple.mobile.installd.plist",
              "com.apple.biomed.plist", "com.apple.ReportCrash.plist", "com.apple.linkd.plist",
              "com.apple.contextstored.plist", "com.apple.dasd.plist",
              "com.apple.appprotectiond.plist"]
# biomed is the only one here Apple ships without RunAtLoad, and that is how it has to stay: it
# serves com.apple.biome.access.user, and launchd starts it when chronod first opens a Biome stream.
# Forcing RunAtLoad on it would start a second copy of Apple's scheduling while the desktop is still
# coming up. EnablePressuredExit is Apple's own and is left as it is: launchd reclaims it when idle
# and starts it again on the next lookup, which is what a client that keeps asking wants anyway.
# com.apple.biomesyncd is deliberately not here: it syncs Biome to paired devices over Rapport and
# Cascade and serves none of the names chronod looks up, so it would answer nothing on this boot.
# ReportCrash holds a Mach service whose entry carries an ExceptionServer subtype: launchd keeps the
# receive right for the name and starts the job when something is delivered to it, so the same
# treatment as biomed (Apple's keys untouched, no RunAtLoad) is also the correct one here. Without
# the job registered, a corpse notification has no receiver and the host that dies leaves nothing on
# the memory disk to read.
# linkd is the largest remaining one: all 1376 raw Code=4099 lines on the biomed round come from
# chronod, and 88 percent of them are com.apple.linkd.registry. Apple's plist has no RunAtLoad and
# lists ten Mach services, so it starts when one of them is looked up. Its LaunchEvents watch
# LaunchServices application registration, which is worth knowing at boot: this boot registers about
# 259 applications in the first seconds, and each notification can wake linkd. That is Apple's own
# shape and is left alone; a pid that changes repeatedly in the run is how it would show.
ON_DEMAND_JOBS = {"com.apple.biomed.plist", "com.apple.ReportCrash.plist", "com.apple.linkd.plist",
                  "com.apple.contextstored.plist", "com.apple.dasd.plist",
                  "com.apple.appprotectiond.plist"}
# contextstored is here for one service: com.apple.coreduetd.context. 9 lines of
# NSCocoaErrorDomain Code=4099 on kernel-12377-attr26-continuous are that name failing to look
# up (error 3, no such process) on the key paths /device/dasd/widgetBudget, widgetSystemAdded and
# widgetOverride; the provider is contextstored, not dasd, which is why registering this one name
# is what answers them. On demand, as Apple ships it: a lookup starts the job, and the point is
# that the lookup has a receiver, not that the daemon runs at boot.
# dasd is here for com.apple.duetactivityscheduler, which widget refreshes ask for the budgets of:
# 74 lines on kernel-12377-attr26-persona3 look that name up and find nothing. On demand too, and
# it matters more here than for contextstored: duetactivityscheduler takes over background
# scheduling once it runs, so forcing RunAtLoad would change when the system runs background work
# and make the performance and temperature readings of this round incomparable with earlier ones.
# appprotectiond is here for com.apple.appprotectiond.read, the largest single name in the 4099
# count (163 on persona3, 172 on continuous): one job provides all eight appprotectiond services,
# so registering it is what gives that lookup a receiver. It watches extensions and view subjects
# through extensionmonitor and viewsubjectmonitor, which is the same ground the widget hosts die
# on, so a round with it needs the death-cause lines from 8776c5d to see a side effect at all.
# The home screen showed two icons of the sixty-four bundles that carry no SBAppTags hidden, and
# lsd[29] asked eight times for com.apple.mobile.installd without an answer: LaunchServices takes
# its application list from installd, and pluginkit takes its extension records from
# LaunchServices, so the two extension identities chronod asks for cannot resolve either. Both
# daemons are in the image and neither is in the job cache; installd's own plist runs it as
# _installd, a user the memory filesystem rebuilt at boot does not have, so it runs as root here.
JOB_USERS = {"com.apple.mobile.installd.plist": "root"}
# chronod is started late on purpose. Every run so far asked for widget and control extension
# identities eight and thirty-nine seconds after boot, before pluginkit and LaunchServices had
# registered the extensions of the 259 applications, and gave up for a day. StartInterval with
# RunAtLoad off puts its first start at about 120 s, after that registration.
LATE_JOBS = {"com.apple.chronod.plist": {"StartInterval": 120}}
JOBS_DIR = HERE / "jobs"
# REST=1 registers the rest of iOS 26's jobs, each started only when something looks up one of its
# Mach services: userland/mkjobs.py --rest, with its DENY list, gives every LaunchDaemon and
# LaunchAngel that has a Mach service and a program on the system image, with RunAtLoad, KeepAlive
# and the other start triggers taken out. A job whose label or one of whose services is registered
# already is left out. Without them a client of a missing service gets an invalid connection at once,
# and some clients reconnect at once, without a pause: on kernel-12377-cmd24-xpc1 SpringBoard's main
# thread rebuilt its Sleep.framework connection to com.apple.sleepd.sleepserver, and MapKit's
# snapshot service its GeoServices connection, for as long as the round lasted. The iOS 16.5 desktop
# ran on the same kind of set (388 jobs, docs/analysis/2026-10-28-ios26-icon-model-and-launchd-cache.md §2).
# On by default, REST=0 turns it off: kernel-12377-cmd32-bo2 was packed without it by mistake and
# spent 57% of four processors on those reconnects (launchd, MapKit's snapshot service,
# appprotectiond), ran its cores at 3 GHz, reached 77 C and dropped the GPU to 160 MHz.
REST = os.environ.get("REST", "1") == "1"
REST_DIR = OUT / "rest-jobs"
# mkjobs.py's DENY was written on iOS 16.5; for this boot it changes in two places.
# - com.apple.dmd is let through. DeviceManagement's clients reconnect to it without a pause when
#   it is not registered: on kernel-12377-cmd24-rest1 appprotectiond and ScreenTimeAgent each spent
#   about a fifth of a processor in Catalyst's connection code, both with dmd denied.
# - PerfPowerServices is left out. On the same round it spent half a processor inside a
#   dispatch_once that walks the SMC keys (libSMC SMCGetKeyFromIndex, PowerlogLiteOperators), which
#   this kernel's SMC does not end.
# REST_ALLOW="word ..." lets more of DENY through for one round. duetexpertd is the candidate for the
# wallpaper: it serves com.apple.proactive.FaceSuggestions.xpc, and on kernel-12377-cmd40-bo16
# PosterBoard's gallery update got 4099 (connection invalid) from it and every extension's
# descriptor push to it failed with -2222.
# AUDIO=1 builds the RedwoodAudio driver plugin (userland/src/services/audio_plugin.c) and
# installs it at /lib/RedwoodAudio.driver/RedwoodAudio. Default off: sound is not part of
# the 26 line yet -- userland/build.sh stages RedwoodAudio only when V != 26.
AUDIO = os.environ.get("AUDIO", "0") == "1"
AUDIO_SOURCES = ["userland/src/services/audio_plugin.c"]
AUDIO_FLAGS = ["-Os", "-Wall", "-Wextra", "-bundle", "-framework", "CoreFoundation"]
AUDIO_PLUGIN = "/lib/RedwoodAudio.driver/RedwoodAudio"
# BLUETOOTH=1 (on by default when BT_FW carries the WCN7850 firmware, or when REST_ALLOW names bluetooth)
# builds userland/src/services/bluetool.c into /bin/bluetool, points com.apple.BlueTool at it, lets the bluetooth
# jobs through mkjobs.py's DENY, and sets KeepAlive and BT_HCI_TRANSPORT/BT_CHIP on com.apple.bluetoothd.
BLUETOOTH = (os.environ.get("BLUETOOTH", "1" if BT_FW else "0") == "1"
             or "bluetooth" in os.environ.get("REST_ALLOW", "").split())
BLUETOOL_SOURCES = ["userland/src/services/bluetool.c"]
BLUETOOL_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror", "-fblocks"]
REST_TRIAL = (os.environ.get("REST_ALLOW", "").split()
              + (["audio"] if AUDIO else [])
              + (["bluetooth"] if BLUETOOTH else []))
REST_ALLOW = ["com.apple.dmd"] + REST_TRIAL
REST_DENY = ["com.apple.PerfPowerServices", "com.apple.syncdefaultsd"]
SDK = ["/usr/bin/xcrun", "-sdk", "iphoneos", "clang", "-arch", "arm64e", "-miphoneos-version-min=26.0",
       # The fork's own headers are flat in userland/src/; the overlay's are nested. Flat first,
       # so a fork file wins over the overlay's copy of the same name.
       "-Iuserland/src", "-Iuserland/src/boot", "-Iuserland/src/inject", "-Iuserland/src/gfx",
       "-Iuserland/src/services", "-Iuserland/src/apps", "-Iuserland/src/tests"]

# SHIM_FLAGS are build.sh's flags for the same library.
SHIM_SOURCES = ["userland/src/inject/sb_metal_experiment.m"]
SHIM_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror", "-framework", "Foundation", "-framework", "Metal",
              "-framework", "UIKit", "-framework", "IOKit"]
# The iOS 26 driver: tools/userland-driver/redwoodmetal.m + redwooddrawable.m, with the
# .inc files and headers in the same directory. DRIVER_INCLUDES is what the manifest
# hashes (the compiler reads the .inc files through the .m files, not as inputs).
DRIVER_SOURCES = ["tools/userland-driver/redwoodmetal.m", "tools/userland-driver/redwooddrawable.m"]
DRIVER_INCLUDES = ["tools/userland-driver/redwood_drawable.h",
                   "tools/userland-driver/air_metal_copy.h",
                   "tools/userland-driver/redwoodmetal_archive.inc",
                   "tools/userland-driver/redwoodmetal_blit.inc",
                   "tools/userland-driver/redwoodmetal_commandbuffer.inc",
                   "tools/userland-driver/redwoodmetal_completion.inc",
                   "tools/userland-driver/redwoodmetal_console.inc",
                   "tools/userland-driver/redwoodmetal_diag.inc",
                   "tools/userland-driver/redwoodmetal_heap.inc",
                   "tools/userland-driver/redwoodmetal_mesa.inc",
                   "tools/userland-driver/redwoodmetal_pipecache.inc",
                   "tools/userland-driver/redwoodmetal_state.inc",
                   "tools/userland-driver/redwoodmetal_stitch.inc",
                   "tools/userland-driver/redwoodmetal_texture.inc",
                   "tools/userland-driver/redwoodmetal_texture_layout.inc",
                   "tools/userland-driver/redwoodmetal_work.inc",
                   "tools/userland-driver/redwoodmetal_work_private.h"]
DRIVER_FLAGS = ["-O1", "-framework", "Foundation", "-framework", "IOKit", "-framework", "Metal"]
BUNDLE_FLAGS = ["-bundle", *DRIVER_FLAGS]
# SpringBoard's 23G90 adaptations: the local activation state, the software icon selection, the
# lock screen dismissal after boot (com.redwood.unlock dismisses it again) and the frame rate readback
# with its boot-arg switches (inject_sb26_rate.c: dl_max, dl_log, ax_rt, ax_rm). The pack builds it
# from the current source and injects it; without it the iOS 26 lock screen stays up. The same
# library is injected into PosterBoard, where only inject_posterboard.c's constructor acts: it logs
# the errors behind the data store PosterBoard cannot create, which is why the wallpaper is empty.
SB26_SOURCES = ["userland/src/inject/inject_sb26.c", "userland/src/inject/inject_sb26_layers.c",
                "userland/src/inject/inject_sb26_rate.c", "userland/src/inject/inject_posterboard.c"]
SB26_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror", "-framework", "CoreFoundation", "-framework", "Metal",
              "-lMobileGestalt"]
USERLAND_SOURCES = ["userland/src/inject/inject.c"]
USERLAND_FLAGS = ["-O1", "-framework", "CoreFoundation", "-framework", "IOKit"]
MEDIA_SOURCES = ["userland/src/inject/inject_media.c", "userland/src/services/camera_device.c",
                 "userland/src/services/camera_isp.c"]
MEDIA_FLAGS = ["-O2", "-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo",
               "-framework", "IOSurface"]
VT_SOURCES = ["userland/src/inject/videotoolbox_iris.c"]
VT_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror", "-Wno-four-char-constants", "-framework", "CoreFoundation",
            "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework", "VideoToolbox", "-framework", "IOSurface"]
XPCWATCH_SOURCES = ["userland/src/inject/xpcwatch.c"]
XPCWATCH_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror"]
AX_SOURCES = ["userland/src/inject/ax.m", "userland/src/inject/statusbar26.m"]
AX_FLAGS = ["-Os", "-Wall", "-Wextra", "-Werror", "-framework", "UIKit", "-framework", "Foundation",
            "-framework", "CoreGraphics"]
VDEC_TEST_SOURCES = ["userland/src/tests/video_decode_test.m"]
VDEC_TEST_FLAGS = ["-O1", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-framework", "Foundation",
                   "-framework", "AVFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo",
                   "-framework", "VideoToolbox"]
VPLAY_TEST_SOURCES = ["userland/src/tests/video_player_test.m"]
VPLAY_TEST_FLAGS = ["-O1", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-framework", "Foundation",
                    "-framework", "AVFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo"]
VCOMB_TEST_SOURCES = ["userland/src/tests/video_combined_test.m"]
VCOMB_TEST_FLAGS = ["-O1", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                    "-framework", "Foundation", "-framework", "Metal", "-framework", "CoreVideo",
                    "-framework", "IOSurface"]
# The reflection test's source (tools/userland-driver/metal_reflection_test.m) is in
# neither this fork nor the userland checkout: it lives in the kernel tree's
# tools/userland-driver/. The build below is conditional on the source
# resolving; in this fork it never does, so the test is skipped with a note.
REFLECTION_TEST_SOURCES = ["tools/userland-driver/metal_reflection_test.m"]
REFLECTION_TEST_FLAGS = ["-O1", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                         "-framework", "Foundation", "-framework", "Metal"]
APP_SOURCES = ["userland/src/inject/inject_app.c"]
APP_FLAGS = ["-O1", "-fblocks"]

# make-payload.py defaults this to 3 while nearly every comparable round used 1, so a forgotten
# GPU_SPEED changes the GPU clock without changing the request. It is recorded here as well as in the
# round manifest, and a note that names a different speed stops the pack.
GPU_SPEED = int(os.environ.get("GPU_SPEED", "3"))
# REDWOOD_DIAG=1 puts a diagnostic variable in the driver processes' environment. Diagnostic prints
# (the driver's load address today, anything else added for one investigation later) are behind it,
# so enabling one does not change the library. The manifest records whether this round carried them.
REDWOOD_DIAG = os.environ.get("REDWOOD_DIAG") == "1"
# JOBS_DROP="a.plist b.plist" packs without those jobs. A round changes one thing, and EXTRA_JOBS
# accumulates what earlier rounds proved: without a way to leave one out, adding the next job also
# changes the previous one's result. The names dropped are recorded in the manifest, so a round says
# what it did not carry as well as what it did.
JOBS_DROP = {n.strip() for n in os.environ.get("JOBS_DROP", "").split() if n.strip()}
missing_drop = JOBS_DROP - set(EXTRA_JOBS)
assert not missing_drop, f"JOBS_DROP names jobs that are not in EXTRA_JOBS: {sorted(missing_drop)}"
# The calls the driver refuses a compiler library without: mesa_available()
# (tools/userland-driver/redwoodmetal_mesa.inc) closes the library and
# returns false when any of these five is NULL. Every other rwa_* the driver
# looks up is optional -- a library without it is a real state of this work,
# and the driver says so when asked instead of refusing to load. The subset
# check below reads the names out of the driver's own sources, so a source
# change that adds a dlsym name fails there until it is classified here.
RWA_REQUIRED = {"rwa_device_open", "rwa_compute_create", "rwa_dispatch",
                "rwa_texture_create", "rwa_sampler_create"}

sys.path.insert(0, str(TOOLS))
from apfs import Container, pick_volume, walk, ROOT_DIR_INO_NUM   # noqa: E402


def dirty_sources():
    """What is uncommitted in the trees a pack is built from.

    The result goes into the pack record so a round states what it was built from. It does not stop
    the pack. One repository now (this fork), plus the userland overlay's commit when the host
    has it laid over the tree (common/setup.sh keeps it in .userland.git)."""
    reports = []
    try:
        result = subprocess.run(["git", "status", "--short"], cwd=WORKSPACE,
                                capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            reports.append(f"fork: could not be read ({result.stderr.strip() or result.returncode})")
        elif result.stdout.strip():
            reports.append(f"fork:\n{result.stdout.rstrip()}")
    except (OSError, subprocess.SubprocessError) as error:
        reports.append(f"fork: could not be read ({error})")
    overlay_git = WORKSPACE / ".userland.git"
    if overlay_git.is_dir():
        try:
            result = subprocess.run(
                ["git", "--git-dir", str(overlay_git), "log", "-1", "--format=%H %s"],
                capture_output=True, text=True, timeout=60)
            if result.returncode == 0 and result.stdout.strip():
                reports.append(f"userland overlay: {result.stdout.strip()}")
        except (OSError, subprocess.SubprocessError):
            pass
    return reports


def sha(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


# Every library in this image is loaded by an arm64e process, and dyld refuses an arm64 slice in one
# by discarding it without a line: the processes then run with none of the library's work done, which
# reads as the library not being injected at all. 0x80000002 is ARM64E with the LIB64 bit, which is
# what build.sh's own gate (:885-922) requires of everything it stages.
ARM64E_WITH_LIB64 = 0x80000002


def cpusubtype(path):
    """The cpusubtype of a thin Mach-O, read from its header."""
    with Path(path).open("rb") as source:
        header = source.read(12)
    magic, _cputype, cpusubtype = struct.unpack("<Iii", header)
    assert magic in (0xFEEDFACF, 0xFEEDFACE), f"{path} is not a thin Mach-O (magic {magic:#x})"
    return cpusubtype & 0xFFFFFFFF


def records(path):
    """Every path in a root image, with the metadata mkapfs.py would have to reproduce."""
    container = Container(str(path))
    volume = pick_volume(container, None)
    meta, bodies = {}, {}
    for name, _, fid, kind in [("/", "", ROOT_DIR_INO_NUM, 4), *walk(volume, ROOT_DIR_INO_NUM)]:
        inode = volume.inode(fid)
        data = inode.contents() if kind == 8 else inode.symlink_target_bytes() if kind == 10 else b""
        bodies[name] = data
        meta[name] = {"kind": kind, "sha256": hashlib.sha256(data).hexdigest(),
                      **{k: getattr(inode, k) for k in
                         ["mode", "owner", "group", "create_time", "mod_time", "change_time",
                          "access_time", "protection_class", "bsd_flags", "internal_flags", "nlink"]},
                      "xattrs": {n: hashlib.sha256(volume.xattr_bytes(fid, n, inode.xattrs)).hexdigest()
                                 for n in inode.xattrs}}
    container.close()
    return meta, bodies


def build(sources, install_name, flags, out, dynamic=True):
    """One arm64e library (or one program, dynamic=False) from the current source, signed as build.sh signs one.

    sources are logical paths; each is resolved (fork-flat first, overlay second) here, so a
    missing source fails with the file's name instead of a compiler error about a path."""
    resolved = [resolve_or_fail(s, "a build input") for s in sources]
    kind = ["-dynamiclib", "-install_name", install_name] if dynamic else []
    arc = [] if "-fobjc-arc" in flags else ["-fno-objc-arc"]
    command = SDK + flags + [*arc, *kind, *[str(p) for p in resolved], "-lobjc", "-o", str(out)]
    with out.with_suffix(".build.log").open("w") as log:
        log.write(" ".join(command) + "\n")
        subprocess.run(command, stdout=log, stderr=log, check=True, cwd=SRC_ROOT)
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(out)], check=True)
    return command


def add_switch(root):
    """REDWOOD_SB_METAL=1 for backboardd (and iconservicesagent with ICON_METAL=1), in the job
    plists and in the launchd cache. Returns backboardd's job."""
    cache_path = root / "xpc/launchd.plist"
    cache = plistlib.loads(cache_path.read_bytes())
    jobs = []
    targets = [(JOB, CACHE_KEY)] + ([(ICON_JOB, ICON_CACHE_KEY)] if ICON_METAL else [])
    for relative, key in targets:
        job_path = root / relative
        job = plistlib.loads(job_path.read_bytes())
        job.setdefault("EnvironmentVariables", {})["REDWOOD_SB_METAL"] = "1"
        if relative == JOB:
            if DIRTY_REGIONS:
                job["EnvironmentVariables"].pop("CA_DISABLE_DIRTY_REGIONS", None)
            else:
                job["EnvironmentVariables"]["CA_DISABLE_DIRTY_REGIONS"] = "1"
        blob = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        job_path.write_bytes(blob)
        assert plistlib.loads(blob) == job, "the job plist did not survive the round trip"
        cache["LaunchDaemons"][key] = job
        jobs.append(job)
    cache_path.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return jobs[0]


def add_diag(root, cache):
    """REDWOOD_DIAG=1 for the two processes that load the driver, when REDWOOD_DIAG=1 asks.

    Diagnostic prints sit behind this variable so a round can turn them on without a different
    library, and the manifest records whether the round carried them. Without the variable the driver
    prints none of them."""
    for name in MSAA_JOBS:
        job_path = root / "LaunchDaemons" / name
        job = plistlib.loads(job_path.read_bytes())

        job.setdefault("EnvironmentVariables", {})["REDWOOD_DIAG"] = "1"
        blob = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        job_path.write_bytes(blob)
        cache["LaunchDaemons"]["/System/Library/LaunchDaemons/" + name] = job
    (root / "xpc/launchd.plist").write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return cache


def cache_key(name):
    """The launchd cache key of a job plist from the system image. launchd treats a job as a launch
    angel only when its path is under /System/Library/LaunchAngels/, and RunningBoard refuses to
    start an angel that launchd reports as anything else ("Attempted to launch angel found a
    non-angel job"), so an angel keeps its own directory in the key."""
    rootfs = IOS_DIR / "rootfs/System/Library"
    if (rootfs / "LaunchAngels" / name).exists() and not (rootfs / "LaunchDaemons" / name).exists():
        return "/System/Library/LaunchAngels/" + name
    return "/System/Library/LaunchDaemons/" + name


def add_env(root, cache, settings):
    """Each (job, key, value) in the job's environment, in its plist and in the launchd cache."""
    for name, key, value in settings:
        job_path = root / "LaunchDaemons" / name
        job = plistlib.loads(job_path.read_bytes())
        job.setdefault("EnvironmentVariables", {})[key] = value
        job_path.write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
        cache["LaunchDaemons"][cache_key(name)] = job
    (root / "xpc/launchd.plist").write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return cache


def add_msaa(root):
    """REDWOOD_MSAA=1 for the two processes that encode render passes, when MSAA=1 asks."""
    cache_path = root / "xpc/launchd.plist"
    cache = plistlib.loads(cache_path.read_bytes())

    for name in MSAA_JOBS:
        job_path = root / "LaunchDaemons" / name
        job = plistlib.loads(job_path.read_bytes())

        job.setdefault("EnvironmentVariables", {})["REDWOOD_MSAA"] = "1"
        blob = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        job_path.write_bytes(blob)
        cache["LaunchDaemons"][cache_key(name)] = job
    cache_path.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))


def add_rest_jobs(root, cache):
    """REST=1: the jobs userland/mkjobs.py --rest makes that are not registered already."""
    if REST_DIR.exists():
        shutil.rmtree(REST_DIR)
    mkjobs = resolve_or_fail("userland/mkjobs.py", "the REST job generator")
    subprocess.run([sys.executable, str(mkjobs), "--rest",
                    str(IOS_DIR / "rootfs"), str(REST_DIR)], check=True, stdout=subprocess.DEVNULL,
                   env=dict(os.environ, ALLOW=" ".join(REST_ALLOW)))
    jobs = cache["LaunchDaemons"]
    labels = {job["Label"] for job in jobs.values()}
    services = {name for job in jobs.values() for name in job.get("MachServices", {})}
    added = []
    for path in sorted(REST_DIR.glob("*.plist")):
        data = path.read_bytes()
        job = plistlib.loads(data)
        shared = services & set(job["MachServices"])
        if shared and any(word in path.name for word in REST_TRIAL):
            # The system image's SpringBoard and duetexpertd jobs both list
            # com.apple.proactive.HomeScreenSuggestion.xpc, and a registered name keeps the whole
            # job out; a job on trial gives up the names another job holds and keeps the rest.
            for name in shared:
                del job["MachServices"][name]
            data = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
            shared = set()
        if (job["Label"] in labels or job["Label"] in REST_DENY or shared
                or (root / "LaunchDaemons" / path.name).exists()):
            continue
        if BLUETOOTH and path.name == "com.apple.BlueTool.plist":
            job["ProgramArguments"] = ["/redwood/bin/bluetool"]
            job.pop("Program", None)
            job["UserName"] = "root"
            data = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        elif BLUETOOTH and path.name == "com.apple.bluetoothd.plist":
            job["ProgramArguments"] = ["/redwood/bin/bluetoothd"]
            job.pop("Program", None)
            job["KeepAlive"] = True
            env = job.setdefault("EnvironmentVariables", {})
            env.setdefault("BT_HCI_TRANSPORT", "H4:POSIX")
            env.setdefault("BT_CHIP", "ACI BT")
            env.setdefault("OS_ACTIVITY_DT_MODE", "YES")
            data = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        (root / "LaunchDaemons" / path.name).write_bytes(data)
        jobs[cache_key(path.name)] = job
        labels.add(job["Label"])
        services |= set(job["MachServices"])
        added.append(path.name)
    (root / "xpc/launchd.plist").write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return cache, added


def add_extra_jobs(root):
    """The wallpaper and the widget services, in the LaunchDaemons directory and the cache."""
    cache_path = root / "xpc/launchd.plist"
    cache = plistlib.loads(cache_path.read_bytes())
    for name in EXTRA_JOBS:
        if name in JOBS_DROP:
            continue
        job = plistlib.loads((JOBS_DIR / name).read_bytes())
        # RunAtLoad for all: nothing else here asks for them early. KeepAlive off, so a service
        # that cannot run in this boot is seen dead once in the log rather than restarted for the
        # length of the run. The jobs in LATE_JOBS are the exception: their first start is put off
        # by StartInterval instead, so it lands after pluginkit has registered the extensions.
        if name not in ON_DEMAND_JOBS:
            job["RunAtLoad"] = True
            job["KeepAlive"] = False
        user = JOB_USERS.get(name)
        if user is not None:
            job["UserName"] = user
            job.pop("GroupName", None)
        late = LATE_JOBS.get(name)
        if late is not None:
            job["RunAtLoad"] = False
            job.update(late)
        blob = plistlib.dumps(job, fmt=plistlib.FMT_BINARY)
        (root / "LaunchDaemons" / name).write_bytes(blob)
        cache["LaunchDaemons"]["/System/Library/LaunchDaemons/" + name] = job
    cache_path.write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    return cache


def install_name(path):
    """The LC_ID_DYLIB install name of a Mach-O, read in pure Python (no otool needed)."""
    with Path(path).open("rb") as source:
        header = source.read(32)
    magic, _cputype, _cpusubtype, _filetype, ncmds, _sizeofcmds, _flags, _reserved = struct.unpack("<IiiIIIII", header)
    assert magic == 0xFEEDFACF, f"{path} is not a 64-bit Mach-O (magic {magic:#x})"
    offset = 32
    with Path(path).open("rb") as source:
        for _ in range(ncmds):
            source.seek(offset)
            cmd, cmdsize = struct.unpack("<II", source.read(8))
            if cmd == 0xD:  # LC_ID_DYLIB
                source.seek(offset + 8)
                name_offset = struct.unpack("<I", source.read(4))[0]
                source.seek(offset + name_offset)
                name = b""
                while True:
                    byte = source.read(1)
                    if byte in (b"\x00", b""):
                        break
                    name += byte
                return name.decode()
            offset += cmdsize
    return None


def exported_symbols(path):
    """The exported symbols of a dylib via nm, or None when nm cannot read it (Linux has no
    Mach-O nm). The driver itself does the authoritative check at load; this is a safety net.

    Mach-O symbols carry a leading underscore (_rwa_device_open); dlsym() names do not
    (rwa_device_open), so one underscore is stripped here -- without that, every
    required symbol would read as missing."""
    nm = shutil.which("nm")
    if nm is None:
        return None
    result = subprocess.run([nm, "-gU", str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    symbols = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        if name.startswith("_"):
            name = name[1:]
        symbols.add(name)
    return symbols


def driver_dlsym_names():
    """Every rwa_* name the driver looks up with dlsym, read out of its own sources."""
    names = set()
    mesa_inc = resolve("tools/userland-driver/redwoodmetal_mesa.inc")
    if mesa_inc is not None:
        text = mesa_inc.read_text()
        names |= set(re.findall(r'dlsym\(mesa\.handle,\s*"([^"]+)"\)', text))
    return names


def source_hashes(names):
    """{logical name: sha256 or None} and {logical name: origin} for a list of sources.

    A name that does not resolve records None instead of failing here: the manifest's
    required-input check at the end is what fails the pack, and it names exactly which
    core input is missing."""
    hashes, origins = {}, {}
    for name in names:
        path = resolve(name)
        if path is None:
            hashes[name] = None
            origins[name] = None
            continue
        hashes[name] = sha(path)
        origins[name] = resolve_origin(name)
    return hashes, origins


def check_air_lib(air_path, record):
    """The AIR library against its build record. Returns a dict of what was checked.

    Fails on: hash mismatch with the record, wrong cpusubtype, a missing AIR_REQUIRED
    string, a record holds{} entry marked absent for a required string. Warns (does not
    fail) on: the mesa-side air_metal_copy.h hash diverging -- the driver-side copy is a
    different file by design, and the two are recorded separately in the manifest."""
    checks = {}
    if "library_sha256" not in record:
        raise SystemExit("the AIR build record has no library_sha256; run tools/k90auto/build-air.py")
    got = sha(air_path)
    checks["library_sha256"] = got
    assert got == record["library_sha256"], (
        f"the AIR library here is {got[:16]}, but the last build-air.py run made "
        f"{record['library_sha256'][:16]}; run tools/k90auto/build-air.py")
    subtype = cpusubtype(air_path)
    checks["cpusubtype"] = f"{subtype:#x}"
    assert subtype == ARM64E_WITH_LIB64, (
        f"the AIR library is cpusubtype {subtype:#x}, and this image's processes are "
        "arm64e: rebuild it with -arch arm64e, as build-air.py does")
    codesign = shutil.which("codesign")
    if codesign is not None:
        signed = subprocess.run([codesign, "-dv", str(air_path)], capture_output=True, text=True)
        checks["adhoc_signed"] = "Signature=adhoc" in signed.stderr
        assert checks["adhoc_signed"], (
            f"{air_path} is not signed adhoc, and dyld will refuse it. The file to pack is "
            "out/ios26-resume/bbmetal-device/libredwoodair.dylib, which build-air.py signs "
            "with codesign --force --sign -; the copy ninja leaves under "
            "out/ios26-resume/mesa-arm64e/build/src/gallium/drivers/freedreno/ is unsigned "
            "by construction, so it can never pass this check. Run tools/k90auto/build-air.py")
    else:
        checks["adhoc_signed"] = None
        print("note: codesign not found, skipping the adhoc-signature check")
    text = Path(air_path).read_bytes()
    held = [t for t in AIR_REQUIRED if t.encode() in text]
    missing = [t for t in AIR_REQUIRED if t not in held]
    checks["air_required_held"] = held
    assert not missing, f"the AIR library does not hold {missing}; check build-air.py --check"
    for name, present in record.get("holds", {}).items():
        assert present or name not in AIR_REQUIRED, f"build-air.py recorded {name} as absent"
    mesa_copy = resolve("tools/mesa/air/air_metal_copy.h")
    record_mesa = record.get("tree_sha256:air_metal_copy.h")
    if mesa_copy is not None and record_mesa is not None:
        checks["mesa_copy_sha256"] = sha(mesa_copy)
        checks["mesa_copy_matches_record"] = checks["mesa_copy_sha256"] == record_mesa
        if not checks["mesa_copy_matches_record"]:
            print("warning: the mesa-side air_metal_copy.h hash differs from the AIR record; "
                  "the driver-side copy is recorded separately in the manifest")
    elif record_mesa is None:
        print("note: the AIR record carries no tree_sha256:air_metal_copy.h; the mesa-side "
              "copy hash check is skipped")
    else:
        print("note: tools/mesa/air/air_metal_copy.h is not in this tree; the mesa-side copy "
              "hash check is skipped (the record's hash is kept in the manifest)")
    driver_copy = resolve("tools/userland-driver/air_metal_copy.h")
    checks["driver_copy_sha256"] = sha(driver_copy) if driver_copy is not None else None
    dlsym = driver_dlsym_names()
    checks["driver_dlsym_rwa_count"] = len(dlsym)
    stale = RWA_REQUIRED - dlsym
    assert not stale, (f"RWA_REQUIRED names {sorted(stale)}, which the driver no longer "
                       "dlsyms: re-classify the required set before packing")
    exports = exported_symbols(air_path)
    if exports is None:
        print("note: nm cannot read this library here, skipping the rwa_* subset check "
              "(the driver does the authoritative check at load)")
        checks["rwa_required_missing"] = None
        checks["rwa_optional_missing"] = None
    else:
        required_missing = sorted(RWA_REQUIRED - exports)
        optional_missing = sorted((dlsym - RWA_REQUIRED) - exports)
        checks["rwa_required_missing"] = required_missing
        checks["rwa_optional_missing"] = optional_missing
        assert not required_missing, (
            f"the AIR library lacks required rwa_* symbols {required_missing}; "
            "the driver refuses the library without them (mesa_available())")
        if optional_missing:
            print(f"note: the AIR library lacks optional rwa_* symbols {optional_missing}; "
                  "the driver loads without them")
    return checks


def fork_head():
    """The fork's HEAD commit, for the manifest. None when this tree is not a git checkout."""
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=WORKSPACE,
                                capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def driver_only():
    """--driver-only: build the tools/ driver (dylib + bundle) and verify it, without the
    iOS rootfs. This is the CI scope: the full pack needs $IOS/rootfs from the IPSW and
    would put Apple system content into root.apfs, neither of which belongs in CI."""
    print("driver-only mode: building the tools/userland-driver driver, no root.apfs")
    dirty = dirty_sources()
    assert AIR_LIB.is_file(), (f"{AIR_LIB} is missing; fetch it from the air-artifacts "
                               "branch (libredwoodair.dylib + air-build-record.json) or run "
                               "tools/k90auto/build-air.py")
    assert AIR_RECORD.is_file(), ("AIR build record is missing; fetch air-build-record.json "
                                  "from the air-artifacts branch or run tools/k90auto/build-air.py")
    record = json.loads(AIR_RECORD.read_text())
    air_checks = check_air_lib(AIR_LIB, record)

    shim = OUT / "libredwood-sbmetal-experiment.dylib"
    driver = OUT / "libredwoodmetal.dylib"
    driver_bundle = OUT / "RedwoodMetal"
    sb26 = OUT / "libredwood-sb26.dylib"
    commands = {"shim": build(SHIM_SOURCES, "/redwood/lib/libredwood-sbmetal-experiment.dylib",
                              SHIM_FLAGS, shim),
                "driver": build(DRIVER_SOURCES, "/redwood/lib/libredwoodmetal.dylib",
                                DRIVER_FLAGS, driver),
                "driver_bundle": build(DRIVER_SOURCES, "", BUNDLE_FLAGS, driver_bundle, dynamic=False),
                "sb26": build(SB26_SOURCES, "/redwood/lib/libredwood-sb26.dylib", SB26_FLAGS, sb26)}
    for name, product in (("shim", shim), ("driver", driver), ("bundle", driver_bundle), ("sb26", sb26)):
        subtype = cpusubtype(product)
        assert subtype == ARM64E_WITH_LIB64, (
            f"{name} is cpusubtype {subtype:#x}, and this image's processes are arm64e: "
            "rebuild it with -arch arm64e and an iOS 26 SDK, as build.sh does for its "
            "other libraries, then sign it")
    assert install_name(driver) == "/redwood/lib/libredwoodmetal.dylib", (
        f"the driver install name is {install_name(driver)}, not "
        "/redwood/lib/libredwoodmetal.dylib: the driver would not load from the image path")
    assert install_name(shim) == "/redwood/lib/libredwood-sbmetal-experiment.dylib"
    assert install_name(sb26) == "/redwood/lib/libredwood-sb26.dylib"

    package = OUT / "package"
    (package / "lib/RedwoodMetal.bundle").mkdir(parents=True, exist_ok=True)
    shutil.copy2(driver, package / "lib/libredwoodmetal.dylib")
    shutil.copy2(driver_bundle, package / "lib/RedwoodMetal.bundle/RedwoodMetal")
    shutil.copy2(shim, package / "lib/libredwood-sbmetal-experiment.dylib")
    shutil.copy2(sb26, package / "lib/libredwood-sb26.dylib")
    shutil.copy2(AIR_LIB, package / "lib/libredwoodair.dylib")
    shutil.copy2(AIR_RECORD, package / "air-build-record.json")
    for name, product in (("lib/libredwoodmetal.dylib", driver),
                          ("lib/RedwoodMetal.bundle/RedwoodMetal", driver_bundle),
                          ("lib/libredwood-sbmetal-experiment.dylib", shim),
                          ("lib/libredwood-sb26.dylib", sb26),
                          ("lib/libredwoodair.dylib", AIR_LIB)):
        assert sha(package / name) == sha(product), f"the staged {name} is not what this run built"
    # The plugin plist the image mounts as RedwoodMetal.bundle/Info.plist. It is our own
    # file (userland/src/metal_plugin.plist); the bundle directory is the driver's home.
    plugin = resolve_or_fail("userland/src/metal_plugin.plist", "the Metal plugin plist")
    shutil.copy2(plugin, package / "lib/RedwoodMetal.bundle/Info.plist")

    sources, origins = source_hashes([*SHIM_SOURCES, *DRIVER_SOURCES, *DRIVER_INCLUDES,
                                      *SB26_SOURCES, "userland/src/metal_plugin.plist"])
    report = {
        "mode": "driver-only",
        "kernel": {"commit": fork_head()},
        "driver_sha256": sha(driver), "driver_bytes": driver.stat().st_size,
        "driver_bundle_sha256": sha(driver_bundle),
        "shim_sha256": sha(shim), "sb26_sha256": sha(sb26),
        "air_sha256": air_checks["library_sha256"],
        "air_checks": air_checks,
        "air_source_sha256": record.get("source_sha256:air_metal.c"),
        "air_sources": {key: value for key, value in record.items()
                        if str(key).startswith(("source_sha256:", "tree_sha256:"))},
        "sources": sources, "source_origins": origins,
        "build_commands": commands,
        # jobs/ lands in item 4; until then the driver package records that it is absent
        # rather than failing on a file the driver build does not need.
        "jobs_source": ({"path": str(JOBS_SOURCE.relative_to(WORKSPACE))
                         if JOBS_SOURCE.is_relative_to(WORKSPACE) else str(JOBS_SOURCE),
                         "sha256": sha(JOBS_SOURCE),
                         "given": JOBS_SOURCE_GIVEN} if JOBS_SOURCE.is_file() else None),
        "probe": PROBE, "gpu_speed": GPU_SPEED,
        "redwood_diag": REDWOOD_DIAG,
        "dirty_worktree": bool(dirty), "worktree_status": dirty,
        "expected_runtime_log": ["redwoodmetal[SpringBoard", "redwoodmetal[backboardd",
                                 "sb26: boot unlock done after", "MiniIOMobileFramebuffer:",
                                 "RedwoodGPU"],
        "must_not_appear": ["incompatible architecture", "unrecognized selector",
                            "OS_REASON_COREANIMATION", "nowhere for that store to go"],
    }
    required = [*SHIM_SOURCES, *DRIVER_SOURCES, *DRIVER_INCLUDES]
    absent = [name for name in required if not report["sources"].get(name)]
    if absent:
        raise SystemExit("the manifest would not record the build inputs, missing: "
                         + ", ".join(absent))
    (package / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in
                      ["driver_sha256", "driver_bundle_sha256", "shim_sha256", "sb26_sha256",
                       "air_sha256"]}, indent=2))
    print(f"driver package staged at {package}")


def main():
    dirty = dirty_sources()
    if dirty:
        print("工作树有未提交的改动，打包照常进行。改动清单写进 pack 记录的 worktree_status，"
              "这一轮由什么构成以那一份清单为准。")
    notes = os.environ.get("ROUND_NOTES", "")
    claimed = set(re.findall(r"(?:adreno8_speed|gpu_speed|GPU_SPEED|speed)\s*[=:]\s*(\d+)", notes))
    wrong = sorted(value for value in claimed if int(value) != GPU_SPEED)
    if wrong:
        raise SystemExit(
            f"round notes name GPU speed {', '.join(wrong)} but this pack runs adreno8_speed={GPU_SPEED} "
            f"(GPU_SPEED defaults to 3). Set GPU_SPEED to the value the round is meant to run, or fix "
            f"the note; a record that says one thing while the round runs another is what this check is for.")

    assert (BASE / "xpc/launchd.plist").is_file(), (
        f"{BASE} is not staged for iOS 26: run `IOS=../ios26 COMPILE_ONLY=1 bash userland/build.sh` "
        "in this fork's userland first")

    shim = OUT / "libredwood-sbmetal-experiment.dylib"
    driver = OUT / "libredwoodmetal.dylib"
    driver_bundle = OUT / "RedwoodMetal"
    air = OUT / "libredwoodair.dylib"
    sb26 = OUT / "libredwood-sb26.dylib"
    userland = OUT / "libredwood.dylib"
    media = OUT / "libredwood-media.dylib"
    vt = OUT / "libredwood-vt.dylib"
    vdec_test = OUT / "video_decode_test"
    vplay_test = OUT / "video_player_test"
    vcomb_test = OUT / "video_combined_test"
    reflection_test = OUT / "metal_reflection_test"
    app = OUT / "libredwood-app.dylib"
    assert air.is_file(), (f"{air} is missing; tools/k90auto/build-air.py writes it and signs it, "
                           "and ninja alone does not")
    assert AIR_RECORD.is_file(), "AIR build record is missing; run tools/k90auto/build-air.py"
    record = json.loads(AIR_RECORD.read_text())
    air_checks = check_air_lib(air, record)
    air_source = record.get("source_sha256:air_metal.c")
    # Record every copied source and build-tree digest for the library used by this image.
    air_sources = {key: value for key, value in record.items()
                   if str(key).startswith(("source_sha256:", "tree_sha256:"))}
    commands = {"shim": build(SHIM_SOURCES, "/redwood/lib/libredwood-sbmetal-experiment.dylib",
                              SHIM_FLAGS, shim),
                "driver": build(DRIVER_SOURCES, "/redwood/lib/libredwoodmetal.dylib",
                                DRIVER_FLAGS, driver),
                "driver_bundle": build(DRIVER_SOURCES, "", BUNDLE_FLAGS, driver_bundle, dynamic=False),
                "sb26": build(SB26_SOURCES, "/redwood/lib/libredwood-sb26.dylib", SB26_FLAGS, sb26),
                "userland": build(USERLAND_SOURCES, "/redwood/lib/libredwood.dylib", USERLAND_FLAGS, userland),
                "media": build(MEDIA_SOURCES, "/redwood/lib/libredwood-media.dylib", MEDIA_FLAGS, media),
                "vt": build(VT_SOURCES, "/redwood/lib/libredwood-vt.dylib", VT_FLAGS, vt),
                "video_decode_test": build(VDEC_TEST_SOURCES, "", VDEC_TEST_FLAGS, vdec_test, dynamic=False),
                "video_player_test": build(VPLAY_TEST_SOURCES, "", VPLAY_TEST_FLAGS, vplay_test, dynamic=False),
                "video_combined_test": build(VCOMB_TEST_SOURCES, "", VCOMB_TEST_FLAGS, vcomb_test, dynamic=False),
                "app": build(APP_SOURCES, "/redwood/lib/libredwood-app.dylib", APP_FLAGS, app)}
    reflection_built = False
    if resolve("tools/userland-driver/metal_reflection_test.m") is not None:
        commands["metal_reflection_test"] = build(
            REFLECTION_TEST_SOURCES, "", REFLECTION_TEST_FLAGS, reflection_test, dynamic=False)
        reflection_built = True
    else:
        print("note: tools/userland-driver/metal_reflection_test.m is not in this tree; "
              "the reflection test is skipped (it lives in the kernel tree's tools/userland-driver/)")
    xpcwatch = OUT / "libredwood-xpcwatch.dylib"
    if XPCWATCH_PROCESSES:
        commands["xpcwatch"] = build(XPCWATCH_SOURCES, "/redwood/lib/libredwood-xpcwatch.dylib",
                                     XPCWATCH_FLAGS, xpcwatch)
    audio = OUT / "RedwoodAudio"
    if AUDIO:
        commands["audio"] = build(AUDIO_SOURCES, "", AUDIO_FLAGS, audio, dynamic=False)
    bluetool = OUT / "bluetool"
    if BLUETOOTH:
        commands["bluetool"] = build(BLUETOOL_SOURCES, "", BLUETOOL_FLAGS, bluetool, dynamic=False)
    ax = OUT / "libredwood-ax.dylib"
    if AX_PROCESSES:
        commands["ax"] = build(AX_SOURCES, "/redwood/lib/libredwood-ax.dylib", AX_FLAGS, ax)
    probe = OUT / "lsprobe"
    if PROBE:
        commands["probe"] = build(PROBE_SOURCES, "", PROBE_FLAGS, probe, dynamic=False)
    sources, origins = source_hashes(
        [*SHIM_SOURCES, *DRIVER_SOURCES, *DRIVER_INCLUDES, *SB26_SOURCES,
         *(XPCWATCH_SOURCES if XPCWATCH_PROCESSES else []),
         *(AX_SOURCES if AX_PROCESSES else []),
         *(AUDIO_SOURCES if AUDIO else []),
         *(BLUETOOL_SOURCES if BLUETOOTH else []),
         "userland/src/inject/inject_sb26_airdrop.inc",
         "userland/src/inject/inject_sb26_loc.inc",
         "userland/src/inject/sb_metal_registration_sequence.inc",
         "userland/src/inject/sb_metal_compression_policy.inc",
         "userland/src/inject/inject_media.c",
         "userland/src/services/camera_device.c",
         "userland/src/services/camera_device_native.inc",
         "userland/src/services/camera_device_stream.inc",
         "userland/src/services/camera_isp.c",
         "userland/src/services/input26_service.c",
         "userland/src/inject/inject_app.c",
         "userland/src/inject/inject_app_network.inc",
         "userland/src/inject/inject_cellular.inc",
         "userland/src/tests/torch_test.c",
         "userland/src/tests/sound_test.c",
         "userland/src/tests/record_test.c",
         *VT_SOURCES,
         *VDEC_TEST_SOURCES,
         *VPLAY_TEST_SOURCES,
         *VCOMB_TEST_SOURCES,
         *REFLECTION_TEST_SOURCES,
         "tools/mesa/air/air_metal.c", "tools/mesa/air/air_to_nir.c",
         "tools/mesa/air/air_metal_yuv.h", "tools/mesa/air/air_metal_yuv.inc",
         *(PROBE_SOURCES if PROBE else [])])
    # The probe goes first, not last. init runs a job file one line at a time and waits for each, so a
    # probe line appended after a few minutes of gestures only starts when the gestures are over: on
    # 2026-10-28 the two lsprobe lines sat behind 500 s of drags and never ran, while the same lines at
    # the top of the file run from the first second of the round.
    jobs_source_bytes = JOBS_SOURCE.read_bytes()
    jobs = (PROBE_LINE.encode() if PROBE else b"") + jobs_source_bytes

    # The untouched base as an image, built the way root.apfs is below: what the comparison at the
    # end measures against.
    base_image = OUT / "base.apfs"
    with (OUT / "base-build.log").open("w") as log:
        subprocess.run([sys.executable, str(TOOLS / "mkapfs.py"), "-z", "--plain", "fw",
                        "-o", str(base_image), "redwood=" + str(BASE)],
                       stdout=log, stderr=log, check=True, cwd=WORKSPACE)
    before, _ = records(base_image)
    protected = {str(base_image): sha(base_image)}

    dest = OUT / "root"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(BASE, dest, symlinks=True)
    # The base root is a build product, and a line added to userland/build.sh does not reach a pack
    # until build.sh is run again: on 2026-10-28 the two directories added to the data volume template
    # were still missing from the packed etc/var.skel, and the round that was meant to test them ran
    # without them. var.skel.extra is the one place those lines are written down; build.sh appends it
    # when it builds the skeleton, and this step appends whatever the base root is too old to have.
    skeleton = dest / "etc/var.skel"
    raw_wanted = [line for line in (WORKSPACE / "userland/var.skel.extra").read_text().splitlines()
                  if line and not line.startswith("#")] + (WIFI_CONTROL_LINES if WIFI_FW else [])
    present = skeleton.read_text()
    template_dirs = {line.split("\t")[-1] for line in present.splitlines() if line.startswith("d\t")}
    seen_dirs = set()
    wanted = []
    for line in raw_wanted:
        if line.startswith("d\t"):
            d = line.split("\t")[-1]
            if d in template_dirs or d in seen_dirs:
                continue
            seen_dirs.add(d)
        wanted.append(line)
    appended = [line for line in wanted if line + "\n" not in present]
    # init stops at the first directory it cannot create, so a path the template already has (with
    # another owner or mode) leaves every later line undone and skips the logd markers (bo60).
    repeated = [line for line in appended if line.startswith("d\t") and line.split("\t")[-1] in template_dirs]
    assert not repeated, f"userland/var.skel.extra repeats directories of the template: {repeated}"
    if appended:
        with skeleton.open("a") as out:
            out.write("".join(line + "\n" for line in appended))
    stale = [line for line in appended if line not in WIFI_CONTROL_LINES]
    if stale:
        print(f"etc/var.skel in the base root is older than userland/var.skel.extra: added "
              f"{len(stale)} line(s); rerun userland/build.sh to rebuild the base root")
    # The count of lines added is a middle quantity: it says what this step did, not what the image
    # carries. These two names are what a round is measured on, so the pack fails when either is
    # missing from the packed skeleton, whoever broke it and however.
    for required in ("/private/var/mobile/Library/MobileInstallation",
                     "/private/var/mobile/Library/SpringBoard"):
        assert required in skeleton.read_text(), (
            f"the packed etc/var.skel has no line for {required}: init will not create that "
            f"directory and the round cannot test anything that needs it")
    shutil.copy2(shim, dest / "lib/libredwood-sbmetal-experiment.dylib")
    shutil.copy2(driver, dest / "lib/libredwoodmetal.dylib")
    (dest / "lib/RedwoodMetal.bundle").mkdir(exist_ok=True)
    shutil.copy2(driver_bundle, dest / "lib/RedwoodMetal.bundle/RedwoodMetal")
    # What lands in the image has to be the library that was just built: on 2026-10-29 a pack left
    # 9d66dd36… in the root while the staging copy was d296b5a4…, and nothing noticed.
    assert sha(dest / "lib/libredwoodmetal.dylib") == sha(driver), \
        "the driver in the image is not the one this pack built"
    shutil.copy2(air, dest / "lib/libredwoodair.dylib")
    shutil.copy2(sb26, dest / "lib/libredwood-sb26.dylib")
    if XPCWATCH_PROCESSES:
        shutil.copy2(xpcwatch, dest / "lib/libredwood-xpcwatch.dylib")
    if AX_PROCESSES:
        shutil.copy2(ax, dest / "lib/libredwood-ax.dylib")
    if AUDIO:
        shutil.copy2(audio, dest / AUDIO_PLUGIN.lstrip("/"))
    if BLUETOOTH:
        shutil.copy2(bluetool, dest / "bin/bluetool")
        shutil.copy2(IOS_DIR / "rootfs/usr/sbin/bluetoothd", dest / "bin/bluetoothd")
    shutil.copy2(userland, dest / "lib/libredwood.dylib")
    shutil.copy2(media, dest / "lib/libredwood-media.dylib")
    shutil.copy2(vt, dest / "lib/libredwood-vt.dylib")
    shutil.copy2(vdec_test, dest / "bin/video_decode_test")
    shutil.copy2(vplay_test, dest / "bin/video_player_test")
    shutil.copy2(vcomb_test, dest / "bin/video_combined_test")
    if reflection_built:
        shutil.copy2(reflection_test, dest / "bin/metal_reflection_test")
    shutil.copy2(app, dest / "lib/libredwood-app.dylib")
    assert sha(dest / "lib/libredwood.dylib") == sha(userland), \
        "libredwood.dylib in the image is not the one this pack built"
    assert cpusubtype(dest / "lib/libredwood.dylib") == ARM64E_WITH_LIB64, (
        f"libredwood.dylib is cpusubtype {cpusubtype(dest / 'lib/libredwood.dylib'):#x}, and this "
        "image's processes are arm64e: rebuild it with -arch arm64e and an iOS 26 SDK, as build.sh "
        "does for its other libraries, then sign it")
    for dylib_file in sorted((dest / "lib").glob("*.dylib")):
        sig = subprocess.run(["/usr/bin/codesign", "-dv", str(dylib_file)], capture_output=True, text=True)
        assert "Signature=adhoc" in sig.stderr, f"{dylib_file} is not adhoc signed: {sig.stderr}"
    for check_dir in (dest / "sbin", dest / "bin"):
        if check_dir.is_dir():
            for exe_file in sorted(check_dir.iterdir()):
                if exe_file.is_file() and not exe_file.is_symlink():
                    sig = subprocess.run(["/usr/bin/codesign", "-dv", str(exe_file)], capture_output=True, text=True)
                    if "not signed at all" in sig.stderr:
                        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(exe_file)], check=True)
    for program in EXTRA_PROGRAMS:
        assert program.is_file(), f"{program} is named in PAYLOAD_PROGRAMS and is not a file"
        shutil.copy2(program, dest / "bin" / program.name)
    for required_bin in ("bin/rwsh",):
        assert (dest / required_bin).is_file(), f"missing /{required_bin} in packed root; run userland/build.sh"
    # /redwood programs must be present in the helper image before its jobs are packaged.
    for plist_path in sorted((dest / "LaunchDaemons").glob("*.plist")):
        plist_data = plistlib.loads(plist_path.read_bytes())
        program = (plist_data.get("ProgramArguments") or [plist_data.get("Program")])[0]
        # Programs under /usr, /bin and /System come from the system volume. mkjobs.py checks
        # their source files while generating jobs; this loop checks the helper image's files.
        if not program or not program.startswith("/redwood/"):
            continue
        # this image is what mounts at /redwood, so /redwood/sbin/init is dest/sbin/init here
        relative = program[len("/redwood"):] if program.startswith("/redwood/") else program
        assert (dest / relative.lstrip("/")).exists(), (
            f"{plist_path.name} runs {program}, which is missing from the helper image")
    held = set()
    for text in [air.read_bytes()]:
        held = set(t for t in AIR_REQUIRED if t.encode() in text)
    missing = [t for t in AIR_REQUIRED if t not in held]
    assert not missing, f"the AIR library does not hold {missing}; check build-air.py --check"
    for name, present in record.get("holds", {}).items():
        assert present or name not in AIR_REQUIRED, f"build-air.py recorded {name} as absent"
    assert len(INJECT.rstrip("\n ").encode()) < 4095, f"etc/inject.conf is {len(INJECT)} bytes, over 4094"
    (dest / "etc/inject.conf").write_text(INJECT)
    for ext_name, settings in EXT_ENV.items():
        (dest / "etc/env").mkdir(exist_ok=True)
        (dest / "etc/env" / ext_name).write_text("".join(s + "\n" for s in settings))
    if PROBE:
        shutil.copy2(probe, dest / "bin/lsprobe")
    if WIFI_FW or BT_FW or IRIS_FW:
        (dest / "fw").mkdir(exist_ok=True)
    for name in WIFI_FW_FILES if WIFI_FW else []:
        shutil.copy2(WIFI_FW_DIR / name, dest / "fw" / name)
    if WIFI_FW:
        (dest / "fw" / WIFI_CONTROL).write_bytes(WIFI_CONTROL_PLIST)
    for name in BT_FW_FILES:
        shutil.copy2(BT_FW_DIR / name, dest / "fw" / name)
    if IRIS_FW:
        shutil.copy2(IRIS_FW_DIR / IRIS_FW_FILE, dest / "fw" / IRIS_FW_FILE)
    # the desktop's own gesture script in place of build.sh's etc/rc.jobs: an acceptance run needs
    # the eight rounds the harness measures. The probe's line ends with "&": init runs the lines of rc.jobs
    # one after another and would otherwise hold the gestures for the probe's last offset.
    (dest / JOBS).write_bytes(jobs)
    job = add_switch(dest)
    if MSAA:
        add_msaa(dest)
    cache = add_extra_jobs(dest)
    rest_added = []
    if REST:
        cache, rest_added = add_rest_jobs(dest, cache)
    if REDWOOD_DIAG:
        cache = add_diag(dest, cache)
        # add_diag rewrote this job too; the assertion further down compares the packed plist with
        # what this variable holds, so it has to be read again rather than kept from add_switch.
        job = plistlib.loads((dest / JOB).read_bytes())
    if JOB_ENV:
        cache = add_env(dest, cache, JOB_ENV)
        job = plistlib.loads((dest / JOB).read_bytes())
    (dest / "xpc/launchd.plist").write_bytes(plistlib.dumps(cache, fmt=plistlib.FMT_BINARY))
    # What launchd will know this boot, names only: the cache is the authority (iOS 26 takes its jobs
    # from /System/Library/xpc/launchd.plist, not from the LaunchDaemons directory), so reading the
    # labels back out of it answers "which jobs did this round carry" without replaying EXTRA_JOBS,
    # ON_DEMAND_JOBS and JOBS_DROP in the reader's head.
    jobs_effective = sorted(Path(key).name for key in cache["LaunchDaemons"])

    with (OUT / "root-build.log").open("w") as log:
        subprocess.run([sys.executable, str(TOOLS / "mkapfs.py"), "-z", "--plain", "fw",
                        "-o", str(OUT / "root.apfs"), "redwood=" + str(dest)],
                       stdout=log, stderr=log, check=True, cwd=WORKSPACE)

    after, bodies = records(OUT / "root.apfs")
    # The staged root and the finished image are two different things: the check above reads the
    # directory this script filled, and only this one answers what the device will load.
    for name, product in (("/lib/libredwood.dylib", userland),
                          ("/lib/libredwoodmetal.dylib", driver)):
        assert after[name]["sha256"] == sha(product), \
            f"{name} in root.apfs is not what this pack built"
    added = ({"/LaunchDaemons/" + name for name in EXTRA_JOBS if name not in JOBS_DROP}
             | ({"/bin/lsprobe"} if PROBE and "/bin/lsprobe" not in before else set())
             | ({"/bin/bluetool", "/bin/bluetoothd"} - set(before) if BLUETOOTH else set())
             | (({"/bin/video_decode_test", "/bin/video_player_test", "/bin/video_combined_test"}
                | ({"/bin/metal_reflection_test"} if reflection_built else set())) - set(before))
             | ({"/fw"} - set(before) if (WIFI_FW or BT_FW or IRIS_FW) else set())
             | ({"/fw/" + name for name in [*WIFI_FW_FILES, WIFI_CONTROL]} - set(before) if WIFI_FW else set())
             | ({"/fw/" + name for name in BT_FW_FILES} - set(before))
             | ({"/fw/" + IRIS_FW_FILE} - set(before) if IRIS_FW else set())
             | {"/bin/" + program.name for program in EXTRA_PROGRAMS if "/bin/" + program.name not in before}
             | ({"/lib/libredwood-xpcwatch.dylib"} if XPCWATCH_PROCESSES else set())
             | ({"/lib/libredwood-ax.dylib"} if AX_PROCESSES else set())
             | ({"/lib/libredwood-vt.dylib"} - set(before))
             | {"/LaunchDaemons/" + name for name in rest_added}
             | ({"/etc/env"} | {"/etc/env/" + name for name in EXT_ENV} if EXT_ENV else set())
             # build.sh stages the AIR library only when build-air.py had already written it
             | ({"/lib/libredwoodair.dylib"} - set(before)))
    assert set(after) - set(before) == added, \
        f"unexpected files: {sorted(set(after) - set(before) - added)}"
    assert set(before) - set(after) == set(), sorted(set(before) - set(after))
    changes = {n: {k: [before[n][k], after[n][k]] for k in before[n] if before[n][k] != after[n][k]}
               for n in before if before[n] != after[n]}
    for name in added:
        changes[name] = {"sha256": [None, after[name]["sha256"]]}
    expected = {"/lib/libredwood-sbmetal-experiment.dylib", "/lib/libredwoodmetal.dylib",
                "/lib/RedwoodMetal.bundle/RedwoodMetal",
                "/lib/libredwoodair.dylib", "/lib/libredwood.dylib", "/lib/libredwood-sb26.dylib",
                "/lib/libredwood-media.dylib", "/lib/libredwood-vt.dylib", "/lib/libredwood-app.dylib",
                "/bin/video_decode_test", "/bin/video_player_test", "/bin/video_combined_test",
                *(["/bin/metal_reflection_test"] if reflection_built else []),
                *(["/lib", "/lib/libredwood-xpcwatch.dylib"] if XPCWATCH_PROCESSES else []),
                *(["/lib", "/lib/libredwood-ax.dylib"] if AX_PROCESSES else []),
                *([AUDIO_PLUGIN] if AUDIO else []),
                *(["/bin/bluetool", "/bin/bluetoothd"] if BLUETOOTH else []),
                "/etc/inject.conf",
                "/etc/rc.jobs", "/" + JOB, *(["/" + ICON_JOB] if ICON_METAL else []),
                "/xpc/launchd.plist", *["/bin/" + program.name for program in EXTRA_PROGRAMS],
                # Anything added under a directory changes that directory's own times, and records()
                # reports the directory alongside the file, so the parent of every added file belongs
                # in this set. /bin is the only one today: the extra jobs' directory is listed on the
                # next line with the same reason. A new file somewhere else needs its parent added
                # here or the comparison above fails on a directory nobody meant to change.
                *(["/bin"] if any(name.startswith("/bin/") for name in added) else []),
                *(["/lib"] if any(name.startswith("/lib/") for name in added) else []),
                *(["/etc", "/etc/env", *("/etc/env/" + name for name in EXT_ENV)] if EXT_ENV else []),
                *(["/bin/lsprobe"] if PROBE else []),
                *(["/fw", *("/fw/" + name for name in [*WIFI_FW_FILES, WIFI_CONTROL])] if WIFI_FW else []),
                *(["/fw", *("/fw/" + name for name in BT_FW_FILES)] if BT_FW else []),
                *(["/fw", "/fw/" + IRIS_FW_FILE] if IRIS_FW else []),
                # the two jobs below change this directory's own times as well
                "/LaunchDaemons", *["/LaunchDaemons/" + name for name in EXTRA_JOBS if name not in JOBS_DROP],
                *["/LaunchDaemons/" + name for name in rest_added],
                # the diagnostic switch rewrites the two jobs whose processes load the driver
                *(["/LaunchDaemons/" + name for name in MSAA_JOBS] if REDWOOD_DIAG else []),
                *{"/LaunchDaemons/" + name for name, _, _ in JOB_ENV},
                # the skeleton is rewritten only when the base root is too old to carry a line that
                # userland/var.skel.extra names, or with WIFI_FW (WIFI_CONTROL_LINES); an empty
                # skeleton_appended means it was left alone
                *(["/etc/var.skel"] if appended else [])}
    # A file this pack installs can equal the base's copy (build.sh builds the shim and sb26 with the
    # same command), so the set is an upper bound; the checks below read back what has to be there.
    assert set(changes) <= expected, f"unexpected changes: {sorted(set(changes) - expected)}"
    for name, diff in changes.items():
        assert set(diff) <= {"sha256", "xattrs", "internal_flags", "nlink"}, (name, sorted(diff))

    assert bodies["/etc/inject.conf"].decode() == INJECT
    assert bodies["/etc/rc.jobs"] == jobs
    assert plistlib.loads(bodies["/" + JOB]) == job
    cache = plistlib.loads(bodies["/xpc/launchd.plist"])
    assert cache["LaunchDaemons"][CACHE_KEY] == job, "the launchd cache and the job plist disagree"
    assert job["EnvironmentVariables"]["REDWOOD_SB_METAL"] == "1"
    if ICON_METAL:
        icon_job = plistlib.loads(bodies["/" + ICON_JOB])
        assert cache["LaunchDaemons"][ICON_CACHE_KEY] == icon_job, "iconservicesagent: cache and plist disagree"
        assert icon_job["EnvironmentVariables"]["REDWOOD_SB_METAL"] == "1"
    for name in EXTRA_JOBS:
        if name in JOBS_DROP:
            continue
        extra = plistlib.loads(bodies["/LaunchDaemons/" + name])
        if name in ON_DEMAND_JOBS:
            assert "RunAtLoad" not in extra and extra["EnablePressuredExit"] is True, name
            assert extra["MachServices"], name
            continue
        assert extra["KeepAlive"] is False, name
        if name in LATE_JOBS:
            assert extra["RunAtLoad"] is False and extra["StartInterval"] == 120, name
        else:
            assert extra["RunAtLoad"] is True, name
        assert cache["LaunchDaemons"]["/System/Library/LaunchDaemons/" + name] == extra, name
    for name, path in [("/lib/libredwood-sbmetal-experiment.dylib", shim),
                       ("/lib/libredwoodmetal.dylib", driver),
                       ("/lib/libredwoodair.dylib", air),
                       *[("/bin/" + program.name, program) for program in EXTRA_PROGRAMS],
                       *([("/bin/lsprobe", probe)] if PROBE else [])]:
        assert hashlib.sha256(bodies[name]).hexdigest() == sha(path), name

    driver_sha, air_sha = sha(driver), sha(air)

    report = {
        "mode": "full",
        "kernel": {"commit": fork_head()},
        "base": str(BASE.relative_to(SRC_ROOT)), "base_root_sha256": protected[str(base_image)],
        "driver_sha256": driver_sha, "air_sha256": air_sha, "shim_sha256": sha(shim),
        "air_checks": air_checks,
        "air_source_sha256": air_source,
        "userland_sha256": sha(userland), "userland_bytes": userland.stat().st_size,
        "inject_source_sha256": sources.get("userland/src/inject/inject.c"),
        "air_sources": air_sources,
        # Every source this pack read, with its sha256: shim, driver, probe, AIR, and the kernel commit
        # is in `kernel` above. Without this section a round cannot be asked which code it ran.
        "sources": sources, "source_origins": origins,
        # What this round ran. A round is a set of choices, and the manifest has to name them: the
        # gesture script, the probe sources, the job set, the libraries, the kernel. air_sources above
        # closed half of that gap; these close the other half.
        "jobs_source": {"path": str(JOBS_SOURCE.relative_to(WORKSPACE))
                        if JOBS_SOURCE.is_relative_to(WORKSPACE) else str(JOBS_SOURCE),
                        "sha256": sha(JOBS_SOURCE),
                        "given": JOBS_SOURCE_GIVEN},
        "probe_sources": {name: sources[name] for name in PROBE_SOURCES if name in sources},
        "build_commands": commands, "source_sha256": sources,
        "inject_conf": INJECT.strip(),
        "jobs_sha256": hashlib.sha256(jobs).hexdigest(), "probe": PROBE,
        "skeleton_appended": appended, "jobs_dropped": sorted(JOBS_DROP), "job_env": [f"{n}:{k}={v}" for n, k, v in JOB_ENV],
        "ext_env": [f"{n}:{s}" for n, settings in EXT_ENV.items() for s in settings],
        "jobs_effective": jobs_effective, "jobs_effective_count": len(jobs_effective),
        "rest": REST, "rest_added_count": len(rest_added), "rest_allow": REST_ALLOW, "rest_deny": REST_DENY,
        "redwood_diag": REDWOOD_DIAG,
        "wifi_fw": {name: sha(WIFI_FW_DIR / name) for name in WIFI_FW_FILES}
                   if WIFI_FW else None,
        "bt_fw": {name: sha(BT_FW_DIR / name) for name in BT_FW_FILES} if BT_FW else None,
        "changes": changes,
        "dirty_worktree": bool(dirty), "worktree_status": dirty,
        # A round can change more than the one variable its request names. Anything passed here is
        # carried into the archive so the round is read as what it actually ran.
        "round_notes": [note.strip() for note in os.environ.get("ROUND_NOTES", "").split(";") if note.strip()],
        "gpu_speed": GPU_SPEED, "dirty_regions": DIRTY_REGIONS,
        "root_sha256": sha(OUT / "root.apfs"), "root_bytes": (OUT / "root.apfs").stat().st_size,
        "expected_runtime_log": ["redwoodmetal[SpringBoard", "redwoodmetal[backboardd",
                                 "sb26: boot unlock done after", "MiniIOMobileFramebuffer:",
                                 "RedwoodGPU"],
        "must_not_appear": ["incompatible architecture", "unrecognized selector",
                            "OS_REASON_COREANIMATION", "nowhere for that store to go"],
        "protected_verified": protected,
    }
    # The manifest has to say which sources this pack read. quiet20 is unaccountable for exactly one
    # reason: its manifest carried no `sources` section, so nothing recorded whether the driver in the
    # image came from the tree that has the dealloc prints. A pack that cannot name its inputs is not
    # written at all, rather than left for somebody to reconstruct later.
    required = [*SHIM_SOURCES, *DRIVER_SOURCES, *DRIVER_INCLUDES,
                "tools/mesa/air/air_metal.c", "tools/mesa/air/air_to_nir.c"]
    absent = [name for name in required if not report.get("sources", {}).get(name)]
    if not report.get("sources") or absent:
        raise SystemExit("the manifest would not record the build inputs, missing: "
                         + (", ".join(absent) if absent else "the whole sources section")
                         + f"\n(the kernel commit is {report.get('kernel', {}).get('commit', '?')})")
    (OUT / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in
                      ["base_root_sha256", "shim_sha256", "driver_sha256", "air_sha256",
                       "root_sha256", "root_bytes"]}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver-only", action="store_true",
                        help="build the tools/userland-driver driver and verify it, without "
                             "the iOS rootfs (the CI scope; no root.apfs is produced)")
    args = parser.parse_args()
    if args.driver_only:
        driver_only()
    else:
        main()
