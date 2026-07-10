#include "ai_uart_i2s_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "audio_play_api.h"
#include "board.h"
#include "ci130x_audio_pre_rslt_out.h"
#include "ci130x_core_eclic.h"
#include "ci130x_uart.h"
#include "ci_assert.h"
#include "codec_manager.h"
#include "system_msg_deal.h"
#include "user_config.h"

#define AI_UART_PROTOCOL_VERSION 0x04
#define AI_UART_SOF0 0xA5
#define AI_UART_SOF1 0x5A
#define AI_UART_MAX_PAYLOAD 48
#define AI_UART_RX_MAX_PAYLOAD 64
#define AI_UART_HEARTBEAT_MS 1000
#define AI_UART_PEER_TIMEOUT_MS 3000

#define AI_UART_MSG_ACK 0x03
#define AI_UART_MSG_PING 0x05
#define AI_UART_MSG_PONG 0x06
#define AI_UART_MSG_WAKE_DETECTED 0x10
#define AI_UART_MSG_DING_DONE 0x11
#define AI_UART_MSG_UPLINK_READY 0x12
#define AI_UART_MSG_STATE 0x16
#define AI_UART_MSG_START_DOWNLINK 0x22
#define AI_UART_MSG_STOP_DOWNLINK 0x23
#define AI_UART_MSG_ENTER_WAKEUP_WAIT 0x25

#define AI_UART_ACK_OK 0x00
#define AI_UART_ACK_UNSUPPORTED 0x02
#define AI_UART_ACK_FAILED 0x03

#define AI_UART_STATE_WAKEUP_WAIT 0x01
#define AI_UART_STATE_LISTENING 0x02
#define AI_UART_STATE_DOWNLINK_PLAYING 0x04

typedef struct
{
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint8_t payload[AI_UART_RX_MAX_PAYLOAD];
} ai_uart_command_t;

static QueueHandle_t command_queue;
static SemaphoreHandle_t send_mutex;
static volatile uint8_t peer_ready;
static volatile uint8_t current_state = AI_UART_STATE_WAKEUP_WAIT;
static volatile uint8_t downlink_enabled;
static volatile uint8_t downlink_codec_started;
static volatile TickType_t last_peer_tick;
static uint8_t tx_seq;
static uint32_t downlink_bytes;
static volatile uint32_t dropped_commands;

