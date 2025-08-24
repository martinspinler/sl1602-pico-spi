
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


#define P_IRQ1  12 // Input from DSPB
#define P_IRQB  13 // Output to CPB

#define P_NSS1_DSPB     19 // Injected nSS1

#define P_MUX_SEL_NIRQ0 20
#define P_MUX_SEL_MOSI1 21
#define P_MUX_SEL_MISO0 22
#define P_MUX_SEL_NSS1  26


#define BUF_LEN 128
#define BR_DEBUG 1
#define MC_EN 1			// Enable multicore
#define MC_DIS (!MC_EN)


struct sysex_stream {
	uint8_t buf[BUF_LEN];
	int16_t pos;
	int16_t len;
	uint8_t in;
};



#define IC_STREAMS 2    // Interception streams
#define IJ_STREAMS 1    // Inject streams
struct sysex_stream streams_ic0[IC_STREAMS];
struct sysex_stream streams_ic1[IC_STREAMS];
struct sysex_stream streams_ij_req[IJ_STREAMS];
struct sysex_stream streams_ij_res[IJ_STREAMS];

struct sysex_stream s_usbtmp;

volatile uint16_t g_st = 0;
uint8_t ic_stream_wrptr = 0;
uint8_t ic_stream_rdptr = 0;
uint8_t ijreq_stream_wrptr = 0;
uint8_t ijreq_stream_rdptr = 0;
uint8_t ijres_stream_wrptr = 0;
uint8_t ijres_stream_rdptr = 0;


const uint8_t g_buf0[1] = {0};

bool do_echo = 1;


