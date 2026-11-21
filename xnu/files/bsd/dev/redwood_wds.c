/*
 * QMI WDS data session: DPM -> WDA -> Bind Mux -> Set IP Family -> Start Network.
 *
 * Ported from pixelpixys/hakios-malmo (drivers/modem/hakios.mpss.c, the data-session
 * section). The request bytes, the service/message numbers and the retry rules are
 * carried over; the modem bring-up (qrtr_open/qrtr_send/qrtr_lookup/qrtr_close,
 * tell(), option(), now_us(), PE_parse_boot_argn, hakios_cell_session,
 * hakios_ipa_modem_endpoints, the cellular_* sysctl globals) is replaced by the
 * transport ops, the client's config fields and the settings/down callbacks.
 * No Blair-specific addresses or registers in this file.
 */
#include <sys/redwood_wds.h>
#include <sys/systm.h>
#include <sys/redwood_qrtr.h>

#define QMI_WDS                 0x01
#define WDS_START_NETWORK_INTERFACE 0x0020
#define WDS_GET_CURRENT_SETTINGS 0x002d
#define WDS_BIND_MUX_DATA_PORT  0x00a2
#define WDS_SET_IP_FAMILY       0x004d
#define QMI_WDA                 0x1a
#define WDA_SET_DATA_FORMAT     0x0020
#define QMI_DPM                 0x2f            /* Data Port Mapper */
#define DPM_OPEN_PORT           0x0020
#define DATA_ENDPOINT_EMBEDDED  4               /* the IPA, "interface" 1: where the packets of this session go */
#define DATA_ENDPOINT_INTERFACE 1
#define DATA_MUX                1               /* the QMAP mux id (redwood_cell.c carries mux 1) */

#define WDS_RETRY_US            6000000ull      /* a refused Start is retried every 6 s */
#define WDS_SETUP_WINDOW_US     20000000ull     /* DPM/WDA get 20 s before the bind goes ahead */

static void wds_advance(struct redwood_wds_client *client);
static void wds_send_start(struct redwood_wds_client *client);
static void wda_send_format(struct redwood_wds_client *client);
static void dpm_send_open(struct redwood_wds_client *client);

static uint64_t
now(struct redwood_wds_client *client)
{
	return client->now_us != NULL ? client->now_us(client->clock_context) : 0;
}

/* The result/error TLV (type 2) of a reply, and the first 64 bytes for the log. */
static void
probe_dump(const char *who, const uint8_t *m, size_t length)
{
	uint32_t result = ~0u, error = ~0u;

	for (size_t at = 7; at + 3 <= length;) {
		uint16_t size = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (m[at] == 2 && size == 4 && at + 7 <= length) {
			result = (uint32_t)(m[at + 3] | m[at + 4] << 8);
			error = (uint32_t)(m[at + 5] | m[at + 6] << 8);
		}
		at += 3 + size;
	}
	printf("wds: %s: < %s %#x, transaction %u, %u bytes: result %u, error %u;", who, m[0] == 2 ? "answer" : m[0] == 4 ? "indication" : "?",
	    m[3] | m[4] << 8, m[1] | m[2] << 8, (unsigned)length, result, error);
	for (size_t i = 0; i < length && i < 64; i++) {
		printf(" %02x", m[i]);
	}
	printf("%s\n", length > 64 ? " ..." : "");
}

/* The value of a TLV of the given type in a reply, or NULL. */
static const uint8_t *
wds_tlv(const uint8_t *m, size_t length, uint8_t type, uint16_t *size)
{
	for (size_t at = 7; at + 3 <= length;) {
		uint16_t s = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (at + 3 + s > length) return NULL;
		if (m[at] == type) {
			*size = s;
			return m + at + 3;
		}
		at += 3 + s;
	}
	return NULL;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
ip4_string(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff);
}

/* Is the DPM step needed (endpoints configured) and not switched off? */
static bool
dpm_needed(struct redwood_wds_client *client)
{
	return !client->dpm_off && client->to_modem != 0 && client->from_modem != 0;
}

