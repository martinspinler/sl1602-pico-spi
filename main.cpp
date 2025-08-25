#include <stdio.h>
#include <cstdint>

#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "mux.pio.h"

#include "bsp/board_api.h"
#include "tusb.h"

/* TODO: Enable injecting request after DSPB init.
 * DSPB init can be recognized by request from the FW chip,
 * but better is to use a timeout (when Pico reboots by software request
 * on the fly, there is no such further request from FW chip.)
 */

#define P_IRQ1  12 // Input from DSPB
#define P_IRQB  13 // Output to CPB

#define P_NSS1_DSPB     19 // Injected nSS1

#define P_MUX_SEL_NIRQ0 20
#define P_MUX_SEL_MOSI1 21
#define P_MUX_SEL_MISO0 22
#define P_MUX_SEL_NSS1  26

#define BUFS_IC 2    // Interception buffer count
#define BUFS_IJREQ 1 // Inject request buffer count
#define BUFS_IJRES 4 // Inject response buffer count

#define BUF_LEN 128
#define BUF_STATUS_LEN 47

#define BR_DEBUG 1
#define MC_EN 1                 // Enable multicore

#define PRINTBUF_MAX 64         // Maximum length of printed buffer (crop)
#define PRINTBUF_BPL 64         // Bytes per line

#define ERR_NO_F0               (1 << 0)
#define ERR_NO_F7               (1 << 1)
#define ERR_RESP_TIMEOUT        (1 << 2)
#define ERR_NULL_STATUS         (1 << 3)
#define ERR_BUF_OVERFLOW        (1 << 4)
#define ERR_IC_BUF_NOTREADY     (1 << 5)
#define ERR_IJRES_BUF_NOTREADY  (1 << 6)

#define RET_ERR_BUF_OVERFLOW    (-32767)
#define RET_ERR_BUF_FULL        (-32766)

struct sysex_buffer {
	uint8_t buf[BUF_LEN];
	int16_t pos;
	int16_t len;
};


struct sysex_buffer buf_ic0[BUFS_IC];
struct sysex_buffer buf_ic1[BUFS_IC];
struct sysex_buffer buf_ijreq[BUFS_IJREQ];
struct sysex_buffer buf_ijres[BUFS_IJRES];
struct sysex_buffer buf_tmp_usb;

uint8_t ptr_ic_wr = 0;
uint8_t ptr_ic_rd = 0;
uint8_t ptr_ijreq_wr = 0;
uint8_t ptr_ijreq_rd = 0;
uint8_t ptr_ijres_wr = 0;
uint8_t ptr_ijres_rd = 0;


uint8_t buf_status[BUF_STATUS_LEN] = {0};


bool do_echo_fw = false;
bool do_echo_usb = false;
bool do_filter_status = true;           // filter out the request / response status message
bool do_filter_request = false;         // filter out the request message
bool do_route_ic_usb = false;           // route interception from Master to USB-MIDI

volatile uint16_t r_err = 0;


void printbuf(uint8_t buf[], size_t len)
{
	size_t i;
	for (i = 0; i < len; ++i) {
		if (i % PRINTBUF_BPL == PRINTBUF_BPL -1)
			printf("%02x\n", buf[i]);
		else
			printf("%02x ", buf[i]);
	}

	if (i % PRINTBUF_BPL) {
		putchar('\n');
	}
}

int init_hw()
{
	uint8_t i;
	uint8_t offset;
	uint8_t pin;

	PIO mypio;

	set_sys_clock_khz(200000, false);

	stdio_init_all();
	stdio_set_translate_crlf(&stdio_usb, false);

	for (i = 0; i < 4; i++) {
		/* MUXed outputs */
		gpio_init(i);
		gpio_set_dir(i, 1);

		/* Select pins */
		pin = (i == 3 ? 26 : (i + 20));
		gpio_init(pin);
		gpio_set_dir(pin, 1);
		gpio_put(pin, 0);
	}

	gpio_init(P_IRQ1);
	gpio_init(P_IRQB);
	gpio_init(P_NSS1_DSPB);
	gpio_set_dir(P_IRQ1, 0);
	gpio_set_dir(P_IRQB, 1);
	gpio_set_dir(P_NSS1_DSPB, 1);
	gpio_put(P_IRQB, 1);
	gpio_put(P_NSS1_DSPB, 1);

	offset = pio_add_program(pio0, &mux_program);

	mypio = pio_get_instance(0);
	for (i = 12; i < 20; i++) {
		hw_set_bits(&mypio->input_sync_bypass, (1u << i));
	}

	for (i = 0; i < 4; i++) {
		pio_gpio_init(pio0, i);

		pio_sm_config c = mux_program_get_default_config(offset);
		sm_config_set_in_pins(&c, 12 + i * 2);
		sm_config_set_out_pins(&c, i, 1);
		sm_config_set_jmp_pin(&c, i == 3 ? 26 : (20 + i));
		sm_config_set_out_shift(&c, false, false, 1);
		pio_sm_set_consecutive_pindirs(pio0, i, i, 1, true);
		pio_sm_init(pio0, i, offset, &c);
	}
	pio_set_sm_mask_enabled(pio0, 0x0F, true);

	spi_init(spi0, 2 * 1000000);
	spi_init(spi1, 2 * 1000000);
	spi_set_slave(spi0, true);
	spi_set_slave(spi1, true);

	for (i = 4; i < 12; i++) {
		gpio_set_function(i, GPIO_FUNC_SPI);
	}

	bi_decl(bi_4pins_with_func(4, 5, 6, 7, GPIO_FUNC_SPI));
	bi_decl(bi_4pins_with_func(8, 9, 10, 11, GPIO_FUNC_SPI));

	//gpio_set_function(P_IRQ1, GPIO_FUNC_SIO);
	//gpio_set_function(P_IRQB, GPIO_FUNC_SIO);

	return 0;
}

