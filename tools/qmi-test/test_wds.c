#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/redwood_wds.h>
#include <sys/redwood_qrtr.h>

/* A fake QRTR transport: three ports (WDS 7:9, WDA 7:10, DPM 7:11). lookup() answers at
 * once with the service; send() records the last frame per port for the test to check. */
#define PORT_WDS 0
#define PORT_WDA 1
#define PORT_DPM 2
static const uint32_t port_number[3] = { 9, 10, 11 };
static const uint32_t port_service[3] = { 1, 0x1a, 0x2f };

static redwood_qrtr_receive_t receive_cb[3];
static redwood_qrtr_server_t server_cb[3];
static void *port_ctx[3];
static struct redwood_qrtr_port *opened[3];
static int opened_count;
/* open order is WDS, DPM, WDA; the service index is PORT_WDS/WDA/DPM. Both directions mapped. */
static int opened_which[3];              /* open index -> service index */
static uint32_t opened_peer[3];          /* open index -> the service's port number */
static int which_to_open[3];             /* service index -> open index, filled by lookup() */
static uint8_t sent_frame[3][256];       /* per service index */
static size_t sent_len[3];
static int sent_count[3];
static int lookups, closes;

static int port_index(struct redwood_qrtr_port *port)
{
    for (int i = 0; i < opened_count; i++) {
        if (opened[i] == port) return i;
    }
    return -1;
}

static struct redwood_qrtr_port *open_port(redwood_qrtr_receive_t receive, redwood_qrtr_server_t on_server, void *context)
{
    int i = opened_count;
    assert(i < 3);
    receive_cb[i] = receive; server_cb[i] = on_server; port_ctx[i] = context;
    opened_which[i] = -1; opened_peer[i] = 0;
    opened[i] = (struct redwood_qrtr_port *)(uintptr_t)(i + 1);
    opened_count++;
    return opened[i];
}
static int send_packet(struct redwood_qrtr_port *port, uint32_t node, uint32_t peer, const void *data, size_t length)
{
    int i = port_index(port);
    int w;
    assert(i >= 0 && node == 7 && peer == opened_peer[i] && length <= sizeof(sent_frame[0]));
    w = opened_which[i];
    assert(w >= 0);
    memcpy(sent_frame[w], data, length);
    sent_len[w] = length;
    sent_count[w]++;
    return 0;
}
static int lookup_port(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
    int i = port_index(port);
    assert(i >= 0 && version == 0 && instance == 0);
    /* the service asked for on this port decides which one it is */
    int which = service == 1 ? PORT_WDS : service == 0x1a ? PORT_WDA : service == 0x2f ? PORT_DPM : -1;
    assert(which >= 0 && port_service[which] == service);
    opened_which[i] = which;
    opened_peer[i] = port_number[which];
    which_to_open[which] = i;
    lookups++;
    server_cb[i](port_ctx[i], true, service, 1, 0, 7, port_number[which]);
    return 0;
}
static void close_port(struct redwood_qrtr_port *port)
{
    assert(port_index(port) >= 0);
    closes++;
}
static const struct redwood_qrtr_ops transport = {
    .open = open_port, .send = send_packet, .lookup = lookup_port, .close = close_port
};

/* A controllable clock, starting at 1 s (the 0 value means "not started" inside). */
static uint64_t fake_now;
static uint64_t fake_clock(void *context) { (void)context; return fake_now; }

static struct redwood_wds_settings last_settings;
static int settings_calls, down_calls;
static void on_settings(void *context, const struct redwood_wds_settings *settings)
{
    assert(context && settings);
    last_settings = *settings;
    settings_calls++;
}
static void on_down(void *context) { assert(context); down_calls++; }
static const struct redwood_wds_ops ops = { .settings = on_settings, .down = on_down };

