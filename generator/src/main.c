#include <avr/interrupt.h>
#include <avr/io.h>

#include <slipdev.h>
#include <timer.h>
#include <uip.h>

#include "clock-arch.h"

#define UART_BAUDRATE   115200UL
#define UART_UBRR_VALUE ((F_CPU / (8UL * UART_BAUDRATE)) - 1)

typedef struct {
    volatile uint8_t buffer[128];
    volatile uint16_t rd;
    volatile uint16_t wr;
} fifo_t;

static fifo_t fifo_tx;
static fifo_t fifo_rx;
static volatile clock_time_t counter_ms = 0;

static void fifo_init(fifo_t *fifo) {
    fifo->rd = 0;
    fifo->wr = 0;
}

static inline uint16_t fifo_write(fifo_t *fifo, const uint8_t *src, uint16_t src_len) {
    uint16_t num = 0;
    const uint8_t sreg = SREG;
    cli();
    while(num < src_len) {
        uint16_t next = fifo->wr + 1;
        if(next >= sizeof(fifo->buffer)) {
            next = 0;
        }
        if(next == fifo->rd) {
            break;
        }
        fifo->buffer[fifo->wr] = src[num];
        fifo->wr = next;
        num++;
    }
    SREG = sreg;
    return num;
}

static inline uint16_t fifo_read(fifo_t *fifo, uint8_t *dst, const uint16_t dst_capacity) {
    uint16_t num = 0;
    const uint8_t sreg = SREG;
    cli();
    while((fifo->rd != fifo->wr) && (num < dst_capacity)) {
        dst[num] = fifo->buffer[fifo->rd];
        fifo->rd++;
        num++;
        if(fifo->rd >= sizeof(fifo->buffer)) {
            fifo->rd = 0;
        }
    }
    SREG = sreg;
    return num;
}

void slipdev_char_put(u8_t c) {
    if(fifo_write(&fifo_tx, &c, sizeof(c)) > 0) {
        UCSRB |= (1 << UDRIE);
    }
}

u8_t slipdev_char_poll(u8_t *c) {
    return (fifo_read(&fifo_rx, c, sizeof(*c)) > 0);
}

clock_time_t clock_time() {
    clock_time_t value;
    const uint8_t sreg = SREG;
    cli();
    value = counter_ms;
    SREG = sreg;
    return value;
}

ISR(USART_RXC_vect) {
    const uint8_t byte = UDR;
    fifo_write(&fifo_rx, &byte, sizeof(byte));
}

ISR(USART_UDRE_vect) {
    uint8_t byte;
    if(fifo_read(&fifo_tx, &byte, sizeof(byte)) > 0) {
        UDR = byte;
    } else {
        UCSRB &= ~(1 << UDRIE);
    }
}

ISR(TIMER1_COMPA_vect) {
    counter_ms++;
}

void telnetd_init();

int main() {
    fifo_init(&fifo_tx);
    fifo_init(&fifo_rx);

    UBRRH = (uint8_t)(UART_UBRR_VALUE >> 8);
    UBRRL = (uint8_t)(UART_UBRR_VALUE);
    UCSRA = (1 << U2X);
    UCSRC = (1 << URSEL) | (1 << UCSZ1) | (1 << UCSZ0);
    UCSRB = (1 << RXEN) | (1 << TXEN) | (1 << RXCIE);

    TCCR1A = 0;
    TCCR1B = (1 << WGM12);
    OCR1A = 249;
    TIMSK |= (1 << OCIE1A);
    TCCR1B |= (1 << CS11) | (1 << CS10);

    slipdev_init();
    uip_init();

    uip_ipaddr_t ip;
    uip_ipaddr(&ip, 192, 168, 9, 1);
    uip_sethostaddr(&ip);

    uip_ipaddr_t netmask;
    uip_ipaddr(&netmask, 255, 255, 255, 0);
    uip_setnetmask(&netmask);

    struct timer timer_periodic;
    timer_set(&timer_periodic, 10);

    telnetd_init();

    sei();

    while(1) {
        uip_len = slipdev_poll();
        if(uip_len > 0) {
            uip_input();
            if(uip_len > 0) {
                slipdev_send();
            }
        }

        if(timer_expired(&timer_periodic)) {
            timer_reset(&timer_periodic);
            for(uint8_t i = 0; i < UIP_CONNS; i++) {
                uip_periodic(i);
                if(uip_len > 0) {
                    slipdev_send();
                }
            }
        }
    }

    return 0;
}
