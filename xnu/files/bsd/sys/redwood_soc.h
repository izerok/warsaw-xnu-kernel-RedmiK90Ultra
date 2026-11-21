/* The SoC addresses of another Qualcomm phone, from boot-args.
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.mpss.c, soc_override()):
 * the mechanism (a table of register blocks + scalar values, each read from a soc_*
 * boot-arg) is carried over. Everything the modem and the IPA touch is here: the register
 * blocks mapped at run time (the AOSS and IPCC mailboxes, the RPMh controller, the
 * shared-memory hardware lock, the IPA and GSI registers and the IPA's local memory, the
 * application processors' SMMU) and the few addresses kept as values (shared memory, the
 * command DB, the modem's memory, the SoC's internal memory, the IPA streams and PAS ids).
 * tools/soc-args.py reads them off the phone's own device tree.
 *
 * All fields default to 0 / empty. No Blair defaults are carried over (malmo fills
 * smem_lock 0x343000, apps_smmu 0xc600000, imem 0xc000000, streams 0x4a0/0x4a2 for its
 * own SoC), and no SM8750 values are filled in here either: a port passes soc_<name>=0x...
 * for each address, and anything not given stays 0, which the drivers must treat as
 * "not configured" and refuse to use.
 */
#ifndef _SYS_REDWOOD_SOC_H_
#define _SYS_REDWOOD_SOC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct redwood_soc_block {
	uint32_t phys;                  /* 0: not given */
	void *base;                     /* mapped at run time by the owning driver; NULL until then */
};

struct redwood_soc_addrs {
	/* Register blocks, from boot-args soc_<name>. */
	struct redwood_soc_block aoss, ipcc, rsc, smem_lock;
	struct redwood_soc_block ipa_regs, ipa_sram, gsi_ee0, apps_smmu;
	/* Values, from boot-args soc_<name>. */
	uint32_t smem, cmd_db, mpss_mem, mpss_size, imem;
	uint32_t ipa_stream, ipa_stream_uc;
	uint32_t pas_mpss, pas_ipa;
	uint32_t ipa_base, gsi_base;    /* soc_ipa_base, soc_gsi_base */
	uint32_t ipa_fw_at;             /* modem_ipa_fw_at: where the IPA's firmware is loaded */
	char ipa_fw[64];                /* soc_ipa_fw: the IPA firmware name */
};

extern struct redwood_soc_addrs redwood_soc;

/* Read the soc_* boot-args over the defaults (all zero). A block already mapped
 * (base != NULL) keeps its address. Prints a summary when anything was given. */
void redwood_soc_override(void);
/* True when any address or name was given. */
bool redwood_soc_configured(void);

#endif