void buf_clear(struct sysex_buffer *s)
{
	s->pos = 0;
	s->len = 0;
}

bool buf_cleared(struct sysex_buffer *s)
{
	return s->pos == 0;
}

bool buf_full(struct sysex_buffer *s)
{
	return s->len != 0;
}

int16_t buf_append(struct sysex_buffer *s, uint8_t c)
{
	if (s->len) {
		return RET_ERR_BUF_FULL;
	}

	if (c == 0xF0) {
		s->buf[0] = c;
		s->pos = 1;
	} else if (s->pos > 0) {
		s->buf[s->pos] = c;
		s->pos++;

		if (c == 0xF7) {
			s->len = s->pos;
			s->pos = 0;
			return s->len;
		} else if (s->pos >= BUF_LEN) {
			s->pos = 0;
			return RET_ERR_BUF_OVERFLOW;
		}
	}
	return 0;
}

bool is_status_req(struct sysex_buffer *s)
{
	return s->len == 4 && s->buf[1] == 0x38 && s->buf[2] == 0x03;
}

bool is_status_res(struct sysex_buffer *s)
{
	return s->len == BUF_STATUS_LEN && s->buf[1] == 0x39 && s->buf[2] == 0x03;
}

void read_uart_cmd()
{
	int c;

	c = stdio_getchar_timeout_us(0);

	if (c == 'h' || c == '?') {
		printf(
				"StudioLive 16.0.2 SPI-USB control bridge\n"
				"Help\n"
				"f/F: enable/disable logging of FireWire messages\n"
				"u/U: enable/disable logging of USB-MIDI messages (shortlog)\n"
				"q/Q: enable/disable filtering out all request messages\n"
				"s/S: enable/disable filtering out status reqest/response messages\n"
				"i/I: enable/disable routing intercepted messages to USB-MIDI\n"
				"c/C: print/clear status and error registers\n"
		);
	} else if (c == 'f') {
		do_echo_fw = true;
		printf("Echo FireWire on\n");
	} else if (c == 'F') {
		do_echo_fw = false;
		printf("Echo FireWire off\n");
	} else if (c == 'u') {
		do_echo_usb = true;
		printf("Echo USB on\n");
	} else if (c == 'U') {
		do_echo_usb = false;
		printf("Echo USB off\n");
	} else if (c == 'S') {
		do_filter_status = false;
	} else if (c == 's') {
		do_filter_status = true;
	} else if (c == 'Q') {
		do_filter_request = false;
	} else if (c == 'q') {
		do_filter_request = true;
	} else if (c == 'i') {
		do_route_ic_usb = true;
	} else if (c == 'I') {
		do_route_ic_usb = false;
	} else if (c == 'C') {
		r_err = 0;
	} else if (c == 'c') {
		printf("Errors: %04x\n", r_err);
		printf("WR ptrs (IC, IJREQ, IJRES): %02x %02x %02x\n", ptr_ic_wr, ptr_ijreq_wr, ptr_ijres_wr);
		printf("RD ptrs (IC, IJREQ, IJRES): %02x %02x %02x\n", ptr_ic_rd, ptr_ijreq_rd, ptr_ijres_rd);
	}
	__dmb();
}

