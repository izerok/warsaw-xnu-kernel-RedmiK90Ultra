/*
 * Test redwood_soc.c (the SoC addresses, from boot-args).
 *
 * The kernel reads soc_* boot-args through PE_parse_boot_argn; here it is a stand-in
 * backed by a table the test fills in, declared in shim/pexpert/pexpert.h. The values
 * below are made up: no real SoC address is baked into this test (the real ones come
 * from tools/soc-args.py on the phone's own device tree).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/redwood_soc.h>
#include <pexpert/pexpert.h>

static int checks, failures;

static void
check(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		fprintf(stderr, "FAIL %s\n", what);
	}
}

/* Fake boot-args for the test. */
struct fake_arg { const char *arg; uint32_t value; const char *str; };
static const struct fake_arg *fake_args;
static unsigned fake_nargs;

boolean_t
PE_parse_boot_argn(const char *arg, void *value, int max_len)
{
	unsigned i;

	for (i = 0; i < fake_nargs; i++) {
		if (strcmp(arg, fake_args[i].arg) == 0) {
			if (fake_args[i].str != NULL) {
				size_t n = strlen(fake_args[i].str);

				if ((size_t)max_len <= n) return 0; /* no room for the NUL */
				memcpy(value, fake_args[i].str, n + 1);
			} else if (max_len >= (int)sizeof(uint32_t)) {
				*(uint32_t *)value = fake_args[i].value;
			} else {
				return 0;
			}
			return 1;
		}
	}
	return 0;
}

int
main(void)
{
	static const struct fake_arg args1[] = {
		{ "soc_aoss", 0x12340000, NULL },
		{ "soc_ipa_regs", 0x1e40000, NULL },
		{ "soc_smem", 0x23450000, NULL },
		{ "soc_ipa_fw", 0, "test-ipa-fw" },
	};

	/* A few args given: those fields are set, the rest stay zero. */
	memset(&redwood_soc, 0, sizeof(redwood_soc));
	fake_args = args1;
	fake_nargs = sizeof(args1) / sizeof(args1[0]);
	redwood_soc_override();
	check(redwood_soc.aoss.phys == 0x12340000, "aoss from soc_aoss");
	check(redwood_soc.ipa_regs.phys == 0x1e40000, "ipa_regs from soc_ipa_regs");
	check(redwood_soc.smem == 0x23450000, "smem from soc_smem");
	check(strcmp(redwood_soc.ipa_fw, "test-ipa-fw") == 0, "ipa_fw from soc_ipa_fw");
	check(redwood_soc.ipcc.phys == 0, "ipcc stays zero when not given");
	check(redwood_soc.pas_mpss == 0, "pas_mpss stays zero when not given");
	check(redwood_soc_configured(), "configured when something was given");

	/* A block already mapped keeps its address: the boot-arg must not overwrite it. */
	memset(&redwood_soc, 0, sizeof(redwood_soc));
	redwood_soc.aoss.base = (void *)0x1;
	redwood_soc_override();
	check(redwood_soc.aoss.phys == 0, "mapped block keeps its address");

	/* No args at all: everything stays zero and nothing is configured. */
	memset(&redwood_soc, 0, sizeof(redwood_soc));
	fake_args = NULL;
	fake_nargs = 0;
	redwood_soc_override();
	check(!redwood_soc_configured(), "not configured when nothing was given");
	check(redwood_soc.aoss.phys == 0 && redwood_soc.smem == 0 && redwood_soc.ipa_fw[0] == 0,
	    "all zero when nothing was given");

	if (failures != 0) {
		fprintf(stderr, "test_soc: %d/%d checks failed\n", failures, checks);
		return 1;
	}
	printf("test_soc: %d checks passed\n", checks);
	return 0;
}
