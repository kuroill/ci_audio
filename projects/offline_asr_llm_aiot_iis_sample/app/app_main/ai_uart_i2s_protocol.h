#ifndef __AI_UART_I2S_PROTOCOL_H__
#define __AI_UART_I2S_PROTOCOL_H__

#include <stdint.h>

void ai_uart_i2s_protocol_init(void);
int ai_uart_i2s_peer_ready(void);
void ai_uart_i2s_on_wake(uint16_t wake_id);
void ai_uart_i2s_on_ding_done(void);

#endif
