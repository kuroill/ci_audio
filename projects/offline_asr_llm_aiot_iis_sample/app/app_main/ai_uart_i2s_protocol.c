#include "ai_uart_i2s_protocol.h"

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "audio_play_api.h"
#include "board.h"
#include "ci130x_audio_pre_rslt_out.h"
#include "ci130x_core_eclic.h"
#include "ci130x_uart.h"
#include "ci_assert.h"
#include "codec_manager.h"
#include "status_share.h"
#include "system_msg_deal.h"
#include "user_config.h"
#include "cias_audio_data_handle.h"
#include "ci130x_dpmu.h"
#include "ci_flash_data_info.h"

#define AI_UART_PROTOCOL_VERSION 0x04
#define AI_UART_SOF0 0xA5
#define AI_UART_SOF1 0x5A
#define AI_UART_MAX_PAYLOAD 48
#define AI_UART_RX_MAX_PAYLOAD 64
#define AI_UART_HEARTBEAT_MS 1000
#define AI_UART_PEER_TIMEOUT_MS 3000
#define AI_PLAY_STOP_WAIT_MS 250

#define AI_UART_MSG_ACK 0x03
#define AI_UART_MSG_PING 0x05
#define AI_UART_MSG_PONG 0x06
#define AI_UART_MSG_WAKE_DETECTED 0x10
#define AI_UART_MSG_DING_DONE 0x11
#define AI_UART_MSG_UPLINK_READY 0x12
#define AI_UART_MSG_STATE 0x16
#define AI_UART_MSG_FIRMWARE_INFO 0x18
#define AI_UART_MSG_START_DOWNLINK 0x22
#define AI_UART_MSG_STOP_DOWNLINK 0x23
#define AI_UART_MSG_ENTER_WAKEUP_WAIT 0x25
#define AI_UART_MSG_ENTER_OTA_MODE 0x28

#define AI_UART_ACK_OK 0x00
#define AI_UART_ACK_UNSUPPORTED 0x02
#define AI_UART_ACK_FAILED 0x03

#define AI_UART_STATE_WAKEUP_WAIT 0x01
#define AI_UART_STATE_LISTENING 0x02
#define AI_UART_STATE_DOWNLINK_PLAYING 0x04

static SemaphoreHandle_t send_mutex;
static volatile uint8_t peer_ready;
static volatile uint8_t current_state = AI_UART_STATE_WAKEUP_WAIT;
static volatile uint8_t downlink_enabled;
static volatile uint8_t downlink_codec_started;
static volatile uint8_t i2s_rx_ready;
static volatile TickType_t last_peer_tick;
static uint8_t tx_seq;
static uint32_t downlink_bytes;
static volatile uint32_t dropped_commands;

static uint8_t wait_audio_play_idle(uint32_t wait_ms)
{
    uint32_t waited_ms = 0;
    while((AUDIO_PLAY_STATE_IDLE != get_audio_play_state()) && (waited_ms < wait_ms))
    {
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }
    return (AUDIO_PLAY_STATE_IDLE == get_audio_play_state()) ? 1 : 0;
}

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

static void send_firmware_info(void)
{
    const partition_table_t *table = get_partition_table();
    uint8_t payload[9] = {0};
    if(NULL == table)
    {
        mprintf("[OTA] firmware info unavailable: partition table missing\n");
        return;
    }
    payload[0] = table->FirmwareFormatVer;
    payload[1] = (uint8_t)(table->soft_ware_version >> 16);
    payload[2] = (uint8_t)(table->soft_ware_version >> 8);
    payload[3] = (uint8_t)table->soft_ware_version;
    payload[4] = (uint8_t)(table->hard_ware_version >> 16);
    payload[5] = (uint8_t)(table->hard_ware_version >> 8);
    payload[6] = (uint8_t)table->hard_ware_version;
    if(0xF0 == table->user_code1_status)
    {
        payload[7] = 1;
    }
    else if(0xF0 == table->user_code2_status)
    {
        payload[7] = 2;
    }
    payload[8] = 0;
    send_frame(AI_UART_MSG_FIRMWARE_INFO, payload, sizeof(payload));
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
        cm_set_codec_mute(PLAY_CODEC_ID, CODEC_OUTPUT, 3, ENABLE);
        ciss_set(CI_SS_PLAY_STATE, CI_SS_PLAY_STATE_IDLE);
        downlink_codec_started = 0;
    }
    mprintf("[DOWNLINK] stopped bytes=%u rx=drain pa=keep-on\n", (unsigned int)downlink_bytes);
}

