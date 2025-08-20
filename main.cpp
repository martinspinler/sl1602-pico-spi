#include <stdio.h>

#include <cstdint>

#include "hardware/spi.h"
#include "hardware/clocks.h"
#include "pico/binary_info.h"
#include "pico/stdlib.h"

#include "mux.pio.h"


#define P_IRQ1  12 // Input from DSPB
#define P_IRQB  13 // Output to CPB

#define P_NSS1_DSPB     19 // Injected nSS1

#define P_MUX_SEL_NIRQ0 20
#define P_MUX_SEL_MOSI1 21
#define P_MUX_SEL_MISO0 22
#define P_MUX_SEL_NSS1  26


#define BUF_LEN 128


struct sysex_stream {
	uint8_t buf[BUF_LEN];
	int16_t pos;
//	int16_t len;
	uint8_t in;
};

const uint8_t g_buf0[1] = {0};

struct sysex_stream s_fw;
struct sysex_stream s_usb;

uint sm[4];
uint offset[4];


void printbuf(uint8_t buf[], size_t len)
{
	size_t i;
	for (i = 0; i < len; ++i) {
		if (i % 16 == 15)
			printf("%02x\n", buf[i]);
		else
			printf("%02x ", buf[i]);
	}

	// append trailing newline if there isn't one
	if (i % 16) {
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

int16_t sysex_stream_check(struct sysex_stream *s)
{
	uint16_t len;
	if (s->in == 0xF0) {
		s->buf[0] = s->in;
		s->pos = 1;
		return 0;
	} else if (s->pos > 0) {
		s->buf[s->pos] = s->in;
		s->pos++;

		if (s->in == 0xF7) {
			len = s->pos;
			s->pos = 0;
			return len;
		} else if (s->pos >= BUF_LEN) {
			s->pos = 0;
			return -1;
		} else {
			return 0;
		}
	}
	return -1;
}

int16_t read_firewire_request(struct sysex_stream *s)
{
	int16_t len = 0;

	//s->pos = 0;
	while (gpio_get(P_IRQ1) == 0 && len == 0) {
		printf("R");
		spi_write_read_blocking(spi1, g_buf0, &s->in, 1);
		printf("%02x\n", s->in);
		len = sysex_stream_check(s);
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

int16_t read_uart_request(struct sysex_stream *s)
{
	int16_t len = 0;
	int c;

	c = stdio_getchar_timeout_us(0);
	while (c >= 0) {
		s->in = c;
		/* TODO: timeout */
		len = sysex_stream_check(s);
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

int read_response(uint8_t *buf)
{
	uint16_t pos = 0;
	uint16_t len = 0;
	uint8_t in;

	while (len == 0) {
		spi_get_hw(spi0)->dr = 0;
		while (spi_is_readable(spi0) == 0);
		in = spi_get_hw(spi0)->dr;

		buf[pos] = in;
		if (pos == 0) {
			if (in == 0xF0) {
				pos++;
			}
		} else {
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

	gpio_put(P_MUX_SEL_NSS1, 1);
	gpio_put(P_MUX_SEL_MISO0, 1);
	gpio_put(P_MUX_SEL_NIRQ0, 1);

	gpio_put(P_IRQB, 0);

	spi_write_blocking(spi0, req, reqlen);

	gpio_put(P_IRQB, 1);

	while (len == 0) {
		len = read_response(resp);
	}

	gpio_put(P_MUX_SEL_NSS1, 0);
	gpio_put(P_MUX_SEL_MISO0, 0);
	gpio_put(P_MUX_SEL_NIRQ0, 0);
	return len;
}

int main()
{
	int16_t len;
	int in = 0 ;
	uint8_t u0 = 0;
	uint8_t u1 = 0;
	uint8_t a0 = 0;
	uint8_t a1 = 0;
	int is_inited = 0;
	int can_send = 0;
	int msSinceBoot;
	int state = 0;

	init_hw();

	while (1) {
#if 0
		a0 = spi_is_readable(spi0);
		a1 = spi_is_readable(spi1);
		if (a0 || a1) {
			/* SPI are drived by the same CLK/nSS, thus should be synced */
			//while (spi_is_readable(spi1) == 0);

			u0 = spi_get_hw(spi0)->dr;
			u1 = spi_get_hw(spi1)->dr;

			//printf("0:%c %02x 1:%c %02x\n", a0 ? 'X': '.', u0, a1 ? 'X': '.', u1);
		}
#endif
#if 0
		if (gpio_get(P_IRQ1) == 0) {
			len = read_firewire_request(&s_fw);
			if (len > 0) {
				printf("FW Req\n");
				printbuf(s_fw.buf, len);

				send_request(s_fw.buf, len);
				len = read_response(s_fw.buf);
				if (len > 0) {
					printf("FW Resp\n");
					printbuf(s_fw.buf, len);
					send_firewire_response(&s_fw, len);
				}
			}
		}
#endif
		len = read_uart_request(&s_usb);
		if (len > 0) {
			len = transceive_request(s_usb.buf, len, s_usb.buf);
			if (len > 0) {
				send_uart_response(&s_usb, len);
			}
		}
	}

	return 0;
}
