/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it. The DSP's protection
 * domains (audio, charger, sensors) ask the application processor's locator which domains provide a service before they start; the
 * battery manager runs in the charger domain. Started with adsp=2. Everything runs in the DSP thread's callbacks: each packet is
 * handled at once and any answer is queued with sheng_adsp_send, so nothing here waits.
 * Also the sensor probe: once the DSP has said hello, it sends the SUID lookup request (a hand-encoded sns_client_request_msg for the
 * fixed lookup sensor 0xabab...ab, msg 512) to every announced sensor service instance under several candidate QMI message ids, and
 * records which instance answers with which type and id. That tells us the client manager's instance and the wrapper id without
 * guessing, before any streaming code is written. */
#include <stddef.h>
#include <stdint.h>
#include "QcomQrtr.h"
#include "ShengAdsp.h"

/* [0] stage (1 channel registered, 2 link up, 3 the DSP's hello, negative: registration failed), [1] packets in, [2] data packets in,
 * [3] bad packets, [4] packets out, [5] sends refused, [6] locator requests, [7] locator answers, [8] the DSP's node, [9] services the
 * DSP announced, [10] link ups, [11] link downs, [12] data for ports we do not have, [13] the last locator request's service name
 * length | its first byte << 8 */
volatile int32_t native_qrtr_stats[16];
/* the services the DSP announced, as service:instance in hex */
char native_qrtr_text[96];
/* sensor probe: [0] stage (1 requests sent), [1] requests sent, [2] replies in, [3] the port a reply came from, [4] its QMI type,
 * [5] its QMI message id, [6] its length, [7] the instance the first request went to */
volatile int32_t native_sensor_stats[16];
/* the last reply, as rx p=.. t=.. m=.. l=.. i=.. in hex */
char native_sensor_text[96];

enum {kLocalNode = 1, kLocatorPort = 1, kProbePort = 2, kSensorService = 0x0f, kProbeService = 0x2a};

static const char *const kAudio[] = {"avs/audio", NULL};
static const char *const kGps[] = {"gps/gps_service", NULL};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},   {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL},    {"msm/cdsp/root_pd", 76, NULL}, {"msm/modem/root_pd", 180, kGps},
};

static QcomQrtr gQrtr;
static ShengAdspChannel *gChannel;

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return sheng_adsp_send(gChannel, packet, length) ? -1 : 0;
}

static void hex(char *out, unsigned *at, unsigned cap, uint32_t value)
{
    char digits[8];
    unsigned n = 0;
    do {digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4;} while (value && n < 8);
    while (n && *at + 1 < cap) out[(*at)++] = digits[--n];
}

/* "key=value" in hex, when there is room. */
static void field(char *out, unsigned *at, unsigned cap, const char *key, uint32_t value)
{
    while (*key && *at + 1 < cap) out[(*at)++] = *key++;
    if (*at + 1 < cap) out[(*at)++] = '=';
    hex(out, at, cap, value);
}

