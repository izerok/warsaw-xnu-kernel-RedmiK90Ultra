##
#  warsawPkg platform description -- scaffold adapted from annibalePkg
#  (iola1999/hackios-mu-silicium @ 90721af9041bcf8683d12583cc12ba4e4658a48b).
#  Per-value source classes: [S1] derived from warsaw's own materials,
#  [S2] borrowed from annibale (same SoC, unverified on warsaw hardware),
#  [S3] unknown -- placeholder, must be filled from the real device.
#  See this package's README.md. NOT built, NOT device-verified.
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = warsaw
  PLATFORM_GUID                  = C6430DBE-0D0E-48BA-BAFC-E8D00C1785D8 # fresh GUID for warsawPkg; must NOT reuse annibale's FA3B7E2A-...
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/warsawPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = warsawPkg/warsaw.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8750-AB
  # 1 = SM8750-3-AB
  # 2 = SM8750-AC
  #
  SOC_TYPE                       = 0 # [S1] warsaw.conf: SM8750-AB (silicon revision to re-confirm on hardware)

!include PakalaPkg/PakalaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000 # [S1] warsaw.dtb /memory base (gunyah_hyp_region@0x80000000)

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000 # [S2] Silicium UEFI layout convention; must stay consistent with MemoryMapLib "UEFI_Stack"
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000 # [S2]

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi" # [S1]
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"REDMI K90 Ultra" # [S2] market name: lrqtech system.prop comment ("市场名 REDMI K90 Ultra") + LKML sm8750-xiaomi-warsaw.dts model = "REDMI K90 Ultra"; dodge parallel: SystemModel = marketing model ("13")
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"warsaw" # [S1] device codename
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"2604FRK1EC" # [S2] retail model number: GSMArena "Models 2604FRK1EC" + 3C certification + mifirmware table (REDMI K90 Ultra | warsaw | 2604FRK1EC); dodge parallel: RetailSku = retail model no. ("CPH2653"); caveat: same number also listed for prague (K90 Max)
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Qualcomm Technologies, Inc. Warsaw based on SM8750" # [S1] DT root `model` in MiCode kernel_devicetree@warsaw-w-oss (HEAD 87154a67), qcom/warsaw-sm8750.dtsi:12 + qcom/warsaw-sm8750-overlay.dts:7 (identical); cross-check: qcom,board-id <0x40008 0> in the same file matches warsaw.dtb; live-device root model verified 2026-11-18 (adb shell su, /sys/firmware/fdt) — identical. Replaces "12" (xiaomi,miboard-id is a kernel board ID, not the board model; base dtsi has 0xB, overlay 0xC).

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1280 # [S1] warsaw panel 1280x2772 (warsaw.conf)
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2772 # [S1]
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32 # [S2]

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA" # [S2]

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|warsawPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|warsawPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  #warsaw/AcpiTables.inf
