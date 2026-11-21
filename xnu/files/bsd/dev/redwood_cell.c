/*
 * en1: the modem's packets as a network interface Darwin will use.
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.cell.c); renamed hakios_* to
 * redwood_*, the MACs to redwood's own pair, the net-layer calls to the frame engine below
 * (the replies are returned, not handed to a unit input: our net layer takes one link).
 * The data plane (IPA transmit/poll/status/data_ready) is behind struct redwood_cell_data_ops,
 * NULL until the chip layer fills it in. No Blair-specific addresses or registers here.
 */
#include <sys/redwood_cell.h>
#include <sys/systm.h>

static const uint8_t cell_mac[6] = { 0x02, 0x72, 0x65, 0x64, 0x77, 0x01 };      /* locally administered: "redw", 1 */
static const uint8_t router_mac[6] = { 0x02, 0x72, 0x65, 0x64, 0x77, 0xfe };

static const struct redwood_cell_data_ops *data_ops;
static bool cell_attached, cell_trace;
static struct {
	bool            up;
	uint32_t        address, mask, router, dns1, dns2, mtu;        /* host order; what DHCP gives out */
	uint32_t        modem_mask, modem_gateway;                      /* what the modem said, for the log */
} session;
static struct redwood_cell_counts counts;

static inline uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static inline void
put16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static uint16_t
checksum(const uint8_t *data, size_t length)
{
	uint32_t sum = 0;

	for (size_t i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t)data[i] << 8 | data[i + 1];
	}
	if (length & 1) {
		sum += (uint32_t)data[length - 1] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

/* ---- ARP: the router answers for everything ---------------------------------------------------- */

static bool
arp_answer(const uint8_t *frame, size_t length, uint8_t *reply, size_t *reply_len)
{
	const uint8_t *arp = frame + 14;

	/* Ethernet/IPv4, a request, not for our own address */
	if (length < 42 || get32(arp) != 0x00010800 || arp[4] != 6 || arp[5] != 4 || arp[7] != 1 || get32(arp + 24) == session.address) {
		return false;
	}
	memcpy(reply, frame + 6, 6);                    /* to the asker */
	memcpy(reply + 6, router_mac, 6);
	put16(reply + 12, 0x0806);
	put32(reply + 14, 0x00010800);
	reply[18] = 6; reply[19] = 4;
	put16(reply + 20, 2);                           /* a reply */
	memcpy(reply + 22, router_mac, 6);              /* "that address is me" */
	memcpy(reply + 28, arp + 24, 4);
	memcpy(reply + 32, arp + 8, 10);                /* the asker's addresses */
	counts.arp++;
	if (cell_trace) {
		printf("cell: ARP: %u.%u.%u.%u is the router\n", arp[24], arp[25], arp[26], arp[27]);
	}
	*reply_len = 42;
	return true;
}

/* ---- DHCP: the session's settings ---------------------------------------------------------------- */

#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_ACK                5
#define DHCP_NAK                6

/* Is this a DHCP request (UDP 68 -> 67)? `udp` is set to its UDP header. */
static bool
dhcp_request(const uint8_t *frame, size_t length, const uint8_t **udp)
{
	const uint8_t *ip = frame + 14;
	size_t ihl;

	if (length < 14 + 20 + 8 + 240 || (ip[0] >> 4) != 4 || ip[9] != 17) {
		return false;
	}
	ihl = 4 * (ip[0] & 0xf);
	if (ihl < 20 || 14 + ihl + 8 + 240 > length) {
		return false;
	}
	*udp = ip + ihl;
	return ((*udp)[2] << 8 | (*udp)[3]) == 67;
}

/* The answer to a DHCP request: an offer for a discover, an acknowledgement for a request, with the session's settings. */
static bool
dhcp_answer(const uint8_t *frame, size_t length, const uint8_t *udp, uint8_t *reply, size_t *reply_len)
{
	const uint8_t *request = udp + 8, *end = frame + length, *option;
	uint8_t *ip = reply + 14, *rudp = ip + 20, *bootp = rudp + 8, *o;
	uint32_t type = 0, wanted = 0, kind;
	size_t size;

	if (request[0] != 1 || request[1] != 1 || request[2] != 6 || get32(request + 236) != 0x63825363) {
		return false;
	}
	/* the options: the message type, the address asked for */
	for (option = request + 240; option + 2 <= end && *option != 255; option += option[0] == 0 ? 1 : 2 + option[1]) {
		if (option[0] == 0) {
			continue;
		}
		if (option + 2 + option[1] > end) {
			break;
		}
		if (option[0] == 53 && option[1] == 1) {
			type = option[2];
		} else if (option[0] == 50 && option[1] == 4) {
			wanted = get32(option + 2);
		}
	}
	if (type != DHCP_DISCOVER && type != DHCP_REQUEST) {
		return false;
	}
	kind = type == DHCP_DISCOVER ? DHCP_OFFER : wanted != 0 && wanted != session.address ? DHCP_NAK : DHCP_ACK;
	memset(reply, 0, 14 + 20 + 8 + 240 + 64);
	/* BOOTP */
	bootp[0] = 2; bootp[1] = 1; bootp[2] = 6;      /* a reply, Ethernet */
	memcpy(bootp + 4, request + 4, 4);              /* the transaction */
	memcpy(bootp + 10, request + 10, 2);            /* the flags (broadcast) */
	if (kind != DHCP_NAK) {
		put32(bootp + 16, session.address);     /* yiaddr */
		put32(bootp + 20, session.router);      /* siaddr */
	}
	memcpy(bootp + 28, request + 28, 16);           /* the client's hardware address */
	put32(bootp + 236, 0x63825363);
	o = bootp + 240;
	*o++ = 53; *o++ = 1; *o++ = (uint8_t)kind;
	*o++ = 54; *o++ = 4; put32(o, session.router); o += 4;      /* the server: the router */
	if (kind != DHCP_NAK) {
		*o++ = 51; *o++ = 4; put32(o, 86400); o += 4;           /* a day's lease */
		*o++ = 1; *o++ = 4; put32(o, session.mask); o += 4;
		*o++ = 3; *o++ = 4; put32(o, session.router); o += 4;
		if (session.dns1 != 0 || session.dns2 != 0) {
			*o++ = 6; *o++ = (uint8_t)(4 * ((session.dns1 != 0) + (session.dns2 != 0)));
			if (session.dns1 != 0) { put32(o, session.dns1); o += 4; }
			if (session.dns2 != 0) { put32(o, session.dns2); o += 4; }
		}
		if (session.mtu >= 576 && session.mtu <= 1500) {
			*o++ = 26; *o++ = 2; put16(o, session.mtu); o += 2;
		}
	}
	*o++ = 255;
	size = (size_t)(o - bootp);
	if (size < 300) {
		size = 300;                             /* the traditional minimum, padded with zeros */
	}
	/* UDP 67 -> 68, no checksum */
	put16(rudp, 67); put16(rudp + 2, 68); put16(rudp + 4, (uint32_t)(8 + size)); put16(rudp + 6, 0);
	/* IPv4 from the router to everyone */
	ip[0] = 0x45; ip[1] = 0;
	put16(ip + 2, (uint32_t)(20 + 8 + size));
	put16(ip + 4, 0); put16(ip + 6, 0);
	ip[8] = 64; ip[9] = 17;
	put32(ip + 12, session.router);
	put32(ip + 16, 0xffffffff);
	put16(ip + 10, checksum(ip, 20));
	/* Ethernet to the client */
	memcpy(reply, request + 28, 6);
	memcpy(reply + 6, router_mac, 6);
	put16(reply + 12, 0x0800);
	counts.dhcp++;
	printf("cell: DHCP %s answered with %s: %u.%u.%u.%u/%u.%u.%u.%u, router %u.%u.%u.%u, DNS %u.%u.%u.%u %u.%u.%u.%u, MTU %u\n",
	    type == DHCP_DISCOVER ? "discover" : "request", kind == DHCP_OFFER ? "an offer" : kind == DHCP_ACK ? "an acknowledgement" : "a refusal",
	    session.address >> 24, (session.address >> 16) & 0xff, (session.address >> 8) & 0xff, session.address & 0xff,
	    session.mask >> 24, (session.mask >> 16) & 0xff, (session.mask >> 8) & 0xff, session.mask & 0xff,
	    session.router >> 24, (session.router >> 16) & 0xff, (session.router >> 8) & 0xff, session.router & 0xff,
	    session.dns1 >> 24, (session.dns1 >> 16) & 0xff, (session.dns1 >> 8) & 0xff, session.dns1 & 0xff,
	    session.dns2 >> 24, (session.dns2 >> 16) & 0xff, (session.dns2 >> 8) & 0xff, session.dns2 & 0xff, session.mtu);
	*reply_len = 14 + 20 + 8 + size;
	return true;
}

/* ---- the frame engine ---------------------------------------------------------------------------- */

/* A frame from the stack: answered here, or its IP packet to the modem. */
int
redwood_cell_handle_out(const uint8_t *frame, size_t length, uint8_t *reply, size_t *reply_len)
{
	const uint8_t *udp;
	uint32_t type;

	if (frame == NULL || length < 14 || length > REDWOOD_CELL_FRAME_MAX) {
		counts.dropped_out++;
		return -1;
	}
	type = (uint32_t)frame[12] << 8 | frame[13];
	if (type == 0x0806) {
		return arp_answer(frame, length, reply, reply_len) ? 1 : -1;
	} else if (type == 0x0800 && dhcp_request(frame, length, &udp)) {
		if (!session.up) return -1;
		return dhcp_answer(frame, length, udp, reply, reply_len) ? 1 : -1;
	} else if (type == 0x0800) {
		bool ok = session.up && data_ops != NULL && data_ops->transmit != NULL &&
		    data_ops->transmit(REDWOOD_CELL_MUX, frame + 14, length - 14);
		if (ok) {
			counts.to_modem++;
			return 0;
		}
		counts.dropped_out++;
		return -1;
	} else {
		counts.not_ip4++;                   /* IPv6 and the rest: not carried */
		return -1;
	}
}

/* A packet from the modem: an Ethernet frame from the router made around it. */
bool
redwood_cell_handle_in(uint8_t mux, const uint8_t *packet, size_t length, uint8_t *frame, size_t *frame_len)
{
	uint32_t version = length > 0 ? packet[0] >> 4 : 0;

	if (packet == NULL || length < 20 || length + 14 > REDWOOD_CELL_FRAME_MAX || (version != 4 && version != 6)) {
		counts.dropped_in++;
		return false;
	}
	if (cell_trace) {
		printf("cell: %zu bytes from the modem on mux %u (IPv%u)\n", length, mux, version);
	}
	memcpy(frame, cell_mac, 6);
	memcpy(frame + 6, router_mac, 6);
	put16(frame + 12, version == 4 ? 0x0800 : 0x86dd);
	memcpy(frame + 14, packet, length);
	counts.from_modem++;
	*frame_len = 14 + length;
	return true;
}

/* The modem's packets, wrapped for the stack. The stack side (an en1 link) is not there yet:
 * the wrapped frames are counted, not delivered, until the net layer takes a second link. */
static void
cell_receive(uint8_t mux, const uint8_t *packet, size_t length)
{
	uint8_t frame[REDWOOD_CELL_FRAME_MAX];
	size_t frame_len = 0;

	(void)redwood_cell_handle_in(mux, packet, length, frame, &frame_len);
}

unsigned
redwood_cell_poll(void)
{
	static unsigned ticks;
	unsigned got = 0;

	if (data_ops != NULL && data_ops->poll != NULL) {
		got = data_ops->poll(cell_receive);
	}
	if (++ticks % 30000 == 0 && (counts.to_modem != 0 || counts.from_modem != 0 || counts.dropped_out != 0)) {
		char line[256];

		if (data_ops != NULL && data_ops->status != NULL) {
			data_ops->status(line, sizeof(line));
		} else {
			snprintf(line, sizeof(line), "no data plane");
		}
		printf("cell: so far %llu packets to the modem, %llu from it; dropped %llu out, %llu in; %llu not IPv4; ARP %llu, DHCP %llu; "
		    "IPA: %s\n", (unsigned long long)counts.to_modem, (unsigned long long)counts.from_modem,
		    (unsigned long long)counts.dropped_out, (unsigned long long)counts.dropped_in,
		    (unsigned long long)counts.not_ip4, (unsigned long long)counts.arp, (unsigned long long)counts.dhcp, line);
	}
	return got;
}

/* ---- the session and the attach -------------------------------------------------------------------- */

void
redwood_cell_set_data_ops(const struct redwood_cell_data_ops *ops)
{
	data_ops = ops;
}

void
redwood_cell_set_trace(bool trace)
{
	cell_trace = trace;
}

void
redwood_cell_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway, uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	uint32_t router;

	session.modem_mask = mask;
	session.modem_gateway = gateway;
	/* a /24 around the address, with the router in it (see the top of the file) */
	mask = 0xffffff00;
	router = (address & mask) | 1;
	if (router == address) {
		router = (address & mask) | 2;
	}
	session.address = address;
	session.mask = mask;
	session.router = router;
	session.dns1 = dns1;
	session.dns2 = dns2;
	session.mtu = mtu;
	session.up = up && address != 0;
	printf("cell: the data session is %s: %u.%u.%u.%u (the modem's mask %u.%u.%u.%u and gateway %u.%u.%u.%u; en1 is told /24 and router "
	    "%u.%u.%u.%u), DNS %u.%u.%u.%u %u.%u.%u.%u, MTU %u%s\n", session.up ? "UP" : "down",
	    address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff,
	    session.modem_mask >> 24, (session.modem_mask >> 16) & 0xff, (session.modem_mask >> 8) & 0xff, session.modem_mask & 0xff,
	    gateway >> 24, (gateway >> 16) & 0xff, (gateway >> 8) & 0xff, gateway & 0xff,
	    router >> 24, (router >> 16) & 0xff, (router >> 8) & 0xff, router & 0xff,
	    dns1 >> 24, (dns1 >> 16) & 0xff, (dns1 >> 8) & 0xff, dns1 & 0xff, dns2 >> 24, (dns2 >> 16) & 0xff, (dns2 >> 8) & 0xff, dns2 & 0xff,
	    mtu, cell_attached ? "" : " (no en1: the net layer takes one link)");
	/* no link-state call here: there is no en1 to tell yet (see redwood_cell_attach). */
}

