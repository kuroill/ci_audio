#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
config="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/user_config.h"
ssp="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/ci_ssp_config.c"
board="$root/driver/boards/CI-D06GT01D.c"
protocol="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c"
project="$root/projects/offline_asr_llm_aiot_iis_sample/project_file/source_file.prj"
demo="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_demo/cias_demo_config.h"

must() { grep -Eq "$1" "$2" || { echo "FAIL: $3" >&2; exit 1; }; }

must '^#define[[:space:]]+AI_UART_CONTROL_UART[[:space:]]+HAL_UART1_BASE' "$config" "UART1 is not selected"
must '^#define[[:space:]]+AI_UART_CONTROL_BAUDRATE[[:space:]]+UART_BaudRate921600' "$config" "UART baudrate is not 921600"
must '^#define[[:space:]]+USE_IIS1_OUT_PRE_RSLT_AUDIO[[:space:]]+1' "$config" "I2S uplink is not enabled"
must '\.iis_left_channel[[:space:]]*=[[:space:]]*DST1' "$ssp" "left uplink is not DST1"
must '\.iis_right_channel[[:space:]]*=[[:space:]]*DST1' "$ssp" "right uplink is not DST1"
must '\.output_iis\.iis_mode_sel[[:space:]]*=[[:space:]]*IIS_SLAVE' "$board" "I2S TX is not slave"
must '\.input_iis\.iis_mode_sel[[:space:]]*=[[:space:]]*IIS_SLAVE' "$board" "I2S RX is not slave"
must 'AI_UART_PROTOCOL_VERSION[[:space:]]+0x04' "$protocol" "UART v4 missing"
must 'AI_UART_MSG_START_DOWNLINK[[:space:]]+0x22' "$protocol" "START_DOWNLINK missing"
must 'AI_UART_MSG_STOP_DOWNLINK[[:space:]]+0x23' "$protocol" "STOP_DOWNLINK missing"
must 'AI_UART_MSG_ENTER_WAKEUP_WAIT[[:space:]]+0x25' "$protocol" "ENTER_WAKEUP_WAIT missing"
must 'source-file: projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c' "$project" "protocol source not in build"
must '^#define[[:space:]]+CIAS_AIOT_DEMO_ENABLE[[:space:]]+0' "$demo" "stock UART1 online demo still owns the transport"

echo "PASS: UART v4 and full-duplex I2S contract"