/*
 * Before the session, the modem is told where its data goes and how it is framed, in the order
 * ModemManager follows for a modem behind the IPA (its port for the "ipa" network driver):
 *
 *   1. DPM Open Port (service 0x2f, 0x20): the hardware data port -- the embedded endpoint,
 *      interface 1, and the IPA endpoints it runs on. The modem's receiving end is our transmit
 *      endpoint and its transmitting end our receive endpoint. Without this the modem knows no
 *      embedded port: WDA answers InvalidOperation, the mux bind Internal, the session
 *      InvalidOperation.
 *   2. WDA Set Data Format (0x1a, 0x20) on that port: raw IP in QMAP (v1: no checksum headers),
 *      at most 16 packets or 14 KB per downlink aggregate.
 *   3. WDS Bind Mux Data Port, Set IP Family, Start Network Interface: wds_advance() below.
 *
 * Each step goes on when the one before has been answered, or after 20 s without an answer.
 */
static void
wda_send_format(struct redwood_wds_client *client)
{
	/* raw IP, QMAP both ways, downlink at most 16 packets or 14 KB per aggregate, on the embedded port */
	uint8_t request[7 + 7 + 7 + 7 + 7 + 7 + 11] = { 0, 0x43, 0, WDA_SET_DATA_FORMAT & 0xff, WDA_SET_DATA_FORMAT >> 8, 0, 0,
		0x11, 4, 0, 2, 0, 0, 0,
		0x12, 4, 0, 5, 0, 0, 0,
		0x13, 4, 0, 5, 0, 0, 0,
		0x15, 4, 0, 16, 0, 0, 0,
		0x16, 4, 0, 0x00, 0x38, 0, 0,
		0x17, 8, 0, DATA_ENDPOINT_EMBEDDED, 0, 0, 0, DATA_ENDPOINT_INTERFACE, 0, 0, 0 };

	if (client->wda_sent || client->wda_port == NULL || client->wda_port_number == 0) {
		return;
	}
	client->wda_sent = true;
	request[5] = sizeof(request) - 7;
	printf("wds: wda: about to set the data format of the embedded port: raw IP in QMAP, no checksum offload\n");
	client->transport->send(client->wda_port, client->wda_node, client->wda_port_number, request, sizeof(request));
}

static void
wda_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct redwood_wds_client *client = context;
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;
	uint32_t result = ~0u, error = ~0u, link = 0, ul = 0, dl = 0;

	(void)node;
	(void)port;
	if (length < 7 || m[0] != 2 || (uint16_t)(m[3] | m[4] << 8) != WDA_SET_DATA_FORMAT) {
		return;
	}
	probe_dump("wda", m, length);
	if ((v = wds_tlv(m, length, 2, &size)) != NULL && size >= 4) {
		result = (uint32_t)(v[0] | v[1] << 8);
		error = (uint32_t)(v[2] | v[3] << 8);
	}
	if ((v = wds_tlv(m, length, 0x11, &size)) != NULL && size >= 4) { link = le32(v); }
	if ((v = wds_tlv(m, length, 0x12, &size)) != NULL && size >= 4) { ul = le32(v); }
	if ((v = wds_tlv(m, length, 0x13, &size)) != NULL && size >= 4) { dl = le32(v); }
	client->wda_set = result == 0;
	client->wda_done = true;
	printf("wds: wda: THE DATA FORMAT %s: result %u error %u; link layer %u (2: raw IP), aggregation up %u down %u (5: QMAP)\n",
	    result == 0 ? "IS SET" : "IS REFUSED", result, error, link, ul, dl);
	wds_advance(client);
}

static void
wda_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node,
    uint32_t port)
{
	struct redwood_wds_client *client = context;
	(void)version;
	(void)instance;
	if (service != QMI_WDA) {
		return;
	}
	if (arrived) {
		client->wda_node = node;
		client->wda_port_number = port;
		if (client->dpm_done || !dpm_needed(client)) {
			wda_send_format(client);
		}
	} else if (node == client->wda_node && port == client->wda_port_number) {
		client->wda_node = client->wda_port_number = 0;
	}
}