void printbuf(uint8_t buf[], size_t len)
{
	const int bpl = 16;

	int i;
	for (i = 0; i < len; ++i) {
		if (i % bpl == bpl-1)
			printf("%02x\n", buf[i]);
		else
			printf("%02x ", buf[i]);
	}

	if (i % bpl) {
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

void stream_clear(struct sysex_stream *s)
{
	s->pos = 0;
	s->len = 0;
}

bool stream_cleared(struct sysex_stream *s)
{
	return s->pos == 0;
}

bool stream_filling(struct sysex_stream *s)
{
	return s->pos != 0;
}

bool stream_full(struct sysex_stream *s)
{
	return s->len != 0;
}

int16_t sysex_stream_check(struct sysex_stream *s, uint8_t c)
{
	s->in = c;

	if (s->in == 0xF0) {
		s->buf[0] = s->in;
		s->pos = 1;
		return -s->pos;
	} else if (s->pos > 0) {
		s->buf[s->pos] = s->in;
		s->pos++;

		if (s->in == 0xF7) {
			s->len = s->pos;
			s->pos = 0;
			return s->len;
		} else if (s->pos >= BUF_LEN) {
			s->pos = 0;
			return -2;
		} else {
			return -s->pos;
		}
	}
	return 0;
}

#if 0
int16_t read_firewire_request(struct sysex_stream *s)
{
	int16_t len = 0;

	//s->pos = 0;
	while (gpio_get(P_IRQ1) == 0 && len == 0) {
		spi_write_read_blocking(spi1, g_buf0, &s->in, 1);
		len = sysex_stream_check(s, s->in);
		/* TODO: check for non-empty len: if valid, just wait for nIRQ=1 */
	}
	while (gpio_get(P_IRQ1) == 0);

	return len;
}

void send_firewire_response(struct sysex_stream *s, uint16_t len)
{
	s->pos = 0;
	while (s->pos < len) {
		spi_get_hw(spi1)->dr = s->buf[s->pos++];
		while (spi_is_writable(spi0) == 0);
	}
}
#endif

void read_uart_cmd()
{
	int c;

	c = stdio_getchar_timeout_us(0);

	if (c == '1') {
		do_echo = 1;
		printf("Echo on\n");
	} else if (c == '0') {
		do_echo = 0;
		printf("Echo off\n");
	}
}

#if 0
int16_t read_uart_request(struct sysex_stream *s)
{
	int16_t len = 0;
	int c;

	c = stdio_getchar_timeout_us(0);

	if (s->pos == 0) {
		if (c == '1')
			do_echo = 1;
		else if (c == '0')
			do_echo = 0;
	}
	while (c >= 0) {
		/* TODO: timeout */
		len = sysex_stream_check(s, c);
		if (len > 0)
			return len;

		c = stdio_getchar_timeout_us(0);
	}

	return len;
}

void send_uart_response(struct sysex_stream *s, uint16_t len)
{
	s->pos = 0;
	while (s->pos < len) {
		printf("%c", s->buf[s->pos++]);
	}
	fflush(stdout);
}
#endif
#if 0
int send_request(uint8_t *buf, uint16_t len)
{
	uint16_t pos;

	pos = 0;
	while (pos < len) {
		spi_get_hw(spi0)->dr = buf[pos++];
		gpio_put(P_IRQB, 0);
		while (spi_is_writable(spi0) == 0);
	}

	/* CHECKME: deassert at last byte? */
	gpio_put(P_IRQB, 1);
	return 0;
}
#endif

int read_response(uint8_t *buf)
{
	bool readable;
	uint16_t pos = 0;
	uint16_t len = 0;
	uint8_t in;

	static absolute_time_t to;
	to = make_timeout_time_us(1000000);
	while (len == 0) {
		readable = spi_is_readable(spi0);
		if (absolute_time_diff_us(to, get_absolute_time()) > 0) {
			g_st |= 0x400;
#if MC_DIS
			printf("Timeout: %d %llu %llu\n", pos, to, get_absolute_time());
#endif
			return 0;
		}
		if (!readable)
			continue;

		gpio_put(P_IRQB, 1);
		in = spi_get_hw(spi0)->dr;

		buf[pos] = in;
		if (pos == 0) {
			if (in == 0xF0) {
				pos++;
			}
		} else {
			/* HOTFIX: skip null status byte */
			if (pos == 1 && in == 0) {
				g_st |= 0x400;
#if MC_DIS
				printf("-Q-");
#endif
				continue;
			}

			/* TODO: BUF len limit */
			if (pos < BUF_LEN-1) {
				pos++;
			}

			if (in == 0xF7) {
				len = pos;
				pos = 0;
			}
		}
	}
	return len;
}

int transceive_request(const uint8_t *req, int reqlen, uint8_t *resp)
{
	int16_t len = 0;
	uint8_t u0;

	gpio_put(P_MUX_SEL_NSS1, 1);
	gpio_put(P_MUX_SEL_MISO0, 1);
	gpio_put(P_MUX_SEL_NIRQ0, 1);


#if MC_DIS
	printf("TR REQ %d", reqlen);
	printbuf(req, reqlen);
#endif

	/* Need read too for FIFO cleanup */
	while (spi_is_readable(spi0))
		u0 = spi_get_hw(spi0)->dr;

	gpio_put(P_IRQB, 0);

#if BR_DEBUG
	u0 = 0;
	for (len = 0; len < reqlen; len++) {
		if (req[len] == 0xf0)
			u0 |= 0x1;
		if (req[len] == 0xf7)
			u0 |= 0x2;
	}
	/* Error: no SOF/EOF in SysEx */
	if (u0 & 1 == 0)
		g_st |= 0x100;
	if (u0 & 2 == 0)
		g_st |= 0x200;
#endif

	while (spi_is_readable(spi0))
		u0 = spi_get_hw(spi0)->dr;

	spi_write_read_blocking(spi0, req, resp, reqlen);

	gpio_put(P_IRQB, 1);

	len = 0;
	while (len == 0) {
		len = read_response(resp);
	}
	len = read_response(resp);

	gpio_put(P_MUX_SEL_NSS1, 0);
	gpio_put(P_MUX_SEL_MISO0, 0);
	gpio_put(P_MUX_SEL_NIRQ0, 0);
	return len;
}

void core1_main()
{
	int i;
	uint8_t u0 = 0;
	uint8_t u1 = 0;
	uint8_t a0 = 0;
	uint8_t a1 = 0;

	uint8_t si;
	bool stream_ready;

	struct sysex_stream *ic0, *ic1, *ij_req, *ij_res;

	stream_ptrs *p = &ptrs;

	static int c1_inited = 0;

	if (c1_inited == 0) {
		c1_inited = 1;

		for (i = 0; i < 16; i++) {
			u0 = spi_get_hw(spi0)->dr;
			u1 = spi_get_hw(spi1)->dr;
		}
		for (i = 0; i < IC_STREAMS; i++) {
			stream_clear(&streams_ic0[i]);
			stream_clear(&streams_ic1[i]);
		}
	}

#if MC_EN
	while (1)
#endif
	{
		si = ic_stream_wrptr % IC_STREAMS;
		ic0 = &streams_ic0[si];
		ic1 = &streams_ic1[si];

		a0 = spi_is_readable(spi0);
		a1 = spi_is_readable(spi1);
		/* SPI are drived by the same CLK/nSS, thus should be synced */
		if (a0 || a1) {
			if (a0) {
				u0 = spi_get_hw(spi0)->dr;
			}
			if (a1) {
				u1 = spi_get_hw(spi1)->dr;
			}

			stream_ready = ic_stream_wrptr - ic_stream_rdptr < IC_STREAMS;

			if (stream_ready) {
				sysex_stream_check(ic0, u0);
				sysex_stream_check(ic1, u1);

				if (stream_full(ic0)) {
					__dmb();
					ic_stream_wrptr++;
				}
			}
		}

		/* TODO: Enable injecting request after DSPB init! */
		if (stream_cleared(ic0) && stream_cleared(ic1)) {
			stream_ready = ijreq_stream_wrptr != ijreq_stream_rdptr;
			si = ijreq_stream_rdptr % IJ_STREAMS;
			ij_req = &streams_ij_req[si];
			ij_res = &streams_ij_res[si];

			if (stream_ready && stream_full(ij_req) && gpio_get(P_IRQ1) == 1) {
				gpio_put(P_IRQB, 0);
				gpio_put(P_MUX_SEL_NIRQ0, 1);
				/* Paranoia */
				if (gpio_get(P_IRQ1) == 1) {
					gpio_put(P_MUX_SEL_NIRQ0, 0);
					gpio_put(P_IRQB, 1);
				} else {
					ij_res->len = transceive_request(ij_req->buf, ij_req->len, ij_res->buf);

					stream_clear(ij_req);

					__dmb();
					ijres_stream_wrptr++;
					ijreq_stream_rdptr++;
				}
			}
		}
	}
}

int main()
{
	int16_t i;
	int16_t len;
	int16_t ijres_tmp_pos = 0;
	uint8_t si;
	bool stream_ready;

	struct sysex_stream *s;

	board_init();
	tusb_init();

	init_hw();
#if MC_EN
	multicore_launch_core1(core1_main);
#endif

	stream_clear(&s_usbtmp);
	for (i = 0; i < IJ_STREAMS; i++) {
		stream_clear(&streams_ij_req[i]);
		stream_clear(&streams_ij_res[i]);
	}

	while (1) {
		tud_task();
		read_uart_cmd();
#if (!MC_EN)
		core1_main();
#endif

		/* Pull intercept streams (both streams are input, synchronized) and print */
		stream_ready = ic_stream_wrptr != ic_stream_rdptr;
		if (stream_ready) {
			si = ic_stream_rdptr % IC_STREAMS;

			s = &streams_ic1[si];
			if (do_echo & 1) {
				printf("FW Req %d\n", s->len);
				printbuf(s->buf, s->len);
			}
			stream_clear(s);

			s = &streams_ic0[si];
			if (do_echo & 1) {
				printf("FW Res %d\n", s->len);
				printbuf(s->buf, s->len);
			}
			stream_clear(s);

			__dmb();
			ic_stream_rdptr++;
		}

		/* Push inject stream readen from UART/USB-CDC */
		stream_ready = ijreq_stream_wrptr - ijreq_stream_rdptr < IJ_STREAMS;
		if (stream_ready) {
			si = ijreq_stream_wrptr % IJ_STREAMS;
			s = &streams_ij_req[si];
			s_usbtmp.len = tud_midi_n_stream_read(0, 0, s_usbtmp.buf, BUF_LEN);
			for (i = 0; i < s_usbtmp.len; i++) {
				len = sysex_stream_check(s, s_usbtmp.buf[i]);
				if (len > 0) {
					if (do_echo & 1)
						printf("USB Req %d\n", s->len);
					__dmb();
					ijreq_stream_wrptr++;
				}
			}
		}

		/* Pull inject stream and write to UART/USB-CDC */
		stream_ready = ijres_stream_wrptr != ijres_stream_rdptr;
		if (stream_ready) {
			si = ijres_stream_rdptr % IJ_STREAMS;
			s = &streams_ij_res[si];
			len = tud_midi_n_stream_write(0, 0, s->buf + ijres_tmp_pos, s->len - ijres_tmp_pos);
			ijres_tmp_pos += len;
			if (ijres_tmp_pos == s->len) {
				if (do_echo & 1)
					printf("USB Res %d\n", s->len);

				stream_clear(s);
				ijres_tmp_pos = 0;
				__dmb();
				ijres_stream_rdptr++;
			}
		}
	}

	return 0;
}
