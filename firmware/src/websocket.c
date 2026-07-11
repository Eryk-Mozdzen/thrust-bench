#include <stdint.h>
#include <string.h>

#include <lwip/tcp.h>
#include <netif/ppp/polarssl/sha1.h>

#include "websocket.h"

typedef enum {
    OPCODE_CONTINUATION_FRAME = 0,
    OPCODE_NON_CONTROL_FRAME_TEXT = 1,
    OPCODE_NON_CONTROL_FRAME_BINARY = 2,
    OPCODE_CONTROL_FRAME_CLOSE = 8,
    OPCODE_CONTROL_FRAME_PING = 9,
    OPCODE_CONTROL_FRAME_PONG = 10,
} opcode_t;

static uint32_t base64_encode(char *dest, const uint8_t *input, const uint32_t input_length) {
    const char encoding_table[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
    };

    const uint32_t mod_table[] = {0, 2, 1};

    const uint32_t dest_length = 4 * ((input_length + 2) / 3);

    for(uint32_t i = 0, j = 0; i < input_length;) {
        const uint32_t octet_a = (i < input_length) ? input[i++] : 0;
        const uint32_t octet_b = (i < input_length) ? input[i++] : 0;
        const uint32_t octet_c = (i < input_length) ? input[i++] : 0;

        const uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        dest[j++] = encoding_table[(triple >> (3 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (2 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (1 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (0 * 6)) & 0x3F];
    }

    for(uint32_t i = 0; i < mod_table[input_length % 3]; i++) {
        dest[dest_length - 1 - i] = '=';
    }

    return dest_length;
}

static err_t websocket_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    websocket_t *ws = arg;

    if(p == NULL) {
        tcp_close(pcb);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);

    const char *data = p->payload;

    switch(ws->state) {
        case WEBSOCKET_STATE_CONNECTING: {
            char key[24] = {0};

            const char *token = data;
            const char *newline = NULL;
            do {
                newline = strchr(token, '\n');
                const size_t token_len = newline - token - 1;

                const char *key_header = "Sec-WebSocket-Key";
                const uint32_t key_header_len = strlen(key_header);
                const uint32_t key_len = 24;

                if(token_len >= (key_header_len + key_len)) {
                    if(memcmp(token, key_header, key_header_len) == 0) {
                        memcpy(key, token + token_len - key_len, key_len);
                    }
                }

                token = newline + 1;
            } while(newline != NULL);

            if(key[0] != 0) {
                const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

                unsigned char sha1[20] = {0};
                sha1_context sha1_ctx;
                sha1_starts(&sha1_ctx);
                sha1_update(&sha1_ctx, (unsigned char *)key, sizeof(key));
                sha1_update(&sha1_ctx, (unsigned char *)magic, 36);
                sha1_finish(&sha1_ctx, sha1);

                char response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Sec-WebSocket-Accept: ????????????????????????????\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Upgrade: websocket\r\n"
                                  "\r\n";

                base64_encode(&response[56], sha1, sizeof(sha1));

                if(tcp_write(pcb, response, strlen(response), TCP_WRITE_FLAG_COPY) == ERR_OK) {
                    tcp_output(pcb);
                    ws->state = WEBSOCKET_STATE_OPEN;
                }
            }
        } break;
        case WEBSOCKET_STATE_OPEN: {

        } break;
        case WEBSOCKET_STATE_CLOSING: {

        } break;
        case WEBSOCKET_STATE_CLOSED: {

        } break;
    }

    pbuf_free(p);

    return ERR_OK;
}

static err_t websocket_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    websocket_t *ws = arg;
    ws->pcb_client = newpcb;
    tcp_arg(ws->pcb_client, arg);
    tcp_recv(ws->pcb_client, websocket_recv);
    return ERR_OK;
}

void websocket_init(websocket_t *ws, const uint32_t port) {
    ws->pcb_listen = tcp_new();
    ws->pcb_client = NULL;
    ws->state = WEBSOCKET_STATE_CONNECTING;

    if(ws->pcb_listen == NULL) {
        return;
    }

    if(tcp_bind(ws->pcb_listen, IP_ADDR_ANY, port) != ERR_OK) {
        tcp_close(ws->pcb_listen);
        return;
    }

    ws->pcb_listen = tcp_listen(ws->pcb_listen);

    tcp_arg(ws->pcb_listen, ws);
    tcp_accept(ws->pcb_listen, websocket_accept);
}

void websocket_write(websocket_t *ws, const void *data, const uint32_t size) {
    if((ws->pcb_client != NULL) && (ws->state == WEBSOCKET_STATE_OPEN)) {
        uint8_t frame[256] = {0};

        const uint32_t header_len = 2;
        const uint32_t data_len = (size <= 125) ? size : 125;
        const uint32_t frame_len = header_len + data_len;

        frame[0] |= (0x01 << 7);
        frame[0] |= (OPCODE_NON_CONTROL_FRAME_BINARY << 0);
        frame[1] |= (data_len << 0);

        memcpy(frame + header_len, data, data_len);

        if(tcp_write(ws->pcb_client, frame, frame_len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(ws->pcb_client);
        }
    }
}

websocket_state_t websocket_state(websocket_t *ws) {
    return ws->state;
}