static void downlink_task(void *arg)
{
    static int16_t mono_pcm[AUDIO_CAP_POINT_NUM_PER_FRM];
    (void)arg;
    for(;;)
    {
        uint32_t input_addr = 0;
        uint32_t input_size = 0;
        if(!i2s_rx_ready)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        cm_read_codec(PLAY_PRE_AUDIO_CODEC_ID, &input_addr, &input_size);
        if(downlink_enabled && input_addr && input_size)
        {
            const int16_t *input_pcm = (const int16_t *)input_addr;
            uint32_t sample_count = input_size / (2U * sizeof(int16_t));
            if(sample_count > AUDIO_CAP_POINT_NUM_PER_FRM)
            {
                sample_count = AUDIO_CAP_POINT_NUM_PER_FRM;
            }
            for(uint32_t i = 0; i < sample_count; i++)
            {
                mono_pcm[i] = input_pcm[2 * i];
            }
            if(AUDIO_PLAY_OS_SUCCESS == audio_play_hw_write_data(mono_pcm, sample_count * sizeof(int16_t)))
            {
                downlink_bytes += sample_count * sizeof(int16_t);
            }
        }
    }
}

static uint8_t start_downlink(void)
{
    static uint8_t downlink_play_buffer[AUDIO_CAP_POINT_NUM_PER_FRM * sizeof(int16_t) * 4];
    cm_pcm_buffer_info_t pcm_buffer_info = {0};
    cm_sound_info_t sound_info = {0};

    if(downlink_enabled)
    {
        return 1;
    }
    if(!i2s_rx_ready)
    {
        mprintf("[DOWNLINK] start failed: i2s rx not ready\n");
        return 0;
    }
    if(AUDIO_PLAY_STATE_IDLE != get_audio_play_state())
    {
        stop_play(NULL, NULL);
        if(!wait_audio_play_idle(AI_PLAY_STOP_WAIT_MS))
        {
            mprintf("[DOWNLINK] start failed: player busy\n");
            return 0;
        }
    }

    pcm_buffer_info.play_buffer_info.block_num = 1;
    pcm_buffer_info.play_buffer_info.buffer_num = 4;
    pcm_buffer_info.play_buffer_info.block_size = AUDIO_CAP_POINT_NUM_PER_FRM * sizeof(int16_t);
    pcm_buffer_info.play_buffer_info.buffer_size = pcm_buffer_info.play_buffer_info.block_size;
    pcm_buffer_info.play_buffer_info.pcm_buffer = downlink_play_buffer;
    cm_config_pcm_buffer(PLAY_CODEC_ID, CODEC_OUTPUT, &pcm_buffer_info);

    sound_info.sample_rate = 16000;
    sound_info.channel_flag = 1;
    sound_info.sample_depth = IIS_DW_16BIT;
    cm_config_codec(PLAY_CODEC_ID, CODEC_OUTPUT, &sound_info);

    downlink_bytes = 0;
    downlink_codec_started = 1;
    cm_start_codec(PLAY_CODEC_ID, CODEC_OUTPUT);
    cm_set_codec_mute(PLAY_CODEC_ID, CODEC_OUTPUT, 3, DISABLE);
    audio_play_hw_pa_da_ctl(ENABLE, true);
    ciss_set(CI_SS_PLAY_STATE, CI_SS_PLAY_STATE_PLAYING);
    downlink_enabled = 1;
    send_state(AI_UART_STATE_DOWNLINK_PLAYING);
    mprintf("[DOWNLINK] started owner=ai_uart format=16000/16/mono pa=on\n");
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
        send_firmware_info();
        mprintf("[AI_UART] peer ready, continuous uplink enabled\n");
    }
}

