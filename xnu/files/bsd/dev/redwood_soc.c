/*
 * The SoC addresses of another Qualcomm phone, from boot-args.
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.mpss.c, soc_override()).
 * The donor fills Blair defaults when the SoC base matches its own phone; this port
 * fills none: every field starts at 0 / empty, and only a soc_* boot-arg sets it.
 * No Blair-specific addresses or registers in this file.
 */
#include <sys/redwood_soc.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

struct redwood_soc_addrs redwood_soc;

void
redwood_soc_override(void)
{
	static const struct { const char *arg; struct redwood_soc_block *block; } blocks[] = {
		{ "soc_aoss", &redwood_soc.aoss }, { "soc_ipcc", &redwood_soc.ipcc },
		{ "soc_rsc", &redwood_soc.rsc }, { "soc_smem_lock", &redwood_soc.smem_lock },
		{ "soc_ipa_regs", &redwood_soc.ipa_regs }, { "soc_ipa_sram", &redwood_soc.ipa_sram },
		{ "soc_gsi_ee0", &redwood_soc.gsi_ee0 }, { "soc_apps_smmu", &redwood_soc.apps_smmu },
	};
	unsigned said = 0;

	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		uint32_t phys = 0;

		if (PE_parse_boot_argn(blocks[i].arg, &phys, sizeof(phys)) && phys != 0 && blocks[i].block->base == NULL) {
			blocks[i].block->phys = phys;
			said++;
		}
	}
	PE_parse_boot_argn("soc_smem", &redwood_soc.smem, sizeof(redwood_soc.smem));
	PE_parse_boot_argn("soc_cmd_db", &redwood_soc.cmd_db, sizeof(redwood_soc.cmd_db));
	PE_parse_boot_argn("soc_mpss_mem", &redwood_soc.mpss_mem, sizeof(redwood_soc.mpss_mem));
	PE_parse_boot_argn("soc_mpss_size", &redwood_soc.mpss_size, sizeof(redwood_soc.mpss_size));
	PE_parse_boot_argn("soc_imem", &redwood_soc.imem, sizeof(redwood_soc.imem));
	PE_parse_boot_argn("soc_ipa_stream", &redwood_soc.ipa_stream, sizeof(redwood_soc.ipa_stream));
	PE_parse_boot_argn("soc_ipa_stream_uc", &redwood_soc.ipa_stream_uc, sizeof(redwood_soc.ipa_stream_uc));
	PE_parse_boot_argn("soc_pas_mpss", &redwood_soc.pas_mpss, sizeof(redwood_soc.pas_mpss));
	PE_parse_boot_argn("soc_pas_ipa", &redwood_soc.pas_ipa, sizeof(redwood_soc.pas_ipa));
	PE_parse_boot_argn("soc_ipa_base", &redwood_soc.ipa_base, sizeof(redwood_soc.ipa_base));
	PE_parse_boot_argn("soc_gsi_base", &redwood_soc.gsi_base, sizeof(redwood_soc.gsi_base));
	PE_parse_boot_argn("modem_ipa_fw_at", &redwood_soc.ipa_fw_at, sizeof(redwood_soc.ipa_fw_at));
	PE_parse_boot_argn("soc_ipa_fw", &redwood_soc.ipa_fw, sizeof(redwood_soc.ipa_fw));
	if (said != 0 || redwood_soc.smem != 0 || redwood_soc.mpss_mem != 0 || redwood_soc.ipa_fw[0] != 0) {
		printf("soc: SoC addresses from boot-args: %u register %s; shared memory %#x, command DB %#x, the modem's "
		    "memory %#x, the SoC's internal memory %#x\n", said, said == 1 ? "block" : "blocks", redwood_soc.smem, redwood_soc.cmd_db,
		    redwood_soc.mpss_mem, redwood_soc.imem);
	}
}

bool
redwood_soc_configured(void)
{
	unsigned i;
	static const struct redwood_soc_block *const blocks[] = {
		&redwood_soc.aoss, &redwood_soc.ipcc, &redwood_soc.rsc, &redwood_soc.smem_lock,
		&redwood_soc.ipa_regs, &redwood_soc.ipa_sram, &redwood_soc.gsi_ee0, &redwood_soc.apps_smmu,
	};

	for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		if (blocks[i]->phys != 0) return true;
	}
	return redwood_soc.smem != 0 || redwood_soc.cmd_db != 0 || redwood_soc.mpss_mem != 0 ||
	    redwood_soc.mpss_size != 0 || redwood_soc.imem != 0 || redwood_soc.ipa_stream != 0 ||
	    redwood_soc.ipa_stream_uc != 0 || redwood_soc.pas_mpss != 0 || redwood_soc.pas_ipa != 0 ||
	    redwood_soc.ipa_base != 0 || redwood_soc.gsi_base != 0 || redwood_soc.ipa_fw_at != 0 ||
	    redwood_soc.ipa_fw[0] != 0;
}
