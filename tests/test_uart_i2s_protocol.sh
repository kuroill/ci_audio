#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
config="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/user_config.h"
ssp="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/ci_ssp_config.c"
board="$root/driver/boards/CI-D06GT01D.c"
protocol="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c"
protocol_h="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.h"
audio_pre="$root/components/audio_pre_rslt_iis_out/ci130x_audio_pre_rslt_out.c"
system_msg_h="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/system_msg_deal.h"
user_msg="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/user_msg_deal.c"
network_send="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_audio_handle/cias_network_msg_send_task.c"
project="$root/projects/offline_asr_llm_aiot_iis_sample/project_file/source_file.prj"
demo="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_demo/cias_demo_config.h"

must() { grep -Eq "$1" "$2" || { echo "FAIL: $3" >&2; exit 1; }; }
must_not() { ! grep -Eq "$1" "$2" || { echo "FAIL: $3" >&2; exit 1; }; }

must '^#define[[:space:]]+AI_UART_CONTROL_UART[[:space:]]+HAL_UART1_BASE' "$config" "UART1 is not selected"
must '^#define[[:space:]]+AI_UART_CONTROL_BAUDRATE[[:space:]]+UART_BaudRate921600' "$config" "UART baudrate is not 921600"
must '^#define[[:space:]]+USE_IIS1_OUT_PRE_RSLT_AUDIO[[:space:]]+1' "$config" "I2S uplink is not enabled"
must '\.iis_left_channel[[:space:]]*=[[:space:]]*DST1' "$ssp" "left uplink is not DST1"
must '\.iis_right_channel[[:space:]]*=[[:space:]]*DST1' "$ssp" "right uplink is not DST1"
must '\.output_iis\.iis_mode_sel[[:space:]]*=[[:space:]]*IIS_SLAVE' "$board" "I2S TX is not slave"
must '\.input_iis\.iis_mode_sel[[:space:]]*=[[:space:]]*IIS_SLAVE' "$board" "I2S RX is not slave"
audio_pre_init_body="$(sed -n '/void audio_pre_rslt_out_codec_init(void)/,/^}/p' "$board")"
! echo "$audio_pre_init_body" | grep -Eq 'cm_config_(pcm_buffer|codec)\(PLAY_PRE_AUDIO_CODEC_ID,[[:space:]]*CODEC_INPUT' || { echo "FAIL: codec 0 input is configured twice during startup" >&2; exit 1; }
must 'AI_UART_PROTOCOL_VERSION[[:space:]]+0x04' "$protocol" "UART v4 missing"
must 'AI_UART_MSG_START_DOWNLINK[[:space:]]+0x22' "$protocol" "START_DOWNLINK missing"
must 'AI_UART_MSG_STOP_DOWNLINK[[:space:]]+0x23' "$protocol" "STOP_DOWNLINK missing"
must 'AI_UART_MSG_ENTER_WAKEUP_WAIT[[:space:]]+0x25' "$protocol" "ENTER_WAKEUP_WAIT missing"
must 'AI_UART_MSG_ENTER_OTA_MODE[[:space:]]+0x28' "$protocol" "ENTER_OTA_MODE missing"
must 'persist_ota_mcu_status' "$protocol" "OTA handoff does not verify the persisted boot flag"
must 'NVDATA_ID_OTA_MCU_STATUS' "$protocol" "OTA handoff does not read back the boot flag"
must 'persisted[[:space:]]*!=[[:space:]]*status' "$protocol" "OTA handoff does not reject a mismatched boot flag"
must 'cinv_item_init\(' "$root/projects/offline_asr_llm_aiot_iis_sample/app/app_audio_handle/cias_audio_data_handle.c" "OTA boot flag is not initialized on first use"
must 'ai_uart_i2s_command_t' "$protocol_h" "compact AI UART command type missing"
must 'ai_uart_i2s_on_audio_ready' "$protocol_h" "audio-ready gate API missing"
must 'ai_uart_i2s_on_audio_ready\(\)' "$audio_pre" "audio init does not release the RX drain worker"
must 'SYS_MSG_TYPE_AI_UART' "$system_msg_h" "AI UART system message type missing"
must 'send_msg_to_sys_task' "$protocol" "UART ISR does not use the SDK system queue"
must 'case[[:space:]]+SYS_MSG_TYPE_AI_UART' "$user_msg" "system task does not dispatch AI UART commands"
must_not 'static[[:space:]]+QueueHandle_t[[:space:]]+command_queue' "$protocol" "private command queue still present"
downlink_stop_body="$(sed -n '/static void stop_downlink(void)/,/^}/p' "$protocol")"
! echo "$downlink_stop_body" | grep -Eq 'cm_stop_codec\(PLAY_PRE_AUDIO_CODEC_ID,[[:space:]]*CODEC_INPUT\)' || { echo "FAIL: downlink stop incorrectly stops persistent I2S RX" >&2; exit 1; }
! echo "$downlink_stop_body" | grep -Eq 'audio_pre_rslt_stop' || { echo "FAIL: downlink stop incorrectly stops continuous uplink" >&2; exit 1; }
echo "$downlink_stop_body" | grep -Eq 'cm_set_codec_mute\(PLAY_CODEC_ID,[[:space:]]*CODEC_OUTPUT,[[:space:]]*3,[[:space:]]*ENABLE\)' || { echo "FAIL: downlink stop does not mute DAC" >&2; exit 1; }
! echo "$downlink_stop_body" | grep -Eq 'audio_play_hw_pa_da_ctl\(DISABLE,[[:space:]]*true\)' || { echo "FAIL: downlink stop disables always-on PA and makes later ding silent" >&2; exit 1; }
echo "$downlink_stop_body" | grep -Eq 'ciss_set\(CI_SS_PLAY_STATE,[[:space:]]*CI_SS_PLAY_STATE_IDLE\)' || { echo "FAIL: downlink stop does not leave AEC playback state" >&2; exit 1; }
downlink_task_body="$(sed -n '/static void downlink_task(void \*arg)/,/^}/p' "$protocol")"
echo "$downlink_task_body" | grep -Eq 'cm_read_codec\(PLAY_PRE_AUDIO_CODEC_ID' || { echo "FAIL: idle worker does not continuously drain I2S RX" >&2; exit 1; }
echo "$downlink_task_body" | grep -Eq 'i2s_rx_ready' || { echo "FAIL: RX worker can read before codec initialization" >&2; exit 1; }
echo "$downlink_task_body" | grep -Eq 'input_pcm\[2[[:space:]]*\*[[:space:]]*i\]' || { echo "FAIL: stereo-slot downlink is not converted to mono" >&2; exit 1; }
start_downlink_body="$(sed -n '/static uint8_t start_downlink(void)/,/^}/p' "$protocol")"
echo "$start_downlink_body" | grep -Eq 'cm_config_pcm_buffer\(PLAY_CODEC_ID,[[:space:]]*CODEC_OUTPUT' || { echo "FAIL: START does not restore DAC buffer" >&2; exit 1; }
echo "$start_downlink_body" | grep -Eq 'cm_config_codec\(PLAY_CODEC_ID,[[:space:]]*CODEC_OUTPUT' || { echo "FAIL: START does not restore DAC format" >&2; exit 1; }
echo "$start_downlink_body" | grep -Eq 'cm_set_codec_mute\(PLAY_CODEC_ID,[[:space:]]*CODEC_OUTPUT,[[:space:]]*3,[[:space:]]*DISABLE\)' || { echo "FAIL: START does not unmute DAC" >&2; exit 1; }
echo "$start_downlink_body" | grep -Eq 'audio_play_hw_pa_da_ctl\(ENABLE,[[:space:]]*true\)' || { echo "FAIL: START does not enable PA" >&2; exit 1; }
echo "$start_downlink_body" | grep -Eq 'ciss_set\(CI_SS_PLAY_STATE,[[:space:]]*CI_SS_PLAY_STATE_PLAYING\)' || { echo "FAIL: START does not enter AEC playback state" >&2; exit 1; }
must 'source-file: projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c' "$project" "protocol source not in build"
must '^#define[[:space:]]+CIAS_AIOT_DEMO_ENABLE[[:space:]]+0' "$demo" "stock UART1 online demo still owns the transport"
must '#if[[:space:]]+!CIAS_AIOT_DEMO_ENABLE' "$network_send" "legacy network send path can use an uninitialized queue"
must 'if[[:space:]]*\(!network_msg_queue\)' "$network_send" "network queue wrapper does not reject a NULL queue"

echo "PASS: UART v4 and full-duplex I2S contract"
