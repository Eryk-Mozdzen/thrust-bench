#include <math.h>
#include <string.h>

#include <stm32u0xx_hal.h>

#include <lwip/apps/fs.h>
#include <lwip/apps/httpd.h>
#include <lwip/apps/lwiperf.h>
#include <lwip/apps/mqtt.h>
#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>
#include <netif/ppp/ppp.h>
#include <netif/ppp/pppos.h>

#include "websocket.h"

typedef struct {
    uint8_t buffer[1024];
    volatile uint32_t rd;
    volatile uint32_t wr;
} fifo_t;

typedef enum {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_WAITING_MQTT,
    STATE_LOOP,
} state_t;

typedef enum {
    BUTTON_NONE,
    BUTTON_OFFSET,
    BUTTON_THRUST,
    BUTTON_TORQUE,
} button_t;

typedef struct {
    fifo_t fifo_tx;
    fifo_t fifo_rx;
    state_t state;
    button_t button;
} context_t;

extern UART_HandleTypeDef huart2;
extern RNG_HandleTypeDef hrng;
extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];
extern const char _binary_style_css_start[];
extern const char _binary_style_css_end[];
extern const char _binary_script_js_start[];
extern const char _binary_script_js_end[];

static volatile uint8_t send_ready = 1;
static volatile uint8_t recv_byte;
static context_t context;

void SystemClock_Config();
void MX_GPIO_Init();
void MX_USART2_UART_Init();
void MX_RNG_Init();

static void fifo_init(fifo_t *fifo) {
    fifo->rd = 0;
    fifo->wr = 0;
}

