
#ifndef __CIAS_DEMO_CONFIG_H__
#define __CIAS_DEMO_CONFIG_H__

#define CIAS_AIOT_DEMO_ENABLE                       0    //生产协议由 ai_uart_i2s_protocol 独占 UART1/I2S
#if CIAS_AIOT_DEMO_ENABLE
#define AUDIO_DATA_UPLOAD_BY_UART                   1
#define AUDIO_DATA_PLAY_BY_UART                     1
#endif

#endif  //__CIAS_DEMO_CONFIG_H__