static void counters(void)
{
    native_qrtr_stats[1] = (int32_t)gQrtr.rx_packets;
    native_qrtr_stats[2] = (int32_t)gQrtr.rx_data;
    native_qrtr_stats[3] = (int32_t)gQrtr.rx_bad;
    native_qrtr_stats[4] = (int32_t)gQrtr.tx_packets;
    native_qrtr_stats[5] = (int32_t)gQrtr.tx_refused;
    native_qrtr_stats[8] = gQrtr.hello_received ? (int32_t)gQrtr.remote_node : -1;
    native_qrtr_stats[9] = (int32_t)gQrtr.remote_count;
    native_qrtr_stats[12] = (int32_t)gQrtr.rx_unrouted;
    if (gQrtr.hello_received && native_qrtr_stats[0] == 2) native_qrtr_stats[0] = 3;
    unsigned at = 0;
    const unsigned cap = sizeof native_qrtr_text;
    for (uint32_t i = 0; i < gQrtr.remote_count && at + 4 < cap; ++i) {
        if (at) native_qrtr_text[at++] = ' ';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].service);
        if (at + 1 < cap) native_qrtr_text[at++] = ':';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].instance);
    }
    native_qrtr_text[at] = 0;
    /* the sensor probe's answer rides along, so one [qrtr] line tells both: sent/replies/type/message/instance */
    if (native_sensor_stats[1] && at + 24 < cap) {
        native_qrtr_text[at++] = ';';
        hex(native_qrtr_text, &at, cap, (uint32_t)native_sensor_stats[1]);
        if (at + 1 < cap) native_qrtr_text[at++] = '/';
        hex(native_qrtr_text, &at, cap, (uint32_t)native_sensor_stats[2]);
        if (at + 1 < cap) native_qrtr_text[at++] = '/';
        hex(native_qrtr_text, &at, cap, (uint32_t)native_sensor_stats[4]);
        if (at + 1 < cap) native_qrtr_text[at++] = '/';
        hex(native_qrtr_text, &at, cap, (uint32_t)native_sensor_stats[5]);
        if (at + 1 < cap) native_qrtr_text[at++] = '/';
        hex(native_qrtr_text, &at, cap, (uint32_t)native_sensor_stats[7]);
        native_qrtr_text[at] = 0;
    }
}

/* protobuf: a varint. */
static uint32_t pbVarint(uint8_t *out, uint64_t value)
{
    uint32_t n = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7f);
        value >>= 7;
        out[n++] = (uint8_t)(value ? (byte | 0x80) : byte);
    } while (value);
    return n;
}

/* The sns_client_request_msg asking the SUID lookup sensor for every sensor of every type: SUID 0xabab...ab (the fixed lookup SUID),
 * msg 512 (SNS_SUID_MSGID_SNS_SUID_REQ), payload sns_suid_req with an empty data type and register_updates. */
static uint32_t sensorRequest(uint8_t *out)
{
    uint8_t suid[24], cfg[8], inner[16];
    uint32_t sn = 0, cn = 0, in = 0, n = 0;
    suid[sn++] = 0x08; sn += pbVarint(suid + sn, 0xababababababababull);  /* sns_std_suid: suid_high */
    suid[sn++] = 0x10; sn += pbVarint(suid + sn, 0xababababababababull);  /* suid_low */
    cfg[cn++] = 0x10; cfg[cn++] = 0x01;                                    /* sns_suid_req: register_updates = true */
    inner[in++] = 0x1a; in += pbVarint(inner + in, cn);                    /* sns_std_request: payload */
    for (uint32_t i = 0; i < cn; ++i) inner[in++] = cfg[i];
    out[n++] = 0x0a; n += pbVarint(out + n, sn);                           /* suid */
    for (uint32_t i = 0; i < sn; ++i) out[n++] = suid[i];
    out[n++] = 0x10; n += pbVarint(out + n, 512);                          /* msg_id */
    out[n++] = 0x1a; n += pbVarint(out + n, in);                           /* request */
    for (uint32_t i = 0; i < in; ++i) out[n++] = inner[i];
    return n;
}

/* Candidate QMI message ids carrying sns_client_request_msg; the one the client manager accepts answers, the others are refused or
 * dropped, which is exactly what the reply tells us. */
static const uint32_t kProbeMessages[] = {0x00, 0x01, 0x02, 0x20, 0x21};

