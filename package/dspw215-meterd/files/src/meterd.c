// SPDX-License-Identifier: MIT
//
// meterd — D-Link DSP-W215 (Prolific PL8331) meter + relay -> MQTT bridge.
//
// Reads the PL8331 energy meter over the UART (115200 8N1, ASCII-hex frames)
// and drives/reads the relay GPIO, publishing to MQTT with Home Assistant
// MQTT Discovery (switch + power/temperature sensors).
//
// Status: the UART/MQTT/relay plumbing is complete; the PL8331 frame format
// (poll command + response parse + scaling) must be filled into poll_meter()
// from a live capture — see ../../../HARDWARE.md and dsp-w215-meter-protocol.md.
// Run `meterd -r -H /dev/ttyS0` to dump raw meter bytes for that capture.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>

struct cfg {
	const char *device, *broker, *user, *pass;
	const char *node, *name, *base, *disc;
	int port, interval, relay_gpio, raw, verbose;
};

struct reading {
	int relay;          // 0/1, -1 unknown
	int have_meter;     // 1 if watt/volt/amp/temp valid
	double watt, volt, amp, temp;
};

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

// ----------------------------------------------------------------------- UART
static int uart_open(const char *dev)
{
	int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { perror("open uart"); return -1; }

	struct termios t;
	if (tcgetattr(fd, &t) != 0) { perror("tcgetattr"); close(fd); return -1; }
	cfmakeraw(&t);
	cfsetispeed(&t, B115200);   // stock relies on the inherited console baud;
	cfsetospeed(&t, B115200);   // we set it explicitly. 8N1:
	t.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
	t.c_cflag |= CS8 | CLOCAL | CREAD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 1;          // 0.1s read timeout
	if (tcsetattr(fd, TCSANOW, &t) != 0) { perror("tcsetattr"); close(fd); return -1; }
	tcflush(fd, TCIOFLUSH);
	return fd;
}

// Read until ~idle_ms with no new bytes (mirrors the stock daemon's timed read).
static int uart_read_idle(int fd, unsigned char *buf, size_t max, int idle_ms)
{
	size_t n = 0;
	struct timespec last;
	clock_gettime(CLOCK_MONOTONIC, &last);
	while (n < max) {
		unsigned char c;
		ssize_t r = read(fd, &c, 1);
		if (r == 1) {
			buf[n++] = c;
			clock_gettime(CLOCK_MONOTONIC, &last);
			continue;
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - last.tv_sec) * 1000 +
			  (now.tv_nsec - last.tv_nsec) / 1000000;
		if (n > 0 && ms >= idle_ms) break;
		if (n == 0 && ms >= idle_ms * 4) break;   // give up if silent
		usleep(2000);
	}
	return (int)n;
}

// --------------------------------------------------------------- relay (sysfs)
static void gpio_export(int gpio)
{
	char p[64];
	snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", gpio);
	if (access(p, F_OK) == 0) return;
	int fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd < 0) return;
	char b[16]; int n = snprintf(b, sizeof b, "%d", gpio);
	(void)!write(fd, b, n); close(fd);
	snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", gpio);
	fd = open(p, O_WRONLY);
	if (fd >= 0) { (void)!write(fd, "out", 3); close(fd); }
}

static int gpio_read(int gpio)
{
	char p[64]; snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", gpio);
	int fd = open(p, O_RDONLY); if (fd < 0) return -1;
	char c = '0'; int r = read(fd, &c, 1); close(fd);
	return (r == 1) ? (c == '1') : -1;
}

static void gpio_write(int gpio, int v)
{
	char p[64]; snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", gpio);
	int fd = open(p, O_WRONLY); if (fd < 0) return;
	(void)!write(fd, v ? "1" : "0", 1); close(fd);
}

