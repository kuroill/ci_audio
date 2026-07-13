# CI1306 Full-Duplex Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the startup FreeRTOS list corruption while keeping CI-to-ESP uplink active throughout ESP-to-CI downlink playback.

**Architecture:** Reuse the SDK's established system-message queue for UART commands instead of a private 68-byte command queue and blocking protocol task. The UART ISR performs bounded parsing and posts a compact message; task context owns replies and codec transitions. I2S TX and RX lifecycles remain independent.

**Tech Stack:** CI1306 SDK C, FreeRTOS, UART1, I2S0, codec manager, shell regression tests.

## Global Constraints

- Keep UART protocol version `0x04` and existing frame format.
- Keep ESP32 as I2S master and CI1306 as I2S slave.
- Never stop I2S TX uplink in START_DOWNLINK or STOP_DOWNLINK handling.
- Do not modify the user's existing `firmware/config.ini` counter change.

---

### Task 1: Regression checks for safe dispatch

**Files:**
- Modify: `tests/test_uart_i2s_protocol.sh`
- Test: `tests/test_uart_i2s_protocol.sh`

**Interfaces:**
- Consumes: current `ai_uart_i2s_protocol.c` source.
- Produces: static checks that require system-message dispatch and forbid the private command queue.

- [ ] Add checks for `SYS_MSG_TYPE_AI_UART`, `send_msg_to_sys_task`, absence of `command_queue`, and absence of `audio_pre_rslt_stop()` in `stop_downlink()`.
- [ ] Run `bash tests/test_uart_i2s_protocol.sh` and verify it fails on the missing system-message path.

### Task 2: Route UART commands through the SDK system task

**Files:**
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/system_msg_deal.h`
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/user_msg_deal.c`
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.h`
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c`

**Interfaces:**
- Produces: `ai_uart_i2s_command_t`, `SYS_MSG_TYPE_AI_UART`, and `void ai_uart_i2s_handle_command(const ai_uart_i2s_command_t *cmd)`.
- Consumes: `send_msg_to_sys_task(sys_msg_t *, BaseType_t *)`.

- [ ] Define a compact UART command payload in the protocol header and add it to `sys_msg_t`.
- [ ] Change the ISR frame-completion path to post `SYS_MSG_TYPE_AI_UART` through `send_msg_to_sys_task`.
- [ ] Dispatch that message from `UserTaskManageProcess` to `ai_uart_i2s_handle_command`.
- [ ] Remove the private command queue and blocking protocol task; retain a heartbeat task for periodic PING and peer timeout.
- [ ] Run `bash tests/test_uart_i2s_protocol.sh` and verify it passes.

### Task 3: Restore the proven downlink playback lifecycle

**Files:**
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c`
- Modify: `driver/boards/CI-D06GT01D.c`
- Modify: `tests/test_uart_i2s_protocol.sh`

**Interfaces:**
- Consumes: `start_downlink()` and `stop_downlink()` in task context.
- Produces: persistent IIS0 RX draining, mono RX-to-DAC forwarding, and local playback-only START/STOP transitions while TX uplink remains untouched.

- [ ] Add failing checks that `stop_downlink()` does not stop `CODEC_INPUT` or call `audio_pre_rslt_stop()`, and that the idle worker continues calling `cm_read_codec()`.
- [ ] Add failing checks that START configures `PLAY_CODEC_ID` as `16000 Hz / 16-bit / mono`, unmutes the DAC, and enables PA.
- [ ] Keep IIS0 RX initialized and started once; discard frames while disabled and forward one sample from each stereo-slot pair while enabled.
- [ ] On every START, stop/wait for any old player, drain stale RX frames, reapply the internal DAC buffer/format, start/unmute output, enable PA, then report success.
- [ ] On START/STOP, report `CI_SS_PLAY_STATE_PLAYING/IDLE` for AEC interruption. On STOP, disable forwarding first, mute DAC, keep always-on PA enabled, and leave IIS0 RX/TX running.
- [ ] Run the protocol regression test and verify it passes.

### Task 3A: Harden wake-ding interruption

**Files:**
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/ai_uart_i2s_protocol.c`
- Modify: `projects/offline_asr_llm_aiot_iis_sample/app/app_main/system_msg_deal.c`
- Modify: `tests/test_wakeup_ding_only.sh`

**Interfaces:**
- Consumes: SDK playback completion callbacks and `get_audio_play_state()`.
- Produces: `DING_DONE` only after real completion and a reusable local playback path after interruption.

- [ ] Add failing checks for bounded wait-to-idle and completion-only `DING_DONE`.
- [ ] Track whether ding playback is active; clear it when interrupted and emit completion only from the callback.
- [ ] Wait up to 250 ms in task context after `stop_play()` before taking the DAC for ding or downlink.
- [ ] Run the wakeup regression test and verify it passes.

### Task 4: Update the production protocol

**Files:**
- Modify: `UART_I2S_PROTOCOL.md`

**Interfaces:**
- Produces: explicit natural-completion STOP_DOWNLINK lifecycle and a final ESP32 coordination section.

- [ ] Replace the natural-completion rule with trailing silence followed by STOP_DOWNLINK.
- [ ] State explicitly that STOP_DOWNLINK never stops uplink TX/AEC.
- [ ] Append `ESP32 本次必须配合的改动` as the final section with exact timing and state requirements.

### Task 5: Full verification

**Files:**
- Verify: all modified files.

**Interfaces:**
- Consumes: completed implementation.
- Produces: test and build evidence.

- [ ] Run `bash tests/test_uart_i2s_protocol.sh` and `bash tests/test_wakeup_ding_only.sh`.
- [ ] Build with `tools/ci-tool-kit.exe make-firmware -p projects/offline_asr_llm_aiot_iis_sample` or the repository-supported equivalent discovered from `--help`.
- [ ] Inspect `git diff --check`, `git status --short`, and confirm `firmware/config.ini` remains the user's unrelated modification.
