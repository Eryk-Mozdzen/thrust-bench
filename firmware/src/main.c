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

#include "ina226_regs.h"
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
} button_t;

typedef struct {
    fifo_t fifo_tx;
    fifo_t fifo_rx;
    state_t state;
    button_t button;
} context_t;

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern I2C_HandleTypeDef hi2c3;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
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
void MX_TIM1_Init();
void MX_TIM2_Init();
void MX_I2C3_Init();
void MX_USART2_UART_Init();
void MX_USART3_UART_Init();

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
    }
}

typedef enum {
    HX711_STATE_IDLE,
    HX711_STATE_SYNC,
    HX711_STATE_READ,
    HX711_STATE_END,
} hx711_state_t;

typedef struct {
    hx711_state_t state;
    uint32_t bits;
    uint32_t measurement[3];
} hx711_t;

static void hx711_init(hx711_t *hx711) {
    hx711->state = HX711_STATE_IDLE;

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
}

static void hx711_wait_us(const uint32_t time_us) {
    __HAL_TIM_SET_COUNTER(&htim1, 0);
    while(__HAL_TIM_GET_COUNTER(&htim1) < time_us) {
    }
}

static uint32_t hx711_read(hx711_t *hx711, int32_t result[3]) {
    switch(hx711->state) {
        case HX711_STATE_IDLE: {
            hx711->measurement[0] = 0;
            hx711->measurement[1] = 0;
            hx711->measurement[2] = 0;
            hx711->bits = 0;
            hx711->state = HX711_STATE_SYNC;
        } break;
        case HX711_STATE_SYNC: {
            if((HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_RESET) &&
               (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_RESET)) {
                hx711_wait_us(1);
                hx711->state = HX711_STATE_READ;
            }
        } break;
        case HX711_STATE_READ: {
            if(hx711->bits < 24) {
                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
                hx711_wait_us(5);

                hx711->measurement[0] <<= 1;
                hx711->measurement[1] <<= 1;
                hx711->measurement[2] <<= 1;

                hx711->measurement[0] |= (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_SET);
                hx711->measurement[1] |= (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_SET);
                hx711->measurement[2] |= (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET);

                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
                hx711_wait_us(5);

                hx711->bits++;
            } else {
                hx711->state = HX711_STATE_END;
            }
        } break;
        case HX711_STATE_END: {
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
            hx711_wait_us(5);
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_RESET);
            hx711_wait_us(5);

            hx711->measurement[0] ^= 0x800000;
            hx711->measurement[1] ^= 0x800000;
            hx711->measurement[2] ^= 0x800000;

            hx711->state = HX711_STATE_IDLE;

            result[0] = hx711->measurement[0];
            result[1] = hx711->measurement[1];
            result[2] = hx711->measurement[2];

            return 1;
        } break;
    }

    return 0;
}

#define PWR_R_SHUNT     0.0039f
#define PWR_MAX_CURRENT 30.f

typedef enum {
    INA226_STATE_IDLE,
    INA226_STATE_WAIT_FOR_VOLTAGE,
    INA226_STATE_WAIT_FOR_CURRENT,
} ina226_state_t;

static volatile uint8_t i2c_buffer[2];
static volatile uint8_t i2c_ready = 0;

typedef struct {
    ina226_state_t state;
    int16_t voltage;
    int16_t current;
} ina226_t;

static void ina226_write(uint8_t address, uint16_t value) {
    uint8_t reverse[2] = {(uint8_t)(value >> 8), (uint8_t)(value)};

    HAL_I2C_Mem_Write(&hi2c3, INA226_ADDR << 1, address, 1, reverse, 2, 100);
}

static void ina226_init(ina226_t *ina226) {
    ina226->state = INA226_STATE_IDLE;

    ina226_write(INA226_REG_CONFIGURATION, INA226_CONFIGURATION_RESET);

    HAL_Delay(100);

    ina226_write(INA226_REG_CONFIGURATION, INA226_CONFIGURATION_AVERAGE_1 |
                                               INA226_CONFIGURATION_BUS_VOLTAGE_CONV_1_1MS |
                                               INA226_CONFIGURATION_SHUNT_VOLTAGE_CONV_1_1MS |
                                               INA226_CONFIGURATION_MODE_CONTINUOUS_SHUNT_BUS);

    ina226_write(INA226_REG_MASK_ENABLE, INA226_MASK_ENABLE_CONVERSION_READY |
                                             INA226_MASK_ENABLE_ALERT_POLARITY_ACTIVE_LOW |
                                             INA226_MASK_ENABLE_ALERT_LATCH_TRANSPARENT);

    const uint16_t calib = INA226_CALIBRATION_VALUE(PWR_MAX_CURRENT, PWR_R_SHUNT);

    ina226_write(INA226_REG_CALIBRATION, calib);
}