// ------------------------------------------------------------- PL8331 protocol
//
// TODO(capture): the PL8331 poll command and response layout are board/chip
// firmware specific and not present as constants in the stock binary. Capture
// the UART (115200 8N1) on a running stock unit, correlate with a known load,
// then implement:
//   1. write the ASCII-hex poll command + '\n' to `fd`
//   2. parse the ASCII-hex response into watt/volt/amp/temp with the scaling
// Until then, meter fields are reported as unavailable (relay still works).
static void poll_meter(int fd, struct cfg *c, struct reading *out)
{
	out->relay = (c->relay_gpio >= 0) ? gpio_read(c->relay_gpio) : -1;
	out->have_meter = 0;

	if (fd < 0) return;

	// --- placeholder framing (shape known; bytes TODO) ---
	// const char *poll = "....\n";              // ASCII-hex poll command
	// write(fd, poll, strlen(poll));
	unsigned char resp[256];
	int n = uart_read_idle(fd, resp, sizeof resp, 20);
	if (c->raw && n > 0) {
		fprintf(stderr, "RAW %d bytes:", n);
		for (int i = 0; i < n; i++) fprintf(stderr, " %02x", resp[i]);
		fprintf(stderr, "\n");
	}
	// TODO: decode resp -> out->watt/volt/amp/temp; set out->have_meter = 1;
}

// ------------------------------------------------------------------------ MQTT
static char T_AVAIL[160], T_STATE[160], T_SET[160];

static void pub(struct mosquitto *m, const char *topic, const char *payload, int retain)
{
	mosquitto_publish(m, NULL, topic, (int)strlen(payload), payload, 1, retain);
}

static void publish_discovery(struct mosquitto *m, struct cfg *c)
{
	char dev[512];
	snprintf(dev, sizeof dev,
		"\"dev\":{\"ids\":[\"dspw215_%s\"],\"name\":\"%s\","
		"\"mdl\":\"DSP-W215\",\"mf\":\"D-Link\"},"
		"\"o\":{\"name\":\"dspw215-meterd\"},"
		"\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\"",
		c->node, c->name, T_AVAIL);

	char topic[200], cfgbuf[900];

	snprintf(topic, sizeof topic, "%s/switch/dspw215_%s/socket/config", c->disc, c->node);
	snprintf(cfgbuf, sizeof cfgbuf,
		"{%s,\"name\":\"Socket\",\"uniq_id\":\"dspw215_%s_socket\","
		"\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"val_tpl\":\"{{value_json.state}}\","
		"\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"ic\":\"mdi:power-socket\"}",
		dev, c->node, T_SET, T_STATE);
	pub(m, topic, cfgbuf, 1);

	snprintf(topic, sizeof topic, "%s/sensor/dspw215_%s/power/config", c->disc, c->node);
	snprintf(cfgbuf, sizeof cfgbuf,
		"{%s,\"name\":\"Power\",\"uniq_id\":\"dspw215_%s_power\","
		"\"stat_t\":\"%s\",\"dev_cla\":\"power\",\"unit_of_meas\":\"W\","
		"\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.power|default('')}}\"}",
		dev, c->node, T_STATE);
	pub(m, topic, cfgbuf, 1);

	snprintf(topic, sizeof topic, "%s/sensor/dspw215_%s/temp/config", c->disc, c->node);
	snprintf(cfgbuf, sizeof cfgbuf,
		"{%s,\"name\":\"Temperature\",\"uniq_id\":\"dspw215_%s_temp\","
		"\"stat_t\":\"%s\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
		"\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.temperature|default('')}}\"}",
		dev, c->node, T_STATE);
	pub(m, topic, cfgbuf, 1);
}

static struct cfg *g_cfg;

static void on_connect(struct mosquitto *m, void *u, int rc)
{
	(void)u; (void)rc;
	mosquitto_subscribe(m, NULL, T_SET, 1);
	pub(m, T_AVAIL, "online", 1);
	publish_discovery(m, g_cfg);
}

static void on_message(struct mosquitto *m, void *u, const struct mosquitto_message *msg)
{
	(void)m; (void)u;
	if (g_cfg->relay_gpio < 0 || !msg->payloadlen) return;
	if (!strncasecmp(msg->payload, "ON", msg->payloadlen))  gpio_write(g_cfg->relay_gpio, 1);
	if (!strncasecmp(msg->payload, "OFF", msg->payloadlen)) gpio_write(g_cfg->relay_gpio, 0);
}