static uint8_t crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    for(uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for(uint8_t bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void send_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[AI_UART_MAX_PAYLOAD + 8];
    uint16_t pos = 0;
    if(len > AI_UART_MAX_PAYLOAD)
    {
        return;
    }
    if(send_mutex)
    {
        xSemaphoreTake(send_mutex, portMAX_DELAY);
    }
    frame[pos++] = AI_UART_SOF0;
    frame[pos++] = AI_UART_SOF1;
    frame[pos++] = AI_UART_PROTOCOL_VERSION;
    frame[pos++] = type;
    frame[pos++] = tx_seq++;
    frame[pos++] = (uint8_t)(len & 0xff);
    frame[pos++] = (uint8_t)(len >> 8);
    if(len)
    {
        memcpy(&frame[pos], payload, len);
        pos += len;
    }
    frame[pos++] = crc8(&frame[2], (uint16_t)(pos - 2));
    for(uint16_t i = 0; i < pos; i++)
    {
        UartPollingSenddata((UART_TypeDef *)AI_UART_CONTROL_UART, frame[i]);
    }
    if(send_mutex)
    {
        xSemaphoreGive(send_mutex);
    }
}

static void send_ack(uint8_t seq, uint8_t status)
{
    uint8_t payload[2] = {seq, status};
    send_frame(AI_UART_MSG_ACK, payload, sizeof(payload));
}

static void send_state(uint8_t state)
{
    current_state = state;
    send_frame(AI_UART_MSG_STATE, &state, 1);
}

static void send_uplink_ready(void)
{
    static const uint8_t payload[5] = {0x80, 0x3e, 0x00, 0x10, 0x02};
    send_frame(AI_UART_MSG_UPLINK_READY, payload, sizeof(payload));
}

static void stop_downlink(void)
{
    downlink_enabled = 0;
    if(downlink_codec_started)
    {
        cm_stop_codec(PLAY_PRE_AUDIO_CODEC_ID, CODEC_INPUT);
        downlink_codec_started = 0;
    }
    mprintf("[DOWNLINK] stopped bytes=%u\n", (unsigned int)downlink_bytes);
}

static void downlink_task(void *arg)
{
    (void)arg;
    for(;;)
    {
        uint32_t input_addr = 0;
        uint32_t input_size = 0;
        if(!downlink_enabled)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        cm_read_codec(PLAY_PRE_AUDIO_CODEC_ID, &input_addr, &input_size);
        if(downlink_enabled && input_addr && input_size &&
           AUDIO_PLAY_OS_SUCCESS == audio_play_hw_write_data((void *)input_addr, input_size))
        {
            downlink_bytes += input_size;
        }
    }
}

static uint8_t start_downlink(void)
{
    if(downlink_enabled)
    {
        return 1;
    }
    stop_play(NULL, NULL);
    downlink_bytes = 0;
    cm_start_codec(PLAY_PRE_AUDIO_CODEC_ID, CODEC_INPUT);
    cm_start_codec(PLAY_CODEC_ID, CODEC_OUTPUT);
    downlink_codec_started = 1;
    downlink_enabled = 1;
    send_state(AI_UART_STATE_DOWNLINK_PLAYING);
    mprintf("[DOWNLINK] started owner=ai_uart\n");
    return 1;
}

static void mark_peer_rx(void)
{
    uint8_t was_ready = peer_ready;
    peer_ready = 1;
    last_peer_tick = xTaskGetTickCount();
    if(!was_ready)
    {
        audio_pre_rslt_start();
        mprintf("[AI_UART] peer ready, continuous uplink enabled\n");
    }
}

static void handle_command(const ai_uart_command_t *cmd)
{
    mark_peer_rx();
    switch(cmd->type)
    {
    case AI_UART_MSG_PING:
        send_frame(AI_UART_MSG_PONG, cmd->payload, cmd->len);
        break;
    case AI_UART_MSG_PONG:
        break;
    case AI_UART_MSG_START_DOWNLINK:
        send_ack(cmd->seq, start_downlink() ? AI_UART_ACK_OK : AI_UART_ACK_FAILED);
        break;
    case AI_UART_MSG_STOP_DOWNLINK:
        send_ack(cmd->seq, AI_UART_ACK_OK);
        stop_downlink();
        send_state(AI_UART_STATE_LISTENING);
        break;
    case AI_UART_MSG_ENTER_WAKEUP_WAIT:
        send_ack(cmd->seq, AI_UART_ACK_OK);
        stop_downlink();
        set_state_exit_wakeup();
        send_state(AI_UART_STATE_WAKEUP_WAIT);
        break;
    default:
        send_ack(cmd->seq, AI_UART_ACK_UNSUPPORTED);
        break;
    }
}

static void post_command_from_isr(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    ai_uart_command_t cmd;
    BaseType_t higher_priority_task_woken = pdFALSE;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.seq = seq;
    cmd.len = len;
    if(len)
    {
        memcpy(cmd.payload, payload, len);
    }
    if(pdPASS != xQueueSendFromISR(command_queue, &cmd, &higher_priority_task_woken))
    {
        dropped_commands++;
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void handle_rx_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    static const uint8_t heartbeat[3] = {0x06, 0x13, 0x01};
    if((type == AI_UART_MSG_PING || type == AI_UART_MSG_PONG) &&
       len == sizeof(heartbeat) && 0 == memcmp(payload, heartbeat, sizeof(heartbeat)))
    {
        post_command_from_isr(type, seq, payload, len);
        return;
    }
    post_command_from_isr(type, seq, payload, len);
}

static void rx_byte(uint8_t byte)
{
    enum { RX_SOF0, RX_SOF1, RX_VER, RX_TYPE, RX_SEQ, RX_LEN_L, RX_LEN_H, RX_PAYLOAD, RX_CRC };
    static uint8_t state = RX_SOF0;
    static uint8_t type;
    static uint8_t seq;
    static uint16_t len;
    static uint16_t pos;
    static uint8_t payload[AI_UART_RX_MAX_PAYLOAD];
    static uint8_t crc_data[AI_UART_RX_MAX_PAYLOAD + 5];
    switch(state)
    {
    case RX_SOF0: state = byte == AI_UART_SOF0 ? RX_SOF1 : RX_SOF0; break;
    case RX_SOF1: state = byte == AI_UART_SOF1 ? RX_VER : RX_SOF0; break;
    case RX_VER:
        if(byte != AI_UART_PROTOCOL_VERSION) { state = RX_SOF0; break; }
        crc_data[0] = byte; state = RX_TYPE; break;
    case RX_TYPE: type = byte; crc_data[1] = byte; state = RX_SEQ; break;
    case RX_SEQ: seq = byte; crc_data[2] = byte; state = RX_LEN_L; break;
    case RX_LEN_L: len = byte; crc_data[3] = byte; state = RX_LEN_H; break;
    case RX_LEN_H:
        len |= (uint16_t)byte << 8; crc_data[4] = byte; pos = 0;
        state = len > AI_UART_RX_MAX_PAYLOAD ? RX_SOF0 : (len ? RX_PAYLOAD : RX_CRC);
        break;
    case RX_PAYLOAD:
        payload[pos] = byte; crc_data[5 + pos] = byte;
        if(++pos >= len) { state = RX_CRC; }
        break;
    case RX_CRC:
        if(byte == crc8(crc_data, (uint16_t)(5 + len))) { handle_rx_frame(type, seq, payload, len); }
        state = RX_SOF0; break;
    default: state = RX_SOF0; break;
    }
}

static void uart_irq_handler(void)
{
    UART_TypeDef *uart = (UART_TypeDef *)AI_UART_CONTROL_UART;
    if(uart->UARTMIS & (1UL << UART_RXInt))
    {
        rx_byte(UART_RXDATA(uart));
        UART_IntClear(uart, UART_RXInt);
    }
    UART_IntClear(uart, UART_AllInt);
}

int ai_uart_i2s_peer_ready(void)
{
    return peer_ready ? 1 : 0;
}

static void protocol_task(void *arg)
{
    ai_uart_command_t cmd;
    TickType_t last_ping = 0;
    static const uint8_t heartbeat[3] = {0x06, 0x13, 0x01};
    (void)arg;
    for(;;)
    {
        TickType_t now = xTaskGetTickCount();
        if(pdPASS == xQueueReceive(command_queue, &cmd, pdMS_TO_TICKS(AI_UART_TASK_POLL_MS)))
        {
            handle_command(&cmd);
        }
        now = xTaskGetTickCount();
        if((now - last_ping) >= pdMS_TO_TICKS(AI_UART_HEARTBEAT_MS))
        {
            last_ping = now;
            send_frame(AI_UART_MSG_PING, heartbeat, sizeof(heartbeat));
        }
        if(peer_ready && (now - last_peer_tick) >= pdMS_TO_TICKS(AI_UART_PEER_TIMEOUT_MS))
        {
            peer_ready = 0;
            stop_downlink();
            audio_pre_rslt_stop();
            current_state = AI_UART_STATE_WAKEUP_WAIT;
            mprintf("[AI_UART] peer timeout\n");
        }
        if(dropped_commands)
        {
            uint32_t dropped = dropped_commands;
            dropped_commands = 0;
            mprintf("[AI_UART] command queue overflow dropped=%u\n", (unsigned int)dropped);
        }
    }
}

void ai_uart_i2s_protocol_init(void)
{
    BaseType_t task_created;
    command_queue = xQueueCreate(8, sizeof(ai_uart_command_t));
    send_mutex = xSemaphoreCreateMutex();
    CI_ASSERT(command_queue && send_mutex, "ai uart resources\n");
    __eclic_irq_set_vector(UART1_IRQn, (int32_t)uart_irq_handler);
    UARTInterruptConfig((UART_TypeDef *)AI_UART_CONTROL_UART, AI_UART_CONTROL_BAUDRATE);
    task_created = xTaskCreate(protocol_task, "ai_uart", 512, NULL, 3, NULL);
    CI_ASSERT(pdPASS == task_created, "ai uart task\n");
    task_created = xTaskCreate(downlink_task, "ai_downlink", 512, NULL, 4, NULL);
    CI_ASSERT(pdPASS == task_created, "ai downlink task\n");
    send_state(AI_UART_STATE_WAKEUP_WAIT);
}

void ai_uart_i2s_on_wake(uint16_t wake_id)
{
    uint8_t payload[2] = {(uint8_t)(wake_id & 0xff), (uint8_t)(wake_id >> 8)};
    if(downlink_enabled || AUDIO_PLAY_STATE_IDLE != get_audio_play_state())
    {
        stop_play(NULL, NULL);
        stop_downlink();
    }
    send_frame(AI_UART_MSG_WAKE_DETECTED, payload, sizeof(payload));
    send_state(AI_UART_STATE_LISTENING);
    send_uplink_ready();
}

void ai_uart_i2s_on_ding_done(void)
{
    send_frame(AI_UART_MSG_DING_DONE, NULL, 0);
}