void ai_uart_i2s_handle_command(const ai_uart_i2s_command_t *cmd)
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
    case AI_UART_MSG_ENTER_OTA_MODE:
        stop_downlink();
        if(CINV_OPER_SUCCESS != write_ota_mcu_status(5))
        {
            send_ack(cmd->seq, AI_UART_ACK_FAILED);
            mprintf("[OTA] enter rejected reason=nv-write-failed\n");
            break;
        }
        send_ack(cmd->seq, AI_UART_ACK_OK);
        UartPollingSenddone((UART_TypeDef *)AI_UART_CONTROL_UART);
        mprintf("[OTA] enter accepted; software reset into FW_V4 updater\n");
        dpmu_software_reset_system_config();
        break;
    default:
        send_ack(cmd->seq, AI_UART_ACK_UNSUPPORTED);
        break;
    }
}

static void post_command_from_isr(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    sys_msg_t msg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    if(len > AI_UART_COMMAND_PAYLOAD_MAX)
    {
        dropped_commands++;
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = SYS_MSG_TYPE_AI_UART;
    msg.msg_data.ai_uart_data.type = type;
    msg.msg_data.ai_uart_data.seq = seq;
    msg.msg_data.ai_uart_data.len = len;
    if(len)
    {
        memcpy(msg.msg_data.ai_uart_data.payload, payload, len);
    }
    if(pdPASS != send_msg_to_sys_task(&msg, &higher_priority_task_woken))
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

void ai_uart_i2s_on_audio_ready(void)
{
    i2s_rx_ready = 1;
}

static void heartbeat_task(void *arg)
{
    TickType_t last_ping = 0;
    static const uint8_t heartbeat[3] = {0x06, 0x13, 0x01};
    (void)arg;
    for(;;)
    {
        TickType_t now = xTaskGetTickCount();
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
        vTaskDelay(pdMS_TO_TICKS(AI_UART_TASK_POLL_MS));
    }
}

void ai_uart_i2s_protocol_init(void)
{
    BaseType_t task_created;
    send_mutex = xSemaphoreCreateMutex();
    CI_ASSERT(send_mutex, "ai uart resources\n");
    task_created = xTaskCreate(heartbeat_task, "ai_uart", 384, NULL, 3, NULL);
    CI_ASSERT(pdPASS == task_created, "ai uart task\n");
    task_created = xTaskCreate(downlink_task, "ai_downlink", 512, NULL, 4, NULL);
    CI_ASSERT(pdPASS == task_created, "ai downlink task\n");
    __eclic_irq_set_vector(UART1_IRQn, (int32_t)uart_irq_handler);
    UARTInterruptConfig((UART_TypeDef *)AI_UART_CONTROL_UART, AI_UART_CONTROL_BAUDRATE);
    send_state(AI_UART_STATE_WAKEUP_WAIT);
}

void ai_uart_i2s_on_wake(uint16_t wake_id)
{
    uint8_t payload[2] = {(uint8_t)(wake_id & 0xff), (uint8_t)(wake_id >> 8)};
    if(downlink_enabled || AUDIO_PLAY_STATE_IDLE != get_audio_play_state())
    {
        stop_downlink();
        if(AUDIO_PLAY_STATE_IDLE != get_audio_play_state())
        {
            stop_play(NULL, NULL);
            if(!wait_audio_play_idle(AI_PLAY_STOP_WAIT_MS))
            {
                mprintf("[AI_UART] wake preempt wait timeout\n");
            }
        }
    }
    send_frame(AI_UART_MSG_WAKE_DETECTED, payload, sizeof(payload));
    send_state(AI_UART_STATE_LISTENING);
    send_uplink_ready();
}

void ai_uart_i2s_on_ding_done(void)
{
    send_frame(AI_UART_MSG_DING_DONE, NULL, 0);
}
