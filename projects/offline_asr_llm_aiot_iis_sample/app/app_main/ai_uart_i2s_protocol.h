#ifndef __AI_UART_I2S_PROTOCOL_H__
#define __AI_UART_I2S_PROTOCOL_H__

#include <stdint.h>

#define AI_UART_COMMAND_PAYLOAD_MAX 8

typedef struct
{
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint8_t payload[AI_UART_COMMAND_PAYLOAD_MAX];
} ai_uart_i2s_command_t;

void ai_uart_i2s_protocol_init(void);
int ai_uart_i2s_peer_ready(void);
void ai_uart_i2s_handle_command(const ai_uart_i2s_command_t *cmd);
void ai_uart_i2s_on_wake(uint16_t wake_id);
void ai_uart_i2s_on_ding_done(void);

#endif
