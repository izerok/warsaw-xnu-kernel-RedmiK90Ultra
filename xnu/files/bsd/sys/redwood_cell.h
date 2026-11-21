/* en1: the modem's packets as a network interface Darwin will use.
 *
 * Darwin's own cellular interface (pdp_ip0) is made by its baseband stack, which is not here. What
 * is here is the modem's IP packets (QMI WDS, QMAP mux 1) shown as Ethernet frames between two
 * made-up stations, the interface's and "the router's", and this file answers, in the kernel,
 * what the stack asks of the router: ARP for any address (everything is behind the router) and
 * DHCP with the data session's settings (the address and DNS servers the modem got from the
 * network, QMI WDS). Packets to the router go to the modem; packets from the modem come from
 * the router. IPv6 is not carried (the session is IPv4).
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.cell.c, sys/hakios.ipa.h):
 * the ARP/DHCP logic is carried over as-is; the MACs are redwood's own locally administered
 * pair (02:72:65:64:77:01/.fe, "redw"), not malmo's "haki" pair.
 *
 * What is NOT here yet: the en1 link itself. Our net layer (sys/redwood_net.h) takes one link,
 * once (en0); there is no unit parameter and no second interface to attach to. So
 * redwood_cell_attach() reports that and stays false until the net layer grows a second unit;
 * the frame engine below is complete and host-tested, and the data plane (the IPA driver's
 * transmit/poll/status/data_ready) sits behind struct redwood_cell_data_ops, NULL until the
 * chip layer fills it in. No locking yet: like the NAS/WDS clients, the state here belongs to
 * the link thread (the bring-up thread owns the session; the transport serializes the callbacks).
 *
 * The mask and the router given to DHCP are not the modem's: a cellular session is a point-to-point
 * link, and its "gateway" need not be in any subnet with the address, which DHCP cannot express.
 * The interface gets a /24 around its address with the router at its .1 (or .2): any router will
 * do, since every packet goes to the modem anyway.
 */
#ifndef _SYS_REDWOOD_CELL_H_
#define _SYS_REDWOOD_CELL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/redwood_wds.h>

#define REDWOOD_CELL_MUX        1               /* the QMAP mux id the session is bound to (WDS Bind Mux Data Port) */
#define REDWOOD_CELL_FRAME_MAX  1514            /* == REDWOOD_NET_FRAME_MAX; the shared one lives under #ifdef KERNEL */

/* The data plane, supplied by the IPA driver (chip layer). All NULL until it exists. */
struct redwood_cell_data_ops {
	/* The IPA data path is up. */
	bool (*ready)(void);
	/* One IP packet to the modem on QMAP mux `mux`. Copied. False if dropped. */
	bool (*transmit)(uint8_t mux, const void *packet, size_t length);
	/* Packets from the modem: `receive` is called for each, with its mux id. Returns how many. */
	unsigned (*poll)(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length));
	/* What has gone through, for the log. */
	void (*status)(char *to, size_t size);
};

void redwood_cell_set_data_ops(const struct redwood_cell_data_ops *ops);
/* Make en1 on the data path. False until the net layer takes a second link (see above);
 * the log says why. The data plane must be ready first. */
bool redwood_cell_attach(void);
/* Called by the modem driver: the data session is up with these settings (as the WDS client
 * reports them), or down. */
void redwood_cell_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway, uint32_t dns1, uint32_t dns2, uint32_t mtu);
bool redwood_cell_up(void);
void redwood_cell_set_trace(bool trace);

/* The WDS client's callbacks, for the bring-up thread to pass to redwood_wds_client_open(). */
void redwood_cell_wds_settings(void *context, const struct redwood_wds_settings *settings);
void redwood_cell_wds_down(void *context);
extern const struct redwood_wds_ops redwood_cell_wds_ops;

/* ---- the frame engine (host-testable, no mbufs, no KPI) ---- */

/* A frame from the stack. 1: a reply frame is in `reply` (deliver it to the stack);
 * 0: the IP packet went to the modem; -1: dropped. `reply` needs REDWOOD_CELL_FRAME_MAX bytes. */
int redwood_cell_handle_out(const uint8_t *frame, size_t length, uint8_t *reply, size_t *reply_len);
/* A packet from the modem: an Ethernet frame from the router made around it. False: dropped.
 * `frame` needs REDWOOD_CELL_FRAME_MAX bytes. */
bool redwood_cell_handle_in(uint8_t mux, const uint8_t *packet, size_t length, uint8_t *frame, size_t *frame_len);
/* Look whether packets have arrived (the IPA driver's poll). Returns how many there were. */
unsigned redwood_cell_poll(void);

struct redwood_cell_counts {
	uint64_t arp, dhcp, to_modem, from_modem, dropped_out, dropped_in, not_ip4;
};
void redwood_cell_counts(struct redwood_cell_counts *counts);

#endif