int read_response(struct sysex_buffer *resp)
{
	bool readable;
	int16_t ret = 0;
	uint16_t pos = 0;
	uint16_t len = 0;
	uint8_t in;

	uint8_t *buf = resp->buf;

	absolute_time_t to;
	to = make_timeout_time_us(1000000);

	while (resp->len == 0) {
		readable = spi_is_readable(spi0);
		if (absolute_time_diff_us(to, get_absolute_time()) > 0) {
			r_err |= ERR_RESP_TIMEOUT;
			return -1;
		}
		if (!readable)
			continue;

		gpio_put(P_IRQB, 1);

		in = spi_get_hw(spi0)->dr;

		if (resp->pos == 1 && in == 0) {
			r_err |= ERR_NULL_STATUS;
			continue;
		}
		ret = buf_append(resp, in);
		if (ret == RET_ERR_BUF_OVERFLOW)
			r_err |= ERR_BUF_OVERFLOW;
	}
	return 0;
}

int transceive_request(struct sysex_buffer *req, struct sysex_buffer *resp)
{
	int16_t i;
	int16_t len = req->len;
	uint8_t *buf = req->buf;
	uint8_t u0;

	/* Error: no SOF/EOF in SysEx */
	if (buf[0] != 0xF0)
		r_err |= ERR_NO_F0;
	if (buf[len-1] != 0xF7)
		r_err |= ERR_NO_F7;

	if (buf[0] != 0xF0 || buf[len-1] != 0xF7)
		return -1;

	gpio_put(P_MUX_SEL_NSS1, 1);
	gpio_put(P_MUX_SEL_MISO0, 1);
	gpio_put(P_MUX_SEL_NIRQ0, 1);

	/* Need read too for FIFO cleanup */
	while (spi_is_readable(spi0))
		u0 = spi_get_hw(spi0)->dr;

	gpio_put(P_IRQB, 0);

	for (i = 0; i < len; i++) {
		while (spi_is_writable(spi0) == 0);
		spi_get_hw(spi0)->dr = buf[i];

		while (spi_is_readable(spi0) == 0);
		u0 = spi_get_hw(spi0)->dr;
	}

	/* INFO: Too early here: deassert in read_response */
//	gpio_put(P_IRQB, 1);

	i = read_response(resp);

	gpio_put(P_MUX_SEL_NSS1, 0);
	gpio_put(P_MUX_SEL_MISO0, 0);
	gpio_put(P_MUX_SEL_NIRQ0, 0);
	return i;
}

void core1_main()
{
	int i;
	uint8_t u0;
	uint8_t u1;
	uint8_t a0;
	uint8_t a1;

	uint8_t si;
	uint8_t ijres_inc;
	bool buf_rdy;

	struct sysex_buffer *ic0, *ic1, *ijreq, *ijres;

	do {
		si = ptr_ic_wr % BUFS_IC;
		ic0 = &buf_ic0[si];
		ic1 = &buf_ic1[si];

		a0 = spi_is_readable(spi0);
		a1 = spi_is_readable(spi1);
		/* SPI are drived by the same CLK/nSS, thus should be synced */
		if (a0 || a1) {
			buf_rdy = ptr_ic_wr - ptr_ic_rd < BUFS_IC;
			if (buf_rdy) {
				if (a0) {
					u0 = spi_get_hw(spi0)->dr;
					buf_append(ic0, u0);
				}
				if (a1) {
					u1 = spi_get_hw(spi1)->dr;
					buf_append(ic1, u1);
				}

				if (buf_full(ic0)) {
					ijres_inc = 0;

					if (do_route_ic_usb) {
						if (!do_filter_request) {
							si = ptr_ijres_wr % BUFS_IJRES;
							ijres = &buf_ijres[si];

							buf_rdy = ptr_ijres_wr - ptr_ijres_rd < BUFS_IJRES;
							if (buf_rdy) {
								memcpy(ijres->buf, ic1->buf, ic1->len);
								ijres->len = ic1->len;
								ijres_inc++;
							} else {
								r_err |= ERR_IJRES_BUF_NOTREADY;
							}
						}

						si = (ptr_ijres_wr + ijres_inc) % BUFS_IJRES;
						ijres = &buf_ijres[si];

						buf_rdy = (ptr_ijres_wr + ijres_inc) - ptr_ijres_rd < BUFS_IJRES;
						if (buf_rdy) {
							memcpy(ijres->buf, ic0->buf, ic0->len);
							ijres->len = ic0->len;
							ijres_inc++;
						} else {
							r_err |= ERR_IJRES_BUF_NOTREADY;
						}
					}
#if MC_EN
					__dmb();
#endif
					ptr_ic_wr++;
					ptr_ijres_wr += ijres_inc;
				}
			} else {
				r_err |= ERR_IC_BUF_NOTREADY;
			}
		}
		if (a0 || a1)
			continue;

		if (buf_cleared(ic0) && buf_cleared(ic1)) {
			buf_rdy = ptr_ijreq_wr != ptr_ijreq_rd;

			si = ptr_ijreq_rd % BUFS_IJREQ;
			ijreq = &buf_ijreq[si];

			si = ptr_ijres_wr % BUFS_IJRES;
			ijres = &buf_ijres[si];

			if (buf_rdy /*&& buf_full(ijreq)*/ && gpio_get(P_IRQ1) == 1) {
				/* Paranoia */
				gpio_put(P_MUX_SEL_NIRQ0, 1);
				if (gpio_get(P_IRQ1) == 1) {
					transceive_request(ijreq, ijres);

					buf_clear(ijreq);

					__dmb();
					ptr_ijres_wr++;
					ptr_ijreq_rd++;
				} else {
					gpio_put(P_MUX_SEL_NIRQ0, 0);
				}
			}
		}
	} while (MC_EN);
}