bool
redwood_cell_up(void)
{
	return session.up;
}

bool
redwood_cell_attach(void)
{
	if (data_ops == NULL || data_ops->ready == NULL || !data_ops->ready()) {
		printf("cell: no en1: the IPA data path is not up\n");
		return false;
	}
	/* The net layer (sys/redwood_net.h) takes one link, once (en0); there is no second
	 * interface to attach this link to. The en1 attach is the multi-link step, not this one. */
	printf("cell: no en1: the net layer takes one link (en0); the multi-link step is still open\n");
	return false;
}

void
redwood_cell_counts(struct redwood_cell_counts *c)
{
	if (c != NULL) *c = counts;
}

/* ---- the WDS client's callbacks ---- */

void
redwood_cell_wds_settings(void *context, const struct redwood_wds_settings *settings)
{
	(void)context;
	if (settings == NULL) return;
	redwood_cell_session(true, settings->address, settings->mask, settings->gateway,
	    settings->dns1, settings->dns2, settings->mtu);
}

void
redwood_cell_wds_down(void *context)
{
	(void)context;
	redwood_cell_session(false, 0, 0, 0, 0, 0, 0);
}

const struct redwood_wds_ops redwood_cell_wds_ops = {
	.settings = redwood_cell_wds_settings,
	.down = redwood_cell_wds_down,
};