/* ---- reply builders (little-endian QMI wire format) ---- */
static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return p + 4;
}
static uint8_t *head(uint8_t *p, uint8_t txn, uint16_t msg)
{
    p[0] = 2; p[1] = txn; p[2] = 0; p[3] = (uint8_t)msg; p[4] = (uint8_t)(msg >> 8); p[5] = 0; p[6] = 0;
    return p + 7;
}
static uint8_t *tlv(uint8_t *p, uint8_t type, const void *v, uint16_t n)
{
    p[0] = type; p[1] = (uint8_t)n; p[2] = (uint8_t)(n >> 8);
    memcpy(p + 3, v, n);
    return p + 3 + n;
}
static uint8_t *tlv_result(uint8_t *p, uint16_t result, uint16_t error)
{
    uint8_t v[4];
    put32(v, (uint32_t)result | (uint32_t)error << 16);
    return tlv(p, 2, v, 4);
}
static size_t finish(uint8_t *frame, uint8_t *end)
{
    size_t payload = (size_t)(end - frame - 7);
    frame[5] = (uint8_t)payload; frame[6] = (uint8_t)(payload >> 8);
    return (size_t)(end - frame);
}
static void inject(int which, const void *frame, size_t length)
{
    int i = which_to_open[which];
    assert(i >= 0 && receive_cb[i]);
    receive_cb[i](port_ctx[i], 7, port_number[which], frame, length);
}
static void reply_ok(int which, uint8_t txn, uint16_t msg)
{
    uint8_t frame[64], *p = head(frame, txn, msg);
    p = tlv_result(p, 0, 0);
    inject(which, frame, finish(frame, p));
}

/* Drive a client to "bound": DPM and WDA answered, bind sent and answered. */
static void drive_to_bound(struct redwood_wds_client *client)
{
    reply_ok(PORT_DPM, 0x45, 0x0020);
    assert(sent_count[PORT_WDA] == 1);
    {   /* WDA answered with its link/aggregation values, not just the result */
        uint8_t frame[64], *p = head(frame, 0x43, 0x0020), v[4];
        p = tlv_result(p, 0, 0);
        put32(v, 2); p = tlv(p, 0x11, v, 4);
        put32(v, 5); p = tlv(p, 0x12, v, 4);
        put32(v, 5); p = tlv(p, 0x13, v, 4);
        inject(PORT_WDA, frame, finish(frame, p));
    }
    assert(client->wda_set);
    redwood_wds_tick(client);
    assert(sent_count[PORT_WDS] == 1);      /* the bind (checked here: the bind reply overwrites it with Set IP Family) */
    assert(sent_frame[PORT_WDS][1] == 0x44 && sent_frame[PORT_WDS][3] == 0xa2);
    assert(sent_frame[PORT_WDS][7] == 0x10 && sent_frame[PORT_WDS][10] == 4 && sent_frame[PORT_WDS][14] == 1);
    assert(sent_frame[PORT_WDS][18] == 0x11 && sent_frame[PORT_WDS][21] == 1);
    reply_ok(PORT_WDS, 0x44, 0x00a2);
    assert(sent_count[PORT_WDS] == 2);      /* Set IP Family */
    reply_ok(PORT_WDS, 0x46, 0x004d);
}

static void reset_fake(void)
{
    memset(receive_cb, 0, sizeof(receive_cb));
    memset(server_cb, 0, sizeof(server_cb));
    memset(port_ctx, 0, sizeof(port_ctx));
    memset(opened, 0, sizeof(opened));
    opened_count = 0;
    memset(opened_which, 0, sizeof(opened_which));
    memset(opened_peer, 0, sizeof(opened_peer));
    memset(which_to_open, 0xff, sizeof(which_to_open));   /* -1: no lookup yet */
    memset(sent_frame, 0, sizeof(sent_frame));
    memset(sent_len, 0, sizeof(sent_len));
    memset(sent_count, 0, sizeof(sent_count));
    lookups = closes = 0;
    settings_calls = down_calls = 0;
    memset(&last_settings, 0, sizeof(last_settings));
}

