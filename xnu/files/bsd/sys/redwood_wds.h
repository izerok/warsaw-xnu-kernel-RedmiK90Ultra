/* QMI WDS data session: DPM -> WDA -> Bind Mux -> Set IP Family -> Start Network.
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.mpss.c, the data-session
 * section): the order ModemManager follows for a modem behind the IPA. The service
 * and message numbers and the TLV layouts are Qualcomm QMI standard (libqmi's
 * qmi-service-wds.json / qmi-service-wda.json / qmi-service-dpm.json), not
 * Blair-specific.
 *
 * What is generic here: the state machine, the request bytes, the retry rules
 * (DPM/WDA get a 20 s window, a refused Start is retried every 6 s). What is
 * NOT here: the IPA endpoints (to_modem/from_modem, 0 = not configured: DPM is
 * not sent until the IPA driver fills them in), the modem bring-up thread that
 * calls tick() about once a second and feeds registration from the NAS client,
 * and the data-plane consumer of the settings (item 4, cell.c). The caller fills
 * the config fields (data_off / dpm_off / wda_off / nobind / apn) from the
 * modem_* boot-args before open; apn is empty for the SIM's default profile.
 *
 * The octet order of the address TLVs in Get Current Settings is read exactly
 * as the donor reads it (little-endian u32). The donor's session never completed
 * on hardware, so the first real reply must be checked against the printed
 * address before the values are trusted.
 */
#ifndef _SYS_REDWOOD_WDS_H_
#define _SYS_REDWOOD_WDS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct redwood_qrtr_ops;
struct redwood_qrtr_port;

/* One data session's settings, host order, as Get Current Settings reports them. */
struct redwood_wds_settings {
	uint32_t handle;                /* the packet data handle from Start Network */
	uint32_t address, gateway, mask, dns1, dns2, mtu;
};

/* The consumer of the session (item 4, cell.c). Called from the transport's thread. */
struct redwood_wds_ops {
	/* Get Current Settings arrived with an address: the session is up. */
	void (*settings)(void *context, const struct redwood_wds_settings *settings);
	/* The session went down (close, or the settings were withdrawn). */
	void (*down)(void *context);
};

struct redwood_wds_client {
	const struct redwood_qrtr_ops *transport;
	const struct redwood_wds_ops *ops;
	void *context;
	/* A microsecond clock; the kernel caller passes a mach_absolute_time wrapper
	 * (redwood_adsp.c has the same shape as now_us()). The host test passes a fake. */
	uint64_t (*now_us)(void *context);
	void *clock_context;

	/* Config, filled by the caller before open. */
	char apn[64];                   /* empty: the SIM's default profile (boot-arg modem_apn) */
	bool data_off;                  /* no data session at all (modem_data_off) */
	bool dpm_off;                   /* skip the DPM step (modem_dpm_off) */
	bool wda_off;                   /* skip the WDA step (modem_wda_off) */
	bool nobind;                    /* the session without binding the client to the mux */
	uint32_t to_modem;              /* IPA endpoint the modem receives on; 0 = not configured */
	uint32_t from_modem;            /* IPA endpoint the modem sends on; 0 = not configured */

	struct redwood_qrtr_port *wds_port, *wda_port, *dpm_port;
	uint32_t wds_node, wds_port_number, wda_node, wda_port_number, dpm_node, dpm_port_number;
	bool registered;                /* NAS says the modem is registered */
	bool started;                   /* Start Network was answered with a handle */
	bool up;                        /* settings arrived with an address */
	bool bound;                     /* Bind Mux Data Port answered success */
	bool wda_set;                   /* WDA Set Data Format answered success */
	bool dpm_done, dpm_open, wda_sent, wda_done;
	uint64_t data_setup_began;      /* when WDS was first seen: DPM/WDA get 20 s */
	bool bind_sent, start_pending;
	uint64_t last_try;              /* when the bind or the session was last sent, for the retry */
	uint32_t handle;
};

/* Open the three service clients (WDS, WDA, DPM) on the transport and look their
 * services up. 0, or an errno. The config fields above must be filled in first. */
int redwood_wds_client_open(struct redwood_wds_client *client, const struct redwood_qrtr_ops *transport,
    const struct redwood_wds_ops *ops, void *context,
    uint64_t (*now_us)(void *context), void *clock_context);
void redwood_wds_client_close(struct redwood_wds_client *client);
/* NAS registration changed; the session starts once registered, retries while refused. */
void redwood_wds_set_registered(struct redwood_wds_client *client, bool registered);
/* The IPA driver fills the endpoints in; if the DPM service is already known the
 * DPM request goes out at once. 0 = not configured: DPM is not sent. */
void redwood_wds_set_endpoints(struct redwood_wds_client *client, uint32_t to_modem, uint32_t from_modem);
/* The APN for Start Network; empty (or NULL) means the SIM's default profile. */
void redwood_wds_set_apn(struct redwood_wds_client *client, const char *apn);
/* Advance the state machine (bind, start, retry). Called about once a second by
 * the modem bring-up thread, and from the NAS and reply paths. */
void redwood_wds_tick(struct redwood_wds_client *client);
bool redwood_wds_up(struct redwood_wds_client *client);

#endif
