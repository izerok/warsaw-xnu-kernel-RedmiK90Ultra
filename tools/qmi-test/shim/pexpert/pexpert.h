/* Host-test stand-in for <pexpert/pexpert.h>: only PE_parse_boot_argn, defined by the test. */
#ifndef _SHIM_PEXPERT_PEXPERT_H_
#define _SHIM_PEXPERT_PEXPERT_H_

typedef int boolean_t;
boolean_t PE_parse_boot_argn(const char *arg, void *value, int max_len);

#endif