int main()
{
	int filter;
	int16_t i;
	int16_t len;
	int16_t ijres_tmp_pos = 0;
	uint8_t si;
	bool buf_rdy;

	struct sysex_buffer *s;

	board_init();
	tusb_init();

	init_hw();

	buf_clear(&buf_tmp_usb);
	for (i = 0; i < BUFS_IJREQ; i++) {
		buf_clear(&buf_ijreq[i]);
	}
	for (i = 0; i < BUFS_IJRES; i++) {
		buf_clear(&buf_ijres[i]);
	}
	for (i = 0; i < BUFS_IC; i++) {
		buf_clear(&buf_ic0[i]);
		buf_clear(&buf_ic1[i]);
	}
	__dmb();

#if MC_EN
	multicore_launch_core1(core1_main);
#endif

	while (1) {
		tud_task();
		read_uart_cmd();
#if (!MC_EN)
		core1_main();
#endif

		/* Pull intercept streams (both streams are input, synchronized) and print */
		buf_rdy = ptr_ic_wr != ptr_ic_rd;
		if (buf_rdy) {
			si = ptr_ic_rd % BUFS_IC;

			s = &buf_ic1[si];
			if (do_echo_fw && !do_filter_request) {
				filter = 0;
				if (is_status_req(s) && do_filter_status) {
					filter = 1;
				}

				if (!filter) {
					printf("F2M %d: ", s->len);
					printbuf(s->buf, s->len < PRINTBUF_MAX? s->len : PRINTBUF_MAX);
				}
			}
			buf_clear(s);

			s = &buf_ic0[si];
			if (do_echo_fw) {
				filter = 0;
				if (is_status_res(s) && do_filter_status) {
					if (memcmp(s->buf, buf_status, BUF_STATUS_LEN) == 0) {
						filter = 1;
					} else {
						memcpy(buf_status, s->buf, BUF_STATUS_LEN);
					}
				}
				if (!filter) {
					printf("M2F %d: ", s->len);
					printbuf(s->buf, s->len < PRINTBUF_MAX ? s->len : PRINTBUF_MAX);
				}
			}
			buf_clear(s);

			__dmb();
			ptr_ic_rd++;
		}

		/* Push inject stream readen from USB-MIDI */
		buf_rdy = ptr_ijreq_wr - ptr_ijreq_rd < BUFS_IJREQ;
		if (buf_rdy) {
			si = ptr_ijreq_wr % BUFS_IJREQ;
			s = &buf_ijreq[si];
			buf_tmp_usb.len = tud_midi_n_stream_read(0, 0, buf_tmp_usb.buf, BUF_LEN);
			for (i = 0; i < buf_tmp_usb.len; i++) {
				len = buf_append(s, buf_tmp_usb.buf[i]);
				if (len > 0) {
					if (do_echo_usb)
						printf("U2M %d\n", s->len);
					__dmb();
					ptr_ijreq_wr++;
					/* FIXME: remainder is discarded */
					break;
				}
			}
		}

		/* Pull inject stream and write to USB-MIDI */
		buf_rdy = ptr_ijres_wr != ptr_ijres_rd;
		if (buf_rdy) {
			si = ptr_ijres_rd % BUFS_IJRES;
			s = &buf_ijres[si];
			len = tud_midi_n_stream_write(0, 0, s->buf + ijres_tmp_pos, s->len - ijres_tmp_pos);
			ijres_tmp_pos += len;
			if (ijres_tmp_pos == s->len) {
				if (do_echo_usb)
					printf("M2U %d\n", s->len);

				buf_clear(s);
				ijres_tmp_pos = 0;
				__dmb();
				ptr_ijres_rd++;
			}
		}
	}

	return 0;
}