static void
dpm_send_open(struct redwood_wds_client *client)
{
	/* TLV 0x11, the hardware data ports: a one-byte count, then per port the endpoint type, the interface
	 * number, the modem's receiving endpoint (ours that sends) and its transmitting one (ours that receives) */
	uint8_t request[7 + 3 + 1 + 16] = { 0, 0x45, 0, DPM_OPEN_PORT & 0xff, DPM_OPEN_PORT >> 8, 20, 0, 0x11, 17, 0, 1 };
	unsigned at = 11;

	if (client->dpm_port == NULL || client->dpm_port_number == 0 || !dpm_needed(client)) {
		return;
	}
#define PUT32(x) do { request[at++] = (uint8_t)(x); request[at++] = (uint8_t)((x) >> 8); request[at++] = (uint8_t)((x) >> 16); request[at++] = (uint8_t)((x) >> 24); } while (0)
	PUT32(DATA_ENDPOINT_EMBEDDED);
	PUT32(DATA_ENDPOINT_INTERFACE);
	PUT32(client->to_modem);
	PUT32(client->from_modem);
#undef PUT32
	printf("wds: dpm: about to open the embedded data port (interface %u, the modem receives on IPA endpoint %u and sends on %u)\n",
	    DATA_ENDPOINT_INTERFACE, client->to_modem, client->from_modem);
	client->transport->send(client->dpm_port, client->dpm_node, client->dpm_port_number, request, sizeof(request));
}

static void
dpm_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct redwood_wds_client *client = context;
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;
	uint32_t result = ~0u, error = ~0u;

	(void)node;
	(void)port;
	if (length < 7 || m[0] != 2 || (uint16_t)(m[3] | m[4] << 8) != DPM_OPEN_PORT) {
		return;
	}
	probe_dump("dpm", m, length);
	if ((v = wds_tlv(m, length, 2, &size)) != NULL && size >= 4) {
		result = (uint32_t)(v[0] | v[1] << 8);
		error = (uint32_t)(v[2] | v[3] << 8);
	}
	client->dpm_open = result == 0;
	client->dpm_done = true;
	printf("wds: dpm: THE EMBEDDED DATA PORT %s: result %u error %u\n", client->dpm_open ? "IS OPEN" : "IS NOT OPENED", result, error);
	wda_send_format(client);
}

static void
dpm_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node,
    uint32_t port)
{
	struct redwood_wds_client *client = context;
	(void)version;
	(void)instance;
	if (service != QMI_DPM) {
		return;
	}
	if (arrived) {
		client->dpm_node = node;
		client->dpm_port_number = port;
		dpm_send_open(client);
	} else if (node == client->dpm_node && port == client->dpm_port_number) {
		client->dpm_node = client->dpm_port_number = 0;
	}
}

