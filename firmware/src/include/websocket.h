#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>

#include <lwip/tcp.h>

typedef enum {
    WEBSOCKET_STATE_CONNECTING,
    WEBSOCKET_STATE_OPEN,
    WEBSOCKET_STATE_CLOSING,
    WEBSOCKET_STATE_CLOSED,
} websocket_state_t;

typedef struct {
    struct tcp_pcb *pcb_listen;
    struct tcp_pcb *pcb_client;
    websocket_state_t state;
} websocket_t;

void websocket_init(websocket_t *ws, const uint32_t port);
void websocket_write(websocket_t *ws, const void *data, const uint32_t size);
websocket_state_t websocket_state(websocket_t *ws);

#endif
