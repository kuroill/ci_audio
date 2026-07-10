# CI1306 full-duplex stability fix

## Goal

Keep CI-to-ESP I2S uplink continuous while ESP-to-CI downlink audio is playing, and remove the startup crash caused by unsafe or corrupted FreeRTOS event-list state.

## Architecture

- UART1 carries only framed control messages.
- The UART ISR reads bytes, validates bounded frames, and posts complete commands to a fixed FreeRTOS queue.
- A protocol task owns ACK, STATE, heartbeat, peer timeout, and downlink state transitions.
- I2S0 TX is the continuous AEC/SSP uplink. START_DOWNLINK and STOP_DOWNLINK must never stop it.
- I2S0 RX is initialized once and remains running while ESP supplies BCLK/LRCK. A single persistent worker always drains RX; it forwards frames only while downlink is enabled and discards idle frames otherwise.
- Downlink start and stop control only local playback ownership, PCM forwarding, mute, and PA state. They do not stop or reconfigure I2S0 TX/RX.

## Safety constraints

- No blocking API, mutex operation, codec start/stop, logging, or task creation in the UART ISR.
- Create queues, mutexes, and tasks before enabling the UART interrupt.
- Use bounded frame and queue payload sizes.
- Initialize codec buffer descriptors before registering them with codec manager.
- Enable FreeRTOS assertions, stack overflow checks, and list integrity checks in debug builds.
- Keep the existing UART V4 wire format and I2S pin assignments.

## Downlink lifecycle

1. ESP sends START_DOWNLINK while continuing silent I2S DOUT.
2. CI protocol task drains stale RX data, configures the internal DAC as 16 kHz/16-bit/mono, unmutes it, enables PA, then returns ACK OK and STATE 0x04.
3. ESP waits for ACK plus the configured settle interval and sends real PCM.
4. ESP sends trailing silence and then STOP_DOWNLINK at natural completion or interruption.
5. CI disables PCM forwarding, mutes the DAC, disables PA, continues draining RX, then returns ACK OK and STATE 0x02. Uplink TX and I2S clocks continue throughout.

## Audio format and playback recovery

- The wire format in both directions is standard I2S, 16 kHz, signed 16-bit PCM, two physical slots per frame.
- CI uplink duplicates processed mono `DST1` into left and right slots.
- ESP downlink duplicates mono playback into left and right slots. CI consumes one selected slot and writes mono PCM to the internal DAC.
- Every START_DOWNLINK reapplies the DAC PCM-buffer and sound-format configuration, because wake-word playback or `stop_play()` may have reconfigured the shared local playback path.
- Wake-word interruption waits for the previous local player to become idle before the ding or a later downlink takes ownership. `DING_DONE` is emitted only from the actual playback-completion callback.

## Verification

- Static regression tests verify ISR restrictions and independent TX/RX lifecycle.
- Build the selected CI1306 project successfully.
- Run existing UART/I2S protocol tests.
- Hardware acceptance: no reboot loop, valid heartbeat, downlink playback works, and uplink samples continue during playback.

## ESP32 coordination

- Send stereo-slot frames with the same mono sample in both slots.
- Send STOP_DOWNLINK after natural playback completion, after 40-100 ms of trailing silence.
- Continue BCLK/LRCK and silent DOUT outside playback.
- Continue reading uplink during playback and after STOP_DOWNLINK.
- Never treat STOP_DOWNLINK as an uplink-stop command.
