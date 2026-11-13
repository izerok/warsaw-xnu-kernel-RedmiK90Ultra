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
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"UNKNOWN" # [S3] annibale used "13" (its own model number); warsaw's is not in our materials -- do not copy
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"warsaw" # [S1] device codename
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"UNKNOWN" # [S3] annibale used "CPH2653"; warsaw equivalent unknown (marketing model 2604FRK1EC is NOT this field's proven value)
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"UNKNOWN" # [S3] annibale used "23893"; warsaw board model unknown

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