int main(void)
{
    struct redwood_wds_client client;
    uint8_t frame[256], *p;

    /* ---- the full order: DPM -> WDA -> Bind -> Set IP Family -> Start -> Settings ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));
    client.to_modem = 2; client.from_modem = 16;
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    assert(opened_count == 3 && lookups == 3);
    /* DPM went out at open (the service was already known); check its bytes */
    assert(sent_count[PORT_DPM] == 1 && sent_len[PORT_DPM] == 27);
    assert(sent_frame[PORT_DPM][0] == 0 && sent_frame[PORT_DPM][1] == 0x45);
    assert(sent_frame[PORT_DPM][3] == 0x20 && sent_frame[PORT_DPM][4] == 0);
    assert(sent_frame[PORT_DPM][7] == 0x11 && sent_frame[PORT_DPM][8] == 17 && sent_frame[PORT_DPM][10] == 1);
    assert(sent_frame[PORT_DPM][11] == 4 && sent_frame[PORT_DPM][15] == 1);   /* embedded, interface 1 */
    assert(sent_frame[PORT_DPM][19] == 2 && sent_frame[PORT_DPM][23] == 16);  /* to_modem 2, from_modem 16 */
    assert(sent_count[PORT_WDA] == 0);      /* WDA waits for DPM */
    drive_to_bound(&client);
    /* WDA request bytes: raw IP, QMAP both ways, 16 packets / 14 KB downlink */
    assert(sent_frame[PORT_WDA][1] == 0x43);
    assert(sent_frame[PORT_WDA][7] == 0x11 && sent_frame[PORT_WDA][10] == 2);
    assert(sent_frame[PORT_WDA][14] == 0x12 && sent_frame[PORT_WDA][17] == 5);
    assert(sent_frame[PORT_WDA][21] == 0x13 && sent_frame[PORT_WDA][24] == 5);
    assert(sent_frame[PORT_WDA][28] == 0x15 && sent_frame[PORT_WDA][31] == 16);
    assert(sent_frame[PORT_WDA][35] == 0x16 && sent_frame[PORT_WDA][38] == 0x00 && sent_frame[PORT_WDA][39] == 0x38);
    assert(sent_frame[PORT_WDA][42] == 0x17 && sent_frame[PORT_WDA][45] == 4 && sent_frame[PORT_WDA][49] == 1);
    /* not registered yet: no Start */
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 2);
    redwood_wds_set_registered(&client, true);
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 2);      /* 6 s have not passed since the bind */
    fake_now += 6000000;
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 3);      /* Start Network, default profile, no APN TLV */
    assert(sent_frame[PORT_WDS][1] == 0x41 && sent_frame[PORT_WDS][3] == 0x20);
    assert(sent_frame[PORT_WDS][7] == 0x19 && sent_frame[PORT_WDS][10] == 4);
    assert(sent_len[PORT_WDS] == 11);
    /* Start answered with a handle: Get Current Settings goes out */
    p = head(frame, 0x41, 0x0020);
    p = tlv_result(p, 0, 0);
    {
        uint8_t h[4];
        put32(h, 0x12345678);
        p = tlv(p, 1, h, 4);
    }
    inject(PORT_WDS, frame, finish(frame, p));
    assert(client.started && client.handle == 0x12345678);
    assert(sent_count[PORT_WDS] == 4);
    assert(sent_frame[PORT_WDS][1] == 0x42 && sent_frame[PORT_WDS][3] == 0x2d);
    assert(sent_frame[PORT_WDS][7] == 0x10 && sent_frame[PORT_WDS][10] == 0x10 && sent_frame[PORT_WDS][11] == 0xa3);
    /* Settings answered: the session is up and the callback fired */
    p = head(frame, 0x42, 0x002d);
    {
        uint8_t v[4];
        put32(v, 0x0a400002); p = tlv(p, 0x1e, v, 4);
        put32(v, 0x0a400001); p = tlv(p, 0x20, v, 4);
        put32(v, 0xffffff00); p = tlv(p, 0x21, v, 4);
        put32(v, 0x08080808); p = tlv(p, 0x15, v, 4);
        put32(v, 0x08080404); p = tlv(p, 0x16, v, 4);
        put32(v, 1500); p = tlv(p, 0x29, v, 4);
    }
    inject(PORT_WDS, frame, finish(frame, p));
    assert(settings_calls == 1 && redwood_wds_up(&client));
    assert(last_settings.handle == 0x12345678 && last_settings.address == 0x0a400002);
    assert(last_settings.gateway == 0x0a400001 && last_settings.mask == 0xffffff00);
    assert(last_settings.dns1 == 0x08080808 && last_settings.dns2 == 0x08080404 && last_settings.mtu == 1500);
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 4);      /* up: nothing more to send */
    redwood_wds_client_close(&client);
    assert(down_calls == 1 && closes == 3);
    puts("wds-test: DPM -> WDA -> Bind -> Set IP Family -> Start -> Settings verified");

    /* ---- a refused Start is retried after 6 s, not before ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));
    client.to_modem = 2; client.from_modem = 16;
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    drive_to_bound(&client);
    redwood_wds_set_registered(&client, true);
    fake_now += 6000000;
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 3);
    p = head(frame, 0x41, 0x0020);
    p = tlv_result(p, 1, 70);               /* refused: packet domain not attached yet */
    {
        uint8_t r[2] = { 0x46, 0x00 };
        p = tlv(p, 0x10, r, 2);
    }
    inject(PORT_WDS, frame, finish(frame, p));
    assert(!client.start_pending && !redwood_wds_up(&client));
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 3);      /* too soon to retry */
    fake_now += 5999999;
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 3);
    fake_now += 1;
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 4);      /* retried at 6 s */
    redwood_wds_client_close(&client);
    assert(down_calls == 0);                /* never came up: no down call */
    puts("wds-test: refused Start retried after 6 s, not before");

    /* ---- endpoints unconfigured: DPM never sent, the rest goes ahead ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));      /* to_modem/from_modem stay 0 */
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    assert(sent_count[PORT_DPM] == 0);
    assert(sent_count[PORT_WDA] == 1);       /* WDA does not wait for an unconfigured DPM */
    reply_ok(PORT_WDA, 0x43, 0x0020);
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 1);       /* the bind went ahead without DPM */
    assert(sent_count[PORT_DPM] == 0);
    redwood_wds_client_close(&client);
    puts("wds-test: unconfigured endpoints: DPM skipped, WDA and bind proceed");

    /* ---- endpoints filled in late: DPM goes out on set_endpoints ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    assert(sent_count[PORT_DPM] == 0);
    redwood_wds_set_endpoints(&client, 2, 16);
    assert(sent_count[PORT_DPM] == 1 && sent_frame[PORT_DPM][1] == 0x45);
    redwood_wds_client_close(&client);
    puts("wds-test: late endpoints: DPM sent on set_endpoints");

    /* ---- APN: Start carries TLV 0x14 ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));
    client.to_modem = 2; client.from_modem = 16;
    redwood_wds_set_apn(&client, "internet");
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    drive_to_bound(&client);
    redwood_wds_set_registered(&client, true);
    fake_now += 6000000;
    redwood_wds_tick(&client);
    assert(sent_count[PORT_WDS] == 3);
    assert(sent_frame[PORT_WDS][11] == 0x14 && sent_frame[PORT_WDS][12] == 8);
    assert(memcmp(sent_frame[PORT_WDS] + 14, "internet", 8) == 0);
    redwood_wds_client_close(&client);
    puts("wds-test: APN carried in Start Network");

    /* ---- data_off: nothing opens ---- */
    fake_now = 1000000;
    reset_fake();
    memset(&client, 0, sizeof(client));
    client.data_off = true;
    assert(redwood_wds_client_open(&client, &transport, &ops, &client, fake_clock, NULL) == 0);
    assert(opened_count == 0 && lookups == 0);
    redwood_wds_client_close(&client);
    assert(closes == 0);
    puts("wds-test: data_off opens nothing");
    return 0;
}