static void ina226_read(ina226_t *ina226, float *voltage, float *current) {
    switch(ina226->state) {
        case INA226_STATE_IDLE: {
            ina226->state = INA226_STATE_WAIT_FOR_VOLTAGE;

            i2c_ready = 0;
            HAL_I2C_Mem_Read_IT(&hi2c3, INA226_ADDR << 1, INA226_REG_BUS_VOLTAGE, 1,
                                (uint8_t *)i2c_buffer, 2);
        } break;
        case INA226_STATE_WAIT_FOR_VOLTAGE: {
            if(i2c_ready) {
                ina226->voltage = (((uint16_t)i2c_buffer[0]) << 8) | i2c_buffer[1];
                ina226->state = INA226_STATE_WAIT_FOR_CURRENT;

                i2c_ready = 0;
                HAL_I2C_Mem_Read_IT(&hi2c3, INA226_ADDR << 1, INA226_REG_CURRENT, 1,
                                    (uint8_t *)i2c_buffer, 2);
            }
        } break;
        case INA226_STATE_WAIT_FOR_CURRENT: {
            if(i2c_ready) {
                ina226->current = (((uint16_t)i2c_buffer[0]) << 8) | i2c_buffer[1];

                *voltage = ina226->voltage * INA226_LSB_BUS_VOLTAGE;
                *current = ina226->current * INA226_LSB_CURRENT(PWR_MAX_CURRENT);

                ina226->state = INA226_STATE_IDLE;
            }
        } break;
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

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if(hi2c == &hi2c3) {
        i2c_ready = 1;
    }
}

int main() {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_TIM1_Init();
    MX_TIM2_Init();
    MX_I2C3_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();

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

    hx711_t hx711;
    hx711_init(&hx711);

    HAL_TIM_Base_Start(&htim1);
    HAL_TIM_Base_Start(&htim2);
    __HAL_TIM_SET_COUNTER(&htim2, 0);

    ina226_t ina226;
    ina226_init(&ina226);

    uint32_t prev1 = 0;
    uint32_t prev2 = 0;
    uint32_t prev3 = 0;
    uint32_t prev4 = 0;
    uint8_t send_buffer[1024];
    uint8_t recv_buffer[1024];

    float thrust = 0;
    float torque = 0;
    float velocity = 0;
    float voltage = 0;
    float current = 0;

    int32_t load_raw[3] = {0};
    int32_t load_offset[3] = {0};

    while(1) {
        const uint32_t timestamp = HAL_GetTick();

        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                          ((timestamp % 1000) < 50) ? GPIO_PIN_SET : GPIO_PIN_RESET);

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
                if((timestamp - prev1) >= 100) {
                    prev1 = timestamp;
                    const float frame[6] = {
                        thrust, torque, velocity, 0.f, voltage, current,
                    };
                    mqtt_publish(mqtt_client, "data", frame, sizeof(frame), 0, 0, NULL, NULL);
                }

                if((timestamp - prev2) >= 100) {
                    prev2 = timestamp;
                    const float frame[6] = {
                        thrust, torque, velocity, 0.f, voltage, current,
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
                load_offset[0] = load_raw[0];
                load_offset[1] = load_raw[1];
                load_offset[2] = load_raw[2];
                context.button = BUTTON_NONE;
            } break;
        }

        if(hx711_read(&hx711, load_raw)) {
            const int32_t load[3] = {
                load_raw[0] - load_offset[0],
                load_raw[1] - load_offset[1],
                load_raw[2] - load_offset[2],
            };

            const float g = 9.8067f;
            const float arm = 0.270f;
            const float k1 = 0.265f / -105000.f;
            const float k2 = 0.2115f / -1711939.f;

            thrust = load[0] * k1 * g;
            torque = 0.5f * (load[1] + load[2]) * k2 * arm * g;
        }

        if((timestamp - prev3) >= 100) {
            const uint32_t rotations = __HAL_TIM_GET_COUNTER(&htim2);
            const float delta = 0.001f * (timestamp - prev3);
            velocity = 6.283185307f * rotations / delta;
            prev3 = timestamp;
            __HAL_TIM_SET_COUNTER(&htim2, 0);
        }

        if((timestamp - prev4) >= 50) {
            prev4 = timestamp;
            ina226_read(&ina226, &voltage, &current);
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