static uint32_t fifo_write(fifo_t *fifo, const uint8_t *src, uint32_t src_len) {
    uint32_t num = 0;
    while(num < src_len) {
        uint32_t next = fifo->wr + 1;
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
    return num;
}

static uint32_t fifo_read(fifo_t *fifo, uint8_t *dst, const uint32_t dst_capacity) {
    uint32_t num = 0;
    while((fifo->rd != fifo->wr) && (num < dst_capacity)) {
        dst[num] = fifo->buffer[fifo->rd];
        fifo->rd++;
        num++;
        if(fifo->rd >= sizeof(fifo->buffer)) {
            fifo->rd = 0;
        }
    }
    return num;
}

static void ppp_link_status_cb(ppp_pcb *pcb, int err_code, void *ctx) {
    (void)pcb;

    context_t *context = ctx;

    if(err_code == PPPERR_NONE) {
        if(context->state == STATE_CONNECTING) {
            context->state = STATE_CONNECTED;
        }
    }
}

static uint32_t ppp_output_cb(ppp_pcb *pcb, const void *data, uint32_t data_size, void *ctx) {
    (void)pcb;

    context_t *context = ctx;

    return fifo_write(&context->fifo_tx, data, data_size);
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    context_t *context = arg;

    if(status == MQTT_CONNECT_ACCEPTED) {
        context->state = STATE_LOOP;
    }
}

static void ws_on_message_cb(void *arg,
                             struct websocket_client *client,
                             const void *message,
                             u32_t message_len) {
    (void)client;
    (void)message_len;

    context_t *context = arg;

    if(strcmp(message, "offset") == 0) {
        context->button = BUTTON_OFFSET;
    } else if(strcmp(message, "thrust") == 0) {
        context->button = BUTTON_THRUST;
    } else if(strcmp(message, "torque") == 0) {
        context->button = BUTTON_TORQUE;
    }
}

sys_prot_t sys_arch_protect() {
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

uint32_t sys_now() {
    return HAL_GetTick();
}

uint32_t sys_jiffies() {
    return HAL_GetTick();
}

uint32_t sys_rng() {
    uint32_t random = 0;
    HAL_RNG_GenerateRandomNumber(&hrng, &random);
    return random;
}

int fs_open_custom(struct fs_file *file, const char *name) {
    if((strcmp(name, "") == 0) || (strcmp(name, "/") == 0)) {
        name = "index.html";
    }

    if(strcmp(name, "/index.html") == 0) {
        file->data = _binary_index_html_start;
        file->len = _binary_index_html_end - _binary_index_html_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    if(strcmp(name, "/style.css") == 0) {
        file->data = _binary_style_css_start;
        file->len = _binary_style_css_end - _binary_style_css_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    if(strcmp(name, "/script.js") == 0) {
        file->data = _binary_script_js_start;
        file->len = _binary_script_js_end - _binary_script_js_start;
        file->index = 0;
        file->flags = 0;
        return 1;
    }

    return 0;
}

void fs_close_custom(struct fs_file *file) {
    (void)file;
}

int fs_read_custom(struct fs_file *file, char *buffer, int count) {
    int read = file->len - file->index;
    if(read > count) {
        read = count;
    }
    memcpy(buffer, (file->data + file->index), read);
    file->index += read;
    return read;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == &huart2) {
        send_ready = 1;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart == &huart2) {
        fifo_write(&context.fifo_rx, (uint8_t *)&recv_byte, 1);
        HAL_UART_Receive_IT(&huart2, (uint8_t *)&recv_byte, 1);
    }
}

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    MX_RNG_Init();

    lwip_init();

    struct netif netif = {0};
    ppp_pcb *ppp = pppos_create(&netif, ppp_output_cb, ppp_link_status_cb, &context);
    ppp_set_default(ppp);

    mqtt_client_t *mqtt_client = NULL;
    websocket_server_t *ws_server = NULL;

    context.state = STATE_DISCONNECTED;
    fifo_init(&context.fifo_tx);
    fifo_init(&context.fifo_rx);
    HAL_UART_Receive_IT(&huart2, (uint8_t *)&recv_byte, 1);

    uint32_t prev1 = 0;
    uint32_t prev2 = 0;
    uint8_t send_buffer[1024];
    uint8_t recv_buffer[1024];

    while(1) {
        const uint32_t timestamp = HAL_GetTick();

        // HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
        //                   ((timestamp % 1000) < 50) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        switch(context.state) {
            case STATE_DISCONNECTED: {
                context.state = STATE_CONNECTING;
                ppp_connect(ppp, 0);
            } break;
            case STATE_CONNECTING: {

            } break;
            case STATE_CONNECTED: {
                ip4_addr_t mqtt_broker;
                IP4_ADDR(&mqtt_broker, 192, 168, 7, 1);

                const struct mqtt_connect_client_info_t mqtt_client_info = {
                    .client_id = "lwip_client",
                    .client_user = NULL,
                    .client_pass = NULL,
                    .keep_alive = 60,
                    .will_topic = NULL,
                    .will_msg = NULL,
                    .will_msg_len = 0,
                    .will_qos = 0,
                    .will_retain = 0,
                };

                mqtt_client = mqtt_client_new();
                mqtt_client_connect(mqtt_client, &mqtt_broker, 1883, mqtt_connection_cb, &context,
                                    &mqtt_client_info);

                lwiperf_start_tcp_server_default(NULL, NULL);

                httpd_init();

                ws_server = websocket_server_new(81);
                websocket_arg(ws_server, &context);
                websocket_on_message(ws_server, ws_on_message_cb);

                context.state = STATE_WAITING_MQTT;
            } break;
            case STATE_WAITING_MQTT: {

            } break;
            case STATE_LOOP: {
                if((timestamp - prev1) >= 1000) {
                    prev1 = timestamp;
                    const char *json = "{ \"field\": 69 }";
                    mqtt_publish(mqtt_client, "test", json, strlen(json), 0, 0, NULL, NULL);
                }

                if((timestamp - prev2) >= 100) {
                    prev2 = timestamp;
                    const int16_t frame[6] = {
                        1.23f + sinf(0.628f * 0.001f * timestamp) * 10,
                        0.45f + sinf(0.5f * 0.001f * timestamp) * 10,
                        678.f + sinf(1.f * 0.001f * timestamp) * 10,
                        47.f + sinf(1.2f * 0.001f * timestamp) * 10,
                        16.8f + sinf(0.1f * 0.001f * timestamp) * 10,
                        2.56f + sinf(0.01f * 0.001f * timestamp) * 10,
                    };
                    for(struct websocket_client *client = ws_server->clients; client != NULL;
                        client = client->next) {
                        websocket_send(client, &frame, sizeof(frame));
                    }
                }
            } break;
        }

        switch(context.button) {
            case BUTTON_NONE: {

            } break;
            case BUTTON_OFFSET: {
                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
                context.button = BUTTON_NONE;
            } break;
            case BUTTON_THRUST: {
                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
                context.button = BUTTON_NONE;
            } break;
            case BUTTON_TORQUE: {
                HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
                context.button = BUTTON_NONE;
            } break;
        }

        const uint32_t recv_len = fifo_read(&context.fifo_rx, recv_buffer, sizeof(recv_buffer));
        if(recv_len > 0) {
            pppos_input(ppp, recv_buffer, recv_len);
        }

        if(send_ready) {
            const uint32_t send_len = fifo_read(&context.fifo_tx, send_buffer, sizeof(send_buffer));
            if(send_len > 0) {
                send_ready = 0;
                HAL_UART_Transmit_IT(&huart2, send_buffer, send_len);
            }
        }

        sys_check_timeouts();
    }

    return 0;
}
