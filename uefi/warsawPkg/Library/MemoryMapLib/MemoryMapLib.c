/** @file
  MemoryMapLib for warsaw (REDMI K90 Ultra, SM8750).

  Scaffold adapted entry-by-entry from annibalePkg's MemoryMapLib.c
  (iola1999/hackios-mu-silicium @ 90721af9041bcf8683d12583cc12ba4e4658a48b).
  Every entry is tagged with its source class:
    [S1] derived from warsaw's own materials (common/devices/warsaw.conf,
         common/devices/warsaw.dtb reserved-memory -- extracted 2026-11-13);
    [S2] borrowed from annibale / Silicium SM8750 layout convention,
         plausible but NOT verified on warsaw hardware;
    [S3] unknown or contradicted for warsaw -- placeholder only, the FD
         must not be booted until this is resolved from the real device.
  See ../../README.md and the fork's PHASE4-ALIGN.md for the full table.
  NOT built, NOT device-verified.
  API note (2026-11-13, phase 9): this file originally used the
  ARM_MEMORY_REGION_DESCRIPTOR_EX / GetPlatformMemoryMap form, which does
  NOT exist at the pinned Mu tree (90721af9) -- no such type or
  EFI_PHYSICAL_ADDRESS_MAX anywhere in Mu_Basecore, and the consumers
  (MemoryInitPeiLib, MemoryMapHelperLib) call GetMemoryMap with the
  EFI_MEMORY_REGION_DESCRIPTOR form. Corrected to annibalePkg's shape at
  the same pin (table rows and values unchanged); see PHASE9-BINARIES.md §5.

  Copyright (c) Microsoft Corporation.
  Copyright (C) 2023 - 2024, Qianqian (Duo) Wang.
  Copyright (C) 2024, Renegade Project.
  Copyright (C) 2024, The Silicium Project.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/MemoryMapLib.h>

static EFI_MEMORY_REGION_DESCRIPTOR gMemoryDescriptor[] = {
  /* Name               Address                 Length                  HobOption        ResourceType          ResourceAttribute    MemoryType          ArmAttributes
  */
  /* DDR Regions */
  {"RAM",                 0x80000000,             0x100000000,            AddMem,          SYS_MEM,            SYS_MEM_CAP,         Conv,               WRITE_BACK_XN},
  {"RSRV0",               0x81960000,             0x000A0000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2] Silicium SM8xxx template carve-out: identical address/length in annibale/onyx/dodge/pagani maps; no warsaw.dtb reserved node claims this span (it ends exactly at XBL_DT) and none contradicts it -- phase-6 ruling: KEEP (deleting it would hand the span to the UEFI allocator as plain RAM); not DT-derived, still pending real-device confirmation (PHASE6-OPEN-ITEMS C1)
  {"XBL_DT",              0x81A00000,             0x00040000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1] warsaw.dtb xbl_dtlog_region@81a00000, len 0x40000 -- identical
  {"AOP",                 0x81C00000,             0x000A0000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2] warsaw.dtb splits this span: aop_image 0x60000 + aop_cmd_db 0x20000 + aop_tme_uefi_merged 0x74000 -- re-derive before use
  {"UEFI_Log",            0x81CE4000,             0x00010000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2] ends exactly where warsaw.dtb dump_display_region@81cf4000 begins; consistent, unverified
  {"SMEM",                0x81D00000,             0x00200000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1] warsaw.dtb smem_region -- identical
  {"PDP_NS",              0x81F00000,             0x00100000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1] warsaw.dtb pdp_ns_shared_region -- identical
  {"PvmFw",               0x824A0000,             0x00100000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1] warsaw.dtb pvm_fw_region -- identical
  {"PIL_Reserved",        0x89E00000,             0x19700000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2] phase-6 correction: DT-vs-DT comparison shows warsaw.dtb and annibale's FdtBlob DT have IDENTICAL reserved-memory layouts (63/63 fixed nodes, same addr/len), so this umbrella covers the same subsystem cluster (mpss..adspslpi, ending at ramoops) on warsaw; it also over-reserves ~2.7 MiB the runtime DT lists as RAM (same as on annibale). NOT [S1]: the endpoints are template choices, not uniquely DT-determined. Supersedes the phase-4 "[S3] layout differs" note, which compared the umbrella against individual nodes (PHASE6-OPEN-ITEMS C1)
  {"Display_Demura",      0xA3500000,             0x02C80000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             WRITE_BACK_XN}, // [S3] CONTRADICTED on warsaw: its demura_region_0/1 are zero-length and 0xA3500000 is ramoops-region (len 0x400000) -- do not copy
  {"Kernel",              0xA8000000,             0x10000000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Conv,               WRITE_BACK_XN}, // [S2] Silicium layout convention
  {"XBL_Ramdump",         0xB8000000,             0x001C0000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1/S2] warsaw.dtb xbl_ramdump_region reserves len 0x200000 on the Android side (tranche 4, kept on record), but every SM8750 Silicium map (annibale/dodge/pagani) carves 0x1C0000 and the dodge/pagani firmware-side DTs declare xbl_ramdump_region len 0x1C0000 (tranche 7, PHASE7-CROSSMODEL.md task B); annibale itself carries both views (map 0x1C0000 vs Android DT 0x200000). 0x1C0000 also removes the overlap with DBI_Dump below; the union of the two carves still covers the whole DT-reserved span 0xB8000000-0xB8200000
  {"DBI_Dump",            0xB81C0000,             0x05770000,             NoHob,           SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1] warsaw's own XBL platform DT: xbl_config.elf SHA-256 f02a935fdbd09c5cb96671aa437dbd710652d207eaa9754eb14f47e3ce75d331, ph3-0000840c.dtb mem-label 'DBI_Dump', len 0x5770000. Supersedes the tranche-7 [S2] template 0xF00000 (annibale/dodge; pagani already used 0x5770000 as its own XBL value). The phase-6 fill candidate 0xB8200000/0xEC0000 stays REFUTED -- do not revive. Adjudicated 2026-11-13 by main agent, PHASE9-BINARIES.md §4.4
  {"DXE_Heap",            0xBD930000,             0x1A6D0000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Conv,               WRITE_BACK_XN}, // [S1] warsaw's own XBL platform DT (same source as DBI_Dump above, mem-label 'DXE_Heap'): base 0xBD930000 = DBI_Dump end, len 0x1A6D0000, ends 0xD8000000. Supersedes the [S2] template 0xB90C0000/0x1D500000, whose base fell 0x4870000 inside the DT's DBI region (real overlap defect, 72.5 MB). Adjudicated 2026-11-13 by main agent, PHASE9-BINARIES.md §4.4

  /* UEFI Regions */
  {"FD_Reserved",         0xA7000000,             0x00100000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2] Silicium layout; the FD window it guards is [S1]
  {"UEFI_FD",             0xA7100000,             0x00300000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         BsData,             NS_DEVICE},  // [S1] warsaw.conf FD_BASE/FD_SIZE (device xbl_config embedded DT)
  {"UEFI_FD_Reserved",    0xA7400000,             0x00200000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S1/S2] start 0xA7400000 confirmed by warsaw.conf (FD end); length is layout convention
  {"Info_Blk",            0xA7601000,             0x00001000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2]
  {"MMU_PageTables",      0xA7602000,             0x00003000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2]
  {"Log_Buffer",          0xA7605000,             0x00008000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2]
  {"UEFI_Stack",          0xA760D000,             0x00040000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         BsData,             NS_DEVICE},  // [S2] must match warsaw.dsc PcdCPUCoresStackBase/Size
  {"SEC_Heap",            0xA764D000,             0x0008C000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         BsData,             NS_DEVICE},  // [S2]
  {"Sched_Heap",          0xA76D9000,             0x00400000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         BsData,             NS_DEVICE},  // [S2]
  {"FV_Region",           0xA7AD9000,             0x00400000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         BsData,             NS_DEVICE},  // [S2]
  {"UEFI_RESV",           0xA7ED9000,             0x00127000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             NS_DEVICE},  // [S2]

  /* Other Memory Regions */
  {"IPC_ROUTER_TOP",      0x00400000,             0x00100000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SM8750 SoC register region
  {"SECURITY_CONTROL",    0x00780000,             0x00001000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"QUP",                 0x00800000,             0x00300000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"PRNG_CFG_PRNG",       0x010C0000,             0x00001000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"CRYPTO0",             0x01DC0000,             0x00040000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"CLK_TCSR",            0x01F00000,             0x00100000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"PERIPH_SS",           0x08800000,             0x04C00000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"USB",                 0x0A600000,             0x06040000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"AOSS",                0x0B000000,             0x04000000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"TLMM",                0x0F000000,             0x01000000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"IMEM_Base",           0x14680000,             0x0002A000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC (SharedIMEMBaseAddr 0x14680000 in ConfigurationMapLib)
  {"IMEM_Cookie",         0x146AA000,             0x00016000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"SMMU",                0x15000000,             0x00200000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC
  {"APSS_HM",             0x16000000,             0x06000000,             AddDev,          MMAP_IO,            INITIALIZED,         Reserv,             UNCACHEABLE}, // [S2] SoC

  /* K90/XNU Regions */
  {"XNU_Payload",         0xF0000000,             0x03800000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             WRITE_BACK_XN}, // [S1] RESOLVED (phase-6): this length is the reservation WINDOW, not the container cap -- Silicium XnuLoader.h defines REGION_SIZE 0x03800000 mirroring this entry and MAX_SIZE 0x03700000 (== warsaw.conf PAYLOAD_CAP) as the loader/packer-enforced container limit, the top 1 MiB being the panic-log tail; the window end 0xF3800000 is exactly warsaw.dtb trust_ui_vm_region's start. No discrepancy (PHASE6-OPEN-ITEMS C2)
  {"Display_Reserved",    0xFC800000,             0x02B00000,             AddMem,          SYS_MEM,            SYS_MEM_CAP,         Reserv,             WRITE_THROUGH_XN}, // [S1] warsaw.dtb splash_region -- identical; also warsaw.conf FB_BASE

};

/**
  This Function returns the Memory Map Data.

  @param[out] MemoryDescriptor             - The Memory Map.
  @param[out] MemoryDescriptorCount        - The Number of Entries in the Memory Map.
**/
VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
