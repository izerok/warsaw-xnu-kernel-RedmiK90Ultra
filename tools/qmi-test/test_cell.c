#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/redwood_cell.h>

/* The cell's own MACs (02:72:65:64:77:01 "redw", router .fe), as the code uses them. */
static const uint8_t cell_mac[6] = { 0x02, 0x72, 0x65, 0x64, 0x77, 0x01 };
static const uint8_t router_mac[6] = { 0x02, 0x72, 0x65, 0x64, 0x77, 0xfe };
static const uint8_t client_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t ip_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < length; i += 2) sum += (uint32_t)data[i] << 8 | data[i + 1];
    if (length & 1) sum += (uint32_t)data[length - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- a fake data plane ---- */
static uint8_t last_to_modem[1514];
static size_t last_to_modem_len;
static int to_modem_count;
static bool fake_ready = true;
static uint8_t canned_packet[64];
static size_t canned_len;
static int canned_count;

static bool fake_ready_fn(void) { return fake_ready; }
static bool fake_transmit(uint8_t mux, const void *packet, size_t length)
{
    assert(mux == 1 && length <= sizeof(last_to_modem));
    memcpy(last_to_modem, packet, length);
    last_to_modem_len = length;
    to_modem_count++;
    return true;
}
static unsigned fake_poll(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length))
{
    unsigned got = 0;
    for (int i = 0; i < canned_count; i++) {
        receive(1, canned_packet, canned_len);
        got++;
    }
    canned_count = 0;
    return got;
}
static void fake_status(char *to, size_t size) { snprintf(to, size, "fake ipa"); }
static const struct redwood_cell_data_ops fake_ops = {
    .ready = fake_ready_fn, .transmit = fake_transmit, .poll = fake_poll, .status = fake_status
};

/* ---- frame builders ---- */
static void eth(uint8_t *frame, const uint8_t *dst, const uint8_t *src, uint16_t type)
{
    memcpy(frame, dst, 6);
    memcpy(frame + 6, src, 6);
    put16(frame + 12, type);
}
/* An ARP request for `target` from the client. */
static size_t arp_request(uint8_t *frame, uint32_t target)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t *arp = frame + 14;
    eth(frame, bcast, client_mac, 0x0806);
    put32(arp, 0x00010800);
    arp[4] = 6; arp[5] = 4;
    put16(arp + 6, 1);
    memcpy(arp + 8, client_mac, 6);
    put32(arp + 14, 0x0a400064);              /* the asker: 10.64.0.100 */
    memset(arp + 18, 0, 6);
    put32(arp + 24, target);
    return 42;
}
/* A DHCP DISCOVER or REQUEST (type 1 or 3); wanted==0 leaves option 50 out. */
static size_t dhcp_request_frame(uint8_t *frame, uint8_t type, uint32_t wanted)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t *ip = frame + 14, *udp = ip + 20, *bootp = udp + 8, *o;
    eth(frame, bcast, client_mac, 0x0800);
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, 20 + 8 + 300);
    put16(ip + 4, 0); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17;
    put32(ip + 12, 0);
    put32(ip + 16, 0xffffffff);
    put16(ip + 10, ip_checksum(ip, 20));
    put16(udp, 68); put16(udp + 2, 67); put16(udp + 4, 8 + 300); put16(udp + 6, 0);
    memset(bootp, 0, 300);
    bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
    put32(bootp + 4, 0x11223344);              /* transaction id */
    memcpy(bootp + 28, client_mac, 6);
    put32(bootp + 236, 0x63825363);
    o = bootp + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (wanted != 0) { *o++ = 50; *o++ = 4; put32(o, wanted); o += 4; }
    *o++ = 255;
    return 14 + 20 + 8 + 300;
}
/* A plain IPv4 packet (UDP to port 53) wrapped in Ethernet. */
static size_t ip_frame(uint8_t *frame, size_t payload_len)
{
    uint8_t *ip = frame + 14, *udp = ip + 20;
    eth(frame, router_mac, client_mac, 0x0800);
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (uint32_t)(20 + 8 + payload_len));
    put16(ip + 4, 0); put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 17;
    put32(ip + 12, 0x0a400002);
    put32(ip + 16, 0x08080808);
    put16(ip + 10, ip_checksum(ip, 20));
    put16(udp, 1234); put16(udp + 2, 53); put16(udp + 4, (uint32_t)(8 + payload_len)); put16(udp + 6, 0);
    memset(udp + 8, 0xab, payload_len);
    return 14 + 20 + 8 + payload_len;
}