static void
wds_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct redwood_wds_client *client = context;
	const uint8_t *m = data;
	uint16_t size = 0, message;
	const uint8_t *v;

	(void)node;
	(void)port;
	if (length < 7) {
		return;
	}
	probe_dump("wds", m, length);
	message = (uint16_t)(m[3] | m[4] << 8);
	if (m[0] == 2 && message == WDS_START_NETWORK_INTERFACE) {
		uint32_t result = ~0u, error = ~0u, reason = ~0u;

		if ((v = wds_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		if ((v = wds_tlv(m, length, 0x10, &size)) != NULL && size >= 2) {
			reason = (uint32_t)(v[0] | v[1] << 8);
		}
		if (result == 0 && (v = wds_tlv(m, length, 0x01, &size)) != NULL && size >= 4) {
			uint8_t ask[14] = { 0, 0x42, 0, WDS_GET_CURRENT_SETTINGS & 0xff, WDS_GET_CURRENT_SETTINGS >> 8, 7, 0, 0x10, 4, 0,
					    0x10, 0xa3, 0, 0 };      /* DNS, the address, the gateway, the MTU, the IP family */

			client->handle = le32(v);
			client->started = true;
			printf("wds: wds: THE DATA SESSION IS STARTED (handle %#x); its settings asked for\n", client->handle);
			client->transport->send(client->wds_port, client->wds_node, client->wds_port_number, ask, sizeof(ask));
		} else {
			printf("wds: wds: the data session is refused for now: result %u, error %u%s, call end reason %u (will retry)\n",
			    result, error, error == 70 ? " (invalid operation: the packet domain is not attached yet)" : "", reason);
			client->start_pending = false;      /* retried by wds_advance() after a few seconds */
			client->last_try = now(client);
		}
	} else if (m[0] == 2 && message == WDS_BIND_MUX_DATA_PORT) {
		uint32_t result = ~0u, error = ~0u;

		if ((v = wds_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		client->bound = result == 0;
		printf("wds: wds: the client %s to mux %u of the embedded port (result %u, error %u); the session waits for registration%s\n",
		    client->bound ? "is bound" : "IS NOT BOUND", DATA_MUX, result, error, client->bound ? "" : " (started all the same)");
		{
			/* the client's IP family: IPv4 (WDS Set IP Family 0x4d), as MM does before starting */
			uint8_t family[11] = { 0, 0x46, 0, WDS_SET_IP_FAMILY & 0xff, WDS_SET_IP_FAMILY >> 8, 4, 0, 0x01, 1, 0, 4 };

			client->transport->send(client->wds_port, client->wds_node, client->wds_port_number, family, sizeof(family));
		}
		client->last_try = now(client);        /* give the session a moment after the bind */
		wds_advance(client);
	} else if (m[0] == 2 && message == WDS_SET_IP_FAMILY) {
		uint32_t result = ~0u, error = ~0u;

		if ((v = wds_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		printf("wds: wds: the IP family (IPv4) %s (result %u, error %u)\n", result == 0 ? "is set" : "is not set", result, error);
	} else if (m[0] == 2 && message == WDS_GET_CURRENT_SETTINGS) {
		uint32_t ip = 0, gw = 0, mask = 0, dns1 = 0, dns2 = 0, mtu = 0;
		char a[20], g[20], d1[20], d2[20], mk[20];
		struct redwood_wds_settings settings;

		if ((v = wds_tlv(m, length, 0x1e, &size)) != NULL && size >= 4) { ip = le32(v); }
		if ((v = wds_tlv(m, length, 0x20, &size)) != NULL && size >= 4) { gw = le32(v); }
		if ((v = wds_tlv(m, length, 0x21, &size)) != NULL && size >= 4) { mask = le32(v); }
		if ((v = wds_tlv(m, length, 0x15, &size)) != NULL && size >= 4) { dns1 = le32(v); }
		if ((v = wds_tlv(m, length, 0x16, &size)) != NULL && size >= 4) { dns2 = le32(v); }
		if ((v = wds_tlv(m, length, 0x29, &size)) != NULL && size >= 4) { mtu = le32(v); }
		ip4_string(a, sizeof(a), ip);
		ip4_string(g, sizeof(g), gw);
		ip4_string(mk, sizeof(mk), mask);
		ip4_string(d1, sizeof(d1), dns1);
		ip4_string(d2, sizeof(d2), dns2);
		printf("wds: wds: THE DATA SESSION'S SETTINGS: address %s, gateway %s, netmask %s, DNS %s %s, MTU %u\n", a, g, mk, d1, d2, mtu);
		if (ip != 0) {
			settings.handle = client->handle;
			settings.address = ip;
			settings.gateway = gw;
			settings.mask = mask;
			settings.dns1 = dns1;
			settings.dns2 = dns2;
			settings.mtu = mtu;
			client->up = true;
			if (client->ops != NULL && client->ops->settings != NULL) {
				client->ops->settings(client->context, &settings);
			}
		}
	}
}

static void
wds_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node,
    uint32_t port)
{
	struct redwood_wds_client *client = context;
	(void)version;
	(void)instance;
	if (service != QMI_WDS) {
		return;
	}
	if (arrived) {
		client->wds_node = node;
		client->wds_port_number = port;
	} else if (node == client->wds_node && port == client->wds_port_number) {
		client->wds_node = client->wds_port_number = 0;
	}
}

/*
 * The data session, step by step, idempotent and retry-aware: bind the client to the mux once,
 * then -- only once NAS is registered, and the packet domain has had a moment to attach -- start
 * the session, retrying every few seconds while it is refused (error 70 right after registration
 * means the packet domain is not attached yet). Called from set_registered(), from the bind reply,
 * and every second from the bring-up thread's tick. From the link's thread.
 */
static void
wds_advance(struct redwood_wds_client *client)
{
	uint8_t bind[7 + 11 + 4] = { 0, 0x44, 0, WDS_BIND_MUX_DATA_PORT & 0xff, WDS_BIND_MUX_DATA_PORT >> 8, 15, 0,
		0x10, 8, 0, DATA_ENDPOINT_EMBEDDED, 0, 0, 0, DATA_ENDPOINT_INTERFACE, 0, 0, 0,
		0x11, 1, 0, DATA_MUX };

	if (client->wds_port == NULL || client->wds_port_number == 0 || client->up) {
		return;
	}
	if (client->data_setup_began == 0) {
		client->data_setup_began = now(client);
	}
	/* the port mapping and the data format first (or 20 s for them); a WDA that never came is not waited for */
	if (!client->bind_sent && now(client) - client->data_setup_began < WDS_SETUP_WINDOW_US) {
		if (dpm_needed(client) && !client->dpm_done) {
			return;
		}
		if (!client->wda_off && client->wda_port_number != 0 && !client->wda_done) {
			wda_send_format(client);      /* (if DPM answered before WDA was there) */
			return;
		}
	}
	/* bind once (unless nobind), before or after registration */
	if (!client->bind_sent && !client->nobind) {
		client->bind_sent = true;
		client->last_try = now(client);
		printf("wds: wds: about to bind the client to mux %u of the embedded port (the IPA)\n", DATA_MUX);
		client->transport->send(client->wds_port, client->wds_node, client->wds_port_number, bind, sizeof(bind));
		return;
	}
	/* start the session only once registered, and retry while it is refused */
	if (client->registered && !client->start_pending && now(client) - client->last_try >= WDS_RETRY_US) {
		client->start_pending = true;
		client->last_try = now(client);
		wds_send_start(client);
	}
}

static void
wds_send_start(struct redwood_wds_client *client)
{
	uint8_t request[7 + 4 + 3 + 100];
	unsigned at = 7;

	if (client->wds_port == NULL || client->wds_port_number == 0) {
		return;
	}
	request[0] = 0; request[1] = 0x41; request[2] = 0;
	request[3] = WDS_START_NETWORK_INTERFACE & 0xff; request[4] = WDS_START_NETWORK_INTERFACE >> 8;
	request[at++] = 0x19; request[at++] = 1; request[at++] = 0; request[at++] = 4;      /* IPv4 */
	if (client->apn[0] != 0) {
		size_t n = strlen(client->apn);

		request[at++] = 0x14; request[at++] = (uint8_t)n; request[at++] = 0;
		memcpy(request + at, client->apn, n);
		at += n;
	}
	request[5] = (uint8_t)(at - 7); request[6] = (uint8_t)((at - 7) >> 8);
	printf("wds: wds: about to start the data session (IPv4, %s%s)\n", client->apn[0] ? "APN " : "the default profile", client->apn);
	client->transport->send(client->wds_port, client->wds_node, client->wds_port_number, request, at);
}

int
redwood_wds_client_open(struct redwood_wds_client *client, const struct redwood_qrtr_ops *transport,
    const struct redwood_wds_ops *ops, void *context,
    uint64_t (*now_us)(void *context), void *clock_context)
{
	if (client == NULL || transport == NULL || transport->open == NULL || transport->lookup == NULL ||
	    transport->send == NULL || transport->close == NULL || now_us == NULL) return -1;
	/* keep the caller's config fields (apn, *_off, nobind, endpoints); clear the state */
	client->wds_port = client->wda_port = client->dpm_port = NULL;
	client->wds_node = client->wds_port_number = client->wda_node = client->wda_port_number = 0;
	client->dpm_node = client->dpm_port_number = 0;
	client->registered = client->started = client->up = client->bound = client->wda_set = false;
	client->dpm_done = client->dpm_open = client->wda_sent = client->wda_done = false;
	client->data_setup_began = 0;
	client->bind_sent = client->start_pending = false;
	client->last_try = 0;
	client->handle = 0;
	client->transport = transport;
	client->ops = ops;
	client->context = context;
	client->now_us = now_us;
	client->clock_context = clock_context;
	if (!client->data_off) {
		client->wds_port = transport->open(wds_receive, wds_server, client);
		if (client->wds_port == NULL) return -1;
		if (transport->lookup(client->wds_port, QMI_WDS, 0, 0) != 0) {
			redwood_wds_client_close(client);
			return -1;
		}
	}
	if (!client->dpm_off && !client->data_off) {
		client->dpm_port = transport->open(dpm_receive, dpm_server, client);
		if (client->dpm_port == NULL) {
			redwood_wds_client_close(client);
			return -1;
		}
		if (transport->lookup(client->dpm_port, QMI_DPM, 0, 0) != 0) {
			redwood_wds_client_close(client);
			return -1;
		}
	}
	if (!client->wda_off && !client->data_off) {
		client->wda_port = transport->open(wda_receive, wda_server, client);
		if (client->wda_port == NULL) {
			redwood_wds_client_close(client);
			return -1;
		}
		if (transport->lookup(client->wda_port, QMI_WDA, 0, 0) != 0) {
			redwood_wds_client_close(client);
			return -1;
		}
	}
	return 0;
}

void
redwood_wds_client_close(struct redwood_wds_client *client)
{
	bool was_up;

	if (client == NULL) return;
	was_up = client->up;
	if (client->wds_port != NULL) {
		client->transport->close(client->wds_port);
		client->wds_port = NULL;
	}
	if (client->wda_port != NULL) {
		client->transport->close(client->wda_port);
		client->wda_port = NULL;
	}
	if (client->dpm_port != NULL) {
		client->transport->close(client->dpm_port);
		client->dpm_port = NULL;
	}
	client->wds_node = client->wds_port_number = client->wda_node = client->wda_port_number = 0;
	client->dpm_node = client->dpm_port_number = 0;
	client->dpm_done = client->dpm_open = client->wda_sent = client->wda_done = false;
	client->data_setup_began = 0;
	client->started = client->up = client->bound = client->wda_set = false;
	client->bind_sent = client->start_pending = client->registered = false;
	client->last_try = 0;
	client->handle = 0;
	if (was_up && client->ops != NULL && client->ops->down != NULL) {
		client->ops->down(client->context);
	}
}

void
redwood_wds_set_registered(struct redwood_wds_client *client, bool registered)
{
	if (client == NULL) return;
	client->registered = registered;
	wds_advance(client);          /* bind once, start the session once registered, retry if refused */
}

void
redwood_wds_set_endpoints(struct redwood_wds_client *client, uint32_t to_modem, uint32_t from_modem)
{
	if (client == NULL) return;
	client->to_modem = to_modem;
	client->from_modem = from_modem;
	if (client->dpm_port_number != 0) {
		dpm_send_open(client);    /* the DPM service is already known: open the port now */
	}
}

void
redwood_wds_set_apn(struct redwood_wds_client *client, const char *apn)
{
	if (client == NULL) return;
	if (apn == NULL) {
		client->apn[0] = 0;
		return;
	}
	snprintf(client->apn, sizeof(client->apn), "%s", apn);
}

void
redwood_wds_tick(struct redwood_wds_client *client)
{
	if (client == NULL) return;
	wds_advance(client);
}

bool
redwood_wds_up(struct redwood_wds_client *client)
{
	return client != NULL && client->up;
}