static void probeSend(void)
{
    uint8_t payload[64], packet[7 + 64];
    const uint32_t pn = sensorRequest(payload);
    uint16_t transaction = 1;
    for (uint32_t r = 0; r < gQrtr.remote_count; ++r) {
        if (gQrtr.remote[r].service != (uint32_t)kSensorService) continue;
        if (!native_sensor_stats[7]) native_sensor_stats[7] = (int32_t)gQrtr.remote[r].instance;
        for (uint32_t m = 0; m < sizeof kProbeMessages / sizeof kProbeMessages[0]; ++m) {
            packet[0] = QCOM_QMI_REQUEST;
            packet[1] = (uint8_t)transaction; packet[2] = (uint8_t)(transaction >> 8);
            packet[3] = (uint8_t)kProbeMessages[m]; packet[4] = (uint8_t)(kProbeMessages[m] >> 8);
            packet[5] = (uint8_t)pn; packet[6] = (uint8_t)(pn >> 8);
            for (uint32_t i = 0; i < pn; ++i) packet[7 + i] = payload[i];
            if (!qcom_qrtr_send_data(&gQrtr, kProbePort, gQrtr.remote[r].node, gQrtr.remote[r].port, packet, 7 + pn)) {
                native_sensor_stats[1]++;
                ++transaction;
            }
        }
    }
    native_sensor_stats[0] = 1;
}

static void probe(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q; (void)port; (void)node;
    QcomQmiMessage m;
    if (qcom_qmi_parse(data, length, &m)) return;
    native_sensor_stats[2]++;
    native_sensor_stats[3] = (int32_t)from;
    native_sensor_stats[4] = m.type;
    native_sensor_stats[5] = (int32_t)m.message;
    native_sensor_stats[6] = (int32_t)length;
    unsigned at = 0;
    const unsigned cap = sizeof native_sensor_text;
    field(native_sensor_text, &at, cap, "rx p", from);
    field(native_sensor_text, &at, cap, " t", (uint32_t)m.type);
    field(native_sensor_text, &at, cap, " m", m.message);
    field(native_sensor_text, &at, cap, " l", length);
    for (uint32_t i = 0; i < gQrtr.remote_count; ++i)
        if (gQrtr.remote[i].port == from) {field(native_sensor_text, &at, cap, " i", gQrtr.remote[i].instance); break;}
    native_sensor_text[at] = 0;
}

static void locator(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context;
    uint8_t answer[480];
    QcomQmiMessage m;
    uint32_t size = 0;
    native_qrtr_stats[6]++;
    if (!qcom_qmi_parse(data, length, &m)) {
        const uint8_t *name = qcom_qmi_tlv(&m, 0x01, &size);
        if (name) native_qrtr_stats[13] = (int32_t)(size | (size ? (uint32_t)name[0] << 8 : 0));
    }
    const uint32_t n = qcom_servloc_answer(kDomains, sizeof kDomains / sizeof kDomains[0], data, length, answer, sizeof answer);
    if (n && !qcom_qrtr_send_data(q, port, node, from, answer, n)) native_qrtr_stats[7]++;
}

static void linkState(void *context, int up)
{
    (void)context;
    if (up) {
        native_qrtr_stats[10]++;
        /* a fresh link: forget the old one's hello and services, keep ours */
        gQrtr.hello_sent = gQrtr.hello_received = 0;
        gQrtr.remote_count = 0;
        gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
        native_qrtr_stats[0] = 2;
        qcom_qrtr_start(&gQrtr);
    } else {
        native_qrtr_stats[11]++;
        native_qrtr_stats[0] = 1;
    }
    counters();
}

static void linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    qcom_qrtr_receive(&gQrtr, data, length);
    counters();
    /* the DSP announces its services in batches: ask the sensor ones who they are each time the list grows, a few rounds at most */
    if (gQrtr.hello_received && gQrtr.remote_count && native_sensor_stats[9] < 8 &&
        (int32_t)gQrtr.remote_count != native_sensor_stats[8]) {
        native_sensor_stats[8] = (int32_t)gQrtr.remote_count;
        native_sensor_stats[9]++;
        probeSend();
        counters();
    }
}

void sheng_qrtr_start(void)
{
    static const ShengAdspClient client = {linkState, linkReceive};
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
    qcom_qrtr_add_service(&gQrtr, (uint32_t)kProbeService, 0x101u, (uint32_t)kProbePort, probe, NULL);
    gChannel = sheng_adsp_channel("IPCRTR", 1024, 8, 0, &client, NULL);
    native_qrtr_stats[0] = gChannel ? 1 : -1;
}