static void publish_state(struct mosquitto *m, struct reading *r)
{
	char buf[256]; int n = 0;
	n += snprintf(buf + n, sizeof buf - n, "{\"state\":\"%s\"",
		      r->relay == 1 ? "ON" : "OFF");
	if (r->have_meter) {
		n += snprintf(buf + n, sizeof buf - n,
			",\"power\":%.1f,\"voltage\":%.1f,\"current\":%.3f,\"temperature\":%.1f",
			r->watt, r->volt, r->amp, r->temp);
	}
	snprintf(buf + n, sizeof buf - n, "}");
	pub(m, T_AVAIL, "online", 1);
	pub(m, T_STATE, buf, 1);
}

// ------------------------------------------------------------------------ main
static void usage(void)
{
	fprintf(stderr,
	  "meterd -b broker [-H dev] [-p port] [-u user] [-P pass]\n"
	  "       [-n node] [-N name] [-g relay_gpio] [-t base] [-d disc] [-i sec]\n"
	  "       [-r] (raw UART dump for protocol capture)  [-v]\n");
}

int main(int argc, char **argv)
{
	struct cfg c = { .device = "/dev/ttyS0", .broker = NULL, .port = 1883,
			 .node = "dspw215", .name = "DSP-W215", .base = "dspw215",
			 .disc = "homeassistant", .interval = 30, .relay_gpio = -1 };
	int opt;
	while ((opt = getopt(argc, argv, "H:b:p:u:P:n:N:g:t:d:i:rv")) != -1) {
		switch (opt) {
		case 'H': c.device = optarg; break;
		case 'b': c.broker = optarg; break;
		case 'p': c.port = atoi(optarg); break;
		case 'u': c.user = optarg; break;
		case 'P': c.pass = optarg; break;
		case 'n': c.node = optarg; break;
		case 'N': c.name = optarg; break;
		case 'g': c.relay_gpio = atoi(optarg); break;
		case 't': c.base = optarg; break;
		case 'd': c.disc = optarg; break;
		case 'i': c.interval = atoi(optarg); break;
		case 'r': c.raw = 1; break;
		case 'v': c.verbose = 1; break;
		default: usage(); return 2;
		}
	}
	g_cfg = &c;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	int uart = uart_open(c.device);
	if (c.relay_gpio >= 0) gpio_export(c.relay_gpio);

	// Raw capture mode: just dump UART bytes, no MQTT (for protocol RE).
	if (c.raw && !c.broker) {
		fprintf(stderr, "raw capture on %s @115200 8N1 (Ctrl-C to stop)\n", c.device);
		struct reading r;
		while (!g_stop) poll_meter(uart, &c, &r);
		if (uart >= 0) close(uart);
		return 0;
	}
	if (!c.broker) { usage(); return 2; }

	snprintf(T_AVAIL, sizeof T_AVAIL, "%s/%s/availability", c.base, c.node);
	snprintf(T_STATE, sizeof T_STATE, "%s/%s/state", c.base, c.node);
	snprintf(T_SET,   sizeof T_SET,   "%s/%s/set", c.base, c.node);

	mosquitto_lib_init();
	struct mosquitto *m = mosquitto_new(NULL, true, NULL);
	if (!m) { fprintf(stderr, "mosquitto_new failed\n"); return 1; }
	if (c.user) mosquitto_username_pw_set(m, c.user, c.pass);
	mosquitto_will_set(m, T_AVAIL, 7, "offline", 1, true);
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_message_callback_set(m, on_message);

	if (mosquitto_connect(m, c.broker, c.port, 60) != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "connect to %s:%d failed\n", c.broker, c.port);
		return 1;
	}
	mosquitto_loop_start(m);

	while (!g_stop) {
		struct reading r;
		poll_meter(uart, &c, &r);
		publish_state(m, &r);
		for (int i = 0; i < c.interval && !g_stop; i++) sleep(1);
	}

	pub(m, T_AVAIL, "offline", 1);
	mosquitto_loop_stop(m, true);
	mosquitto_disconnect(m);
	mosquitto_destroy(m);
	mosquitto_lib_cleanup();
	if (uart >= 0) close(uart);
	return 0;
}
