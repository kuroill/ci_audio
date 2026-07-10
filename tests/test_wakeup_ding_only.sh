#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
config="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/user_config.h"
system_msg="$root/projects/offline_asr_llm_aiot_iis_sample/app/app_main/system_msg_deal.c"
prompt_h="$root/components/cmd_info/prompt_player.h"
prompt_c="$root/components/cmd_info/prompt_player.c"
ding="$root/projects/offline_asr_llm_aiot_iis_sample/firmware/voice/src/[1000]ding.wav"

assert_line() {
    grep -Eq "$1" "$2" || { echo "FAIL: $3" >&2; exit 1; }
}

assert_line '^#define[[:space:]]+PLAY_WELCOME_EN[[:space:]]+0' "$config" "boot prompt enabled"
assert_line '^#define[[:space:]]+PLAY_ENTER_WAKEUP_EN[[:space:]]+1' "$config" "wakeup ding disabled"
assert_line '^#define[[:space:]]+PLAY_EXIT_WAKEUP_EN[[:space:]]+0' "$config" "exit prompt enabled"
assert_line '^#define[[:space:]]+PLAY_OTHER_CMD_EN[[:space:]]+0' "$config" "command prompt enabled"
assert_line '^#define[[:space:]]+WMAN_PLAY_EN[[:space:]]+0' "$config" "gender prompt enabled"
assert_line '^#define[[:space:]]+WAKEUP_DING_VOICE_ID[[:space:]]+1000U' "$prompt_h" "ding ID is not 1000"
assert_line 'return[[:space:]]+voice_id[[:space:]]*==[[:space:]]*WAKEUP_DING_VOICE_ID' "$prompt_c" "allowlist missing"
assert_line 'prompt_voice_id_is_allowed\(\(uint16_t\)cmd_handle\)' "$prompt_c" "prompt entry is not guarded"
assert_line 'prompt combination rejected' "$prompt_c" "combination prompts are not blocked"

[[ "$(grep -Ec 'prompt_play_by_voice_id\(WAKEUP_DING_VOICE_ID,' "$system_msg")" -eq 2 ]] || {
    echo "FAIL: wakeup branches do not both play ding" >&2
    exit 1
}

[[ -f "$ding" ]] || { echo "FAIL: ding resource missing" >&2; exit 1; }
python3 -c 'import sys, wave
with wave.open(sys.argv[1], "rb") as f:
    actual = (f.getframerate(), f.getsampwidth(), f.getnchannels(), f.getcomptype())
expected = (16000, 2, 1, "NONE")
if actual != expected:
    raise SystemExit(f"FAIL: ding format {actual}, expected {expected}")' "$ding"

echo "PASS: only wakeup ding local playback is allowed"