int main(void)
{
    uint8_t frame[1514], reply[1514];
    size_t len, reply_len;
    struct redwood_cell_counts c;

    redwood_cell_set_data_ops(&fake_ops);
    assert(!redwood_cell_up());

    /* ---- attach: the data plane is ready, but the net layer takes one link ---- */
    assert(!redwood_cell_attach());   /* honest limitation: en1 waits for the multi-link step */
    redwood_cell_set_data_ops(NULL);
    assert(!redwood_cell_attach());   /* no data plane at all */
    redwood_cell_set_data_ops(&fake_ops);

    /* ---- session math: address .2 gets router .1 ---- */
    redwood_cell_session(true, 0x0a400002, 0xffffff00, 0x0a400001, 0x08080808, 0x08080404, 1500);
    assert(redwood_cell_up());

    /* ---- ARP: the router answers for any address but its own ---- */
    len = arp_request(frame, 0x08080808);
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1 && reply_len == 42);
    assert(memcmp(reply, client_mac, 6) == 0 && memcmp(reply + 6, router_mac, 6) == 0);
    assert(reply[12] == 0x08 && reply[13] == 0x06);
    assert(reply[20] == 0 && reply[21] == 2);              /* a reply */
    assert(memcmp(reply + 22, router_mac, 6) == 0);       /* "that address is me" */
    assert(get32(reply + 28) == 0x08080808);
    assert(memcmp(reply + 32, client_mac, 6) == 0);
    assert(get32(reply + 38) == 0x0a400064);
    len = arp_request(frame, 0x0a400002);                  /* our own address: no answer */
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == -1);
    len = arp_request(frame, 0x08080808);
    frame[14 + 7] = 2;                                     /* not a request: no answer */
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == -1);

    /* ---- DHCP: discover -> offer, request -> ack, wrong address -> nak ---- */
    len = dhcp_request_frame(frame, 1, 0);
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1);
    assert(memcmp(reply, client_mac, 6) == 0 && memcmp(reply + 6, router_mac, 6) == 0);
    assert(reply[12] == 0x08 && reply[13] == 0x00);
    assert(get32(reply + 14 + 12) == 0x0a400001);          /* IP from the router */
    assert(get32(reply + 14 + 16) == 0xffffffff);          /* to everyone */
    assert(ip_checksum(reply + 14, 20) == 0);              /* the header checksum is right */
    {
        const uint8_t *bootp = reply + 14 + 20 + 8;
        assert(bootp[0] == 2 && get32(bootp + 4) == 0x11223344);
        assert(get32(bootp + 16) == 0x0a400002);           /* yiaddr: the session address */
        assert(get32(bootp + 20) == 0x0a400001);           /* siaddr: the router */
        assert(get32(bootp + 236) == 0x63825363);
        assert(bootp[240] == 53 && bootp[242] == 2);       /* an offer */
        assert(bootp[243] == 54 && get32(bootp + 245) == 0x0a400001);
        assert(bootp[249] == 51 && get32(bootp + 251) == 86400);
        assert(bootp[255] == 1 && get32(bootp + 257) == 0xffffff00);
        assert(bootp[261] == 3 && get32(bootp + 263) == 0x0a400001);
        assert(bootp[267] == 6 && bootp[268] == 8);
        assert(get32(bootp + 269) == 0x08080808 && get32(bootp + 273) == 0x08080404);
        assert(bootp[277] == 26 && bootp[278] == 2 && bootp[279] == (1500 >> 8) && bootp[280] == (1500 & 0xff));
    }
    len = dhcp_request_frame(frame, 3, 0x0a400002);         /* request for our address */
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1);
    assert(reply[14 + 20 + 8 + 240] == 53 && reply[14 + 20 + 8 + 242] == 5);   /* an acknowledgement */
    len = dhcp_request_frame(frame, 3, 0x0a400099);         /* request for another address */
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1);
    assert(reply[14 + 20 + 8 + 240] == 53 && reply[14 + 20 + 8 + 242] == 6);   /* a refusal */
    assert(get32(reply + 14 + 20 + 8 + 16) == 0);          /* a NAK carries no address */
    redwood_cell_session(false, 0, 0, 0, 0, 0, 0);         /* session down: DHCP stops */
    assert(!redwood_cell_up());
    len = dhcp_request_frame(frame, 1, 0);
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == -1);

    /* ---- session math, second case: address .1 gets router .2 ---- */
    redwood_cell_session(true, 0x0a400001, 0xffffff00, 0x0a400001, 0x08080808, 0, 1400);
    len = dhcp_request_frame(frame, 1, 0);
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1);
    assert(get32(reply + 14 + 20 + 8 + 20) == 0x0a400002);  /* siaddr .2, not .1 */
    assert(get32(reply + 14 + 20 + 8 + 16) == 0x0a400001);  /* yiaddr .1 */

    /* ---- IPv4 out: the packet goes to the modem on mux 1 ---- */
    len = ip_frame(frame, 12);
    to_modem_count = 0;
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 0);
    assert(to_modem_count == 1 && last_to_modem_len == len - 14);
    assert(memcmp(last_to_modem, frame + 14, len - 14) == 0);
    /* IPv6 and the rest: not carried */
    eth(frame, router_mac, client_mac, 0x86dd);
    assert(redwood_cell_handle_out(frame, 60, reply, &reply_len) == -1);
    /* too short, too long: dropped */
    assert(redwood_cell_handle_out(frame, 13, reply, &reply_len) == -1);
    assert(redwood_cell_handle_out(frame, REDWOOD_CELL_FRAME_MAX + 1, reply, &reply_len) == -1);
    /* no data plane: the packet is dropped, not sent */
    redwood_cell_set_data_ops(NULL);
    len = ip_frame(frame, 12);
    assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == -1);
    assert(to_modem_count == 1);
    redwood_cell_set_data_ops(&fake_ops);

    /* ---- packets from the modem: wrapped as Ethernet from the router ---- */
    {
        uint8_t ip[20 + 8 + 4];
        memset(ip, 0, sizeof(ip));
        ip[0] = 0x45;
        put16(ip + 2, sizeof(ip));
        ip[9] = 17;
        assert(redwood_cell_handle_in(1, ip, sizeof(ip), reply, &reply_len));
        assert(reply_len == 14 + sizeof(ip));
        assert(memcmp(reply, cell_mac, 6) == 0 && memcmp(reply + 6, router_mac, 6) == 0);
        assert(reply[12] == 0x08 && reply[13] == 0x00);
        assert(memcmp(reply + 14, ip, sizeof(ip)) == 0);
        ip[0] = 0x60;                                        /* IPv6: wrapped, type 0x86dd */
        assert(redwood_cell_handle_in(1, ip, sizeof(ip), reply, &reply_len));
        assert(reply[12] == 0x86 && reply[13] == 0xdd);
        assert(!redwood_cell_handle_in(1, ip, 19, reply, &reply_len));   /* too short */
    }

    /* ---- poll drives the receive path ---- */
    canned_len = 20 + 8;
    memset(canned_packet, 0, canned_len);
    canned_packet[0] = 0x45;
    canned_count = 2;
    assert(redwood_cell_poll() == 2);
    redwood_cell_counts(&c);
    assert(c.from_modem >= 2 && c.to_modem == 1 && c.arp == 1 && c.dhcp == 4);   /* the WDS discover below makes 5 */
    assert(c.dropped_out >= 2 && c.not_ip4 == 1);

    /* ---- the WDS callbacks drive the session ---- */
    {
        struct redwood_wds_settings s = { .handle = 0x77, .address = 0x0a400005, .gateway = 0x0a400001,
            .mask = 0xffffff00, .dns1 = 0x08080808, .dns2 = 0, .mtu = 1500 };
        redwood_cell_wds_settings(NULL, &s);
        assert(redwood_cell_up());
        len = dhcp_request_frame(frame, 1, 0);
        assert(redwood_cell_handle_out(frame, len, reply, &reply_len) == 1);
        assert(get32(reply + 14 + 20 + 8 + 16) == 0x0a400005);
        redwood_cell_wds_down(NULL);
        assert(!redwood_cell_up());
    }
    puts("cell-test: ARP answers for any address, DHCP offer/ack/nak with the session settings, IPv4 to the modem on mux 1, modem packets wrapped; en1 attach still waits for multi-link");
    return 0;
}
