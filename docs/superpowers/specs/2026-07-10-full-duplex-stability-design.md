# CI1306 full-duplex stability fix

## Goal

Keep CI-to-ESP I2S uplink continuous while ESP-to-CI downlink audio is playing, and remove the startup crash caused by unsafe or corrupted FreeRTOS event-list state.

## Architecture

- UART1 carries only framed control messages.
- The UART ISR reads bytes, validates bounded frames, and posts complete commands to a fixed FreeRTOS queue.
- A protocol task owns ACK, STATE, heartbeat, peer timeout, and downlink state transitions.
- I2S0 TX is the continuous AEC/SSP uplink. START_DOWNLINK and STOP_DOWNLINK must never stop it.
- I2S0 RX is the on-demand downlink. A single persistent worker drains RX only while downlink is enabled.
- Downlink start and stop only control I2S RX, local playback, mute, and PA state.

## Safety constraints

- No blocking API, mutex operation, codec start/stop, logging, or task creation in the UART ISR.
- Create queues, mutexes, and tasks before enabling the UART interrupt.
- Use bounded frame and queue payload sizes.
- Initialize codec buffer descriptors before registering them with codec manager.
- Enable FreeRTOS assertions, stack overflow checks, and list integrity checks in debug builds.
- Keep the existing UART V4 wire format and I2S pin assignments.

## Downlink lifecycle

1. ESP sends START_DOWNLINK while continuing silent I2S DOUT.
2. CI protocol task starts RX and playback, then returns ACK OK and STATE 0x04.
3. ESP waits for ACK plus the configured settle interval and sends real PCM.
4. ESP sends trailing silence and then STOP_DOWNLINK at natural completion or interruption.
5. CI stops only RX/playback, returns ACK OK and STATE 0x02. Uplink TX continues throughout.

## Verification

- Static regression tests verify ISR restrictions and independent TX/RX lifecycle.
- Build the selected CI1306 project successfully.
- Run existing UART/I2S protocol tests.
- Hardware acceptance: no reboot loop, valid heartbeat, downlink playback works, and uplink samples continue during playback.

## ESP32 coordination

- Send STOP_DOWNLINK after natural playback completion, after a short trailing-silence drain.
- Continue BCLK/LRCK and silent DOUT outside playback.
- Continue reading uplink during playback and after STOP_DOWNLINK.
- Never treat STOP_DOWNLINK as an uplink-stop command.
