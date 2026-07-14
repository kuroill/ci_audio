# CI1306 <-> ESP32 UART / I2S 生产协议（最终版）

本文档描述 `lenwell-firmware` 与 `ci_audio` 的目标生产协议，作为实施与验收的唯一依据。

## 1. 职责划分

### 1.1 CI1306

- 负责本地唤醒词检测。
- 负责播放本地唤醒提示音 `voice_id=1000`。
- 负责 AEC/SSP 前端处理。
- 负责通过 I2S 持续输出 AEC/SSP 后上行 PCM。
- 负责接收 ESP32 下行 PCM 并驱动本地喇叭播放。
- 通过 UART 只发送唤醒、提示音完成、上行参数、状态和 ACK。

### 1.2 ESP32-S3

- 启动后立即初始化 CI UART 和 I2S master。
- 始终读取 CI1306 上行 I2S PCM；未唤醒或不需要上传时直接丢弃。
- 只在待唤醒阶段学习环境 RMS 参考值；唤醒进入连续对话后冻结该基准，直到回到待唤醒再更新。
- 唤醒后使用 ESP-SR VAD + 自适应 RMS 环境参考决定是否是真人语音。
- 确认语音后才创建 HTTP voice upload，补发 pre-roll，并把 Opus 上云。
- 接收 HTTP downlink Opus，解码为 16 kHz PCM 后通过 I2S 推给 CI1306 播放。
- 播放期间继续读取上行 PCM，避免 DMA 堵塞和下一轮丢头。
- 独立维护"CI 对端存活"状态，与 CI 侧的对端超时检测对称（见 7.1）。

## 2. 生产接线

### 2.1 UART 控制通道

生产 UART 使用 CI1306 开发板右侧 `IIC/UART1` 引脚，避免占用 Type-C/CH340 的 UART0。

| 信号 | CI1306 开发板 | ESP32 GPIO | 方向 | 说明 |
|---|---|---:|---|---|
| UART TX1 | PB7 / SDA / TX1 | GPIO16 / UART_RX | CI -> ESP32 | CI 发送 PING、WAKE、UPLINK_READY、DING_DONE、STATE、ACK |
| UART RX1 | PC0 / SCL / RX1 | GPIO15 / UART_TX | ESP32 -> CI | ESP32 发送 PONG、START_DOWNLINK、STOP_DOWNLINK、ENTER_WAKEUP_WAIT |
| GND | GND | GND | - | 两板必须共地 |

左侧 `PA2/TX1`、`PA3/RX1` 不用于 UART，因为已固定分配给 I2S。

> ⚠️ **生产接线强提示**：开发板丝印上 `TX1/RX1` 标签在左右两组物理引脚上各出现一次，功能完全不同——右侧是本节的 UART 信号，左侧实际是 2.2 节的 I2S 下行 DATA 和 WS 信号。生产 SOP 必须用颜色或编号强制区分这两组标签，首件须用万用表或示波器实测确认引脚功能后再批量接线。

### 2.2 I2S 音频通道

ESP32 是 I2S master，CI1306 是 I2S slave。ESP32 从启动开始持续输出 BCLK/LRCK。

| 信号 | CI1306 开发板 | ESP32 GPIO | 方向 | 说明 |
|---|---|---:|---|---|
| BCLK/SCK | SCK / PA5 / TX2 | GPIO5 / I2S_BCLK | ESP32 -> CI | bit clock |
| WS/LRCK | LRCK / PA3 / RX1 | GPIO6 / I2S_WS | ESP32 -> CI | word select |
| 上行 DATA | SDO / PA4 | GPIO4 / I2S_DIN | CI -> ESP32 | CI 输出 AEC/SSP 后 PCM |
| 下行 DATA | SDI / PA2 / TX1 | GPIO7 / I2S_DOUT | ESP32 -> CI | ESP32 推送 TTS PCM |
| MCLK | MCK / PA6 / RX2 | 不接 | - | 当前协议不依赖 MCLK |
| GND | GND | GND | - | 与 UART 共地 |

## 3. 音频格式

### 3.1 上行 CI -> ESP32

- sample rate：16000 Hz。
- sample format：signed 16-bit little-endian PCM。
- physical layout：standard I2S stereo slots，同一 mono 样本同时写入 left/right slot。
- semantic layout：mono 用户语音。
- CI1306 `ci_ssp_config.c` 将 AEC/SSP 后的 `DST1` 同时写入 left/right slot。
- ESP32 按 16 kHz stereo RX 读取，取 left slot 作为 mono 上云样本。
- 不允许把 interleaved stereo bytes 直接当作 mono 连续样本处理。
- 由于 ESP32 作为 I2S master 从握手起就持续以固定 BCLK/LRCK（2-slot 帧结构）驱动总线，双 slot 复制不额外占用总线带宽，只是 CI 侧内部多一次数据拷贝，属于刻意保留的稳定方案。

CI1306 侧关键配置：

- `USE_IIS1_OUT_PRE_RSLT_AUDIO=1`。
- `AI_I2S_RUNTIME_UPLINK_EN=1`。
- `AI_I2S_UPLINK_MONO_EN=0`。
- `iis_left_channel=DST1`。
- `iis_right_channel=DST1`。
- `vad_mark_enable=false`。

### 3.2 下行 ESP32 -> CI

- sample rate：16000 Hz。
- sample format：signed 16-bit little-endian PCM。
- semantic layout：mono TTS PCM。
- physical layout：与上行对称，ESP32 把同一个 mono 样本同时写入 left/right 两个 physical slot，不把 mono 样本流当作连续字节直接推给双 slot 的物理帧结构。
- 非下行播放期间（握手完成后尚未 `START_DOWNLINK`、或 `STOP_DOWNLINK` 之后），ESP32 必须持续在 DOUT 上输出全零静音 PCM，不能停止写入或让线路悬空。
- ESP32 发送 `START_DOWNLINK` 并收到 ACK 后开始写 I2S DOUT 真实 TTS 数据。
- ESP32 发送 `STOP_DOWNLINK` 后结束本轮 TTS 推送，随后立即切回输出静音 PCM。
- CI1306 的 IIS0 RX 在握手后保持运行；非播放期间仍持续读取并丢弃静音帧，避免 DMA 中残留上一轮音频。
- CI1306 从双 physical slot 中取一个 slot 作为 mono PCM 写入内部 DAC，禁止把左右交错数据直接当作 mono 播放。
- CI1306 收到 `STOP_DOWNLINK` 后只停止 PCM 转发并静音内部 DAC；当前固件 `PLAYER_CONTROL_PA=0`，PA 按常开策略保留，不能在 STOP 中关闭，否则后续本地 ding 虽有 `play start/play end` 日志但实际无声。STOP 不停止 IIS0 RX、IIS0 TX、麦克风采集或 AEC/SSP。

## 4. 单麦 AEC 接线

当前硬件使用单麦 AEC：

- 用户真实麦克风接 `L MIC`。
- `MICR` 不接第二个真实麦克风，作为喇叭参考输入。
- AEC 四针短接：

```text
GND   <-> MICR-
SPK-  <-> MICR+
```

要求：

- `PAUSE_VOICE_IN_WITH_PLAYING` 保持关闭，播放时不能暂停采音、AEC/SSP 或 I2S 上行。
- AEC 只做回声抑制，不决定是否上传，也不直接决定是否打断。
- 播放中是否打断由 ESP32 使用 AEC 后 PCM、本地 VAD、能量和状态机判断。
- 参考信号和麦克风信号都不能削顶；播放中识别差时优先检查 mic/ref/dst 幅值。

## 5. UART 帧格式

UART 只传事件、状态和命令，不传连续音频。

```text
A5 5A VER TYPE SEQ LEN_L LEN_H PAYLOAD CRC8
```

- `VER` 固定为 `0x04`。
- `SEQ` 由发送方自增，8 bit 回绕。
- `LEN_L/LEN_H` 为 payload little-endian 长度。
- ESP32 parser 最大 payload 长度为 64 bytes。
- CI1306 发送 buffer 把 payload 限制在 48 bytes 内。
- `CRC8` 对 `VER..PAYLOAD` 计算，不包含 `A5 5A`，多项式 `0x07`，初始值 `0x00`。
- ESP32 收到非法 version、超长 payload、CRC 错误或未知 type 时丢弃该帧。

## 6. UART 消息类型

| TYPE | 名称 | 方向 | Payload | 处理 |
|---:|---|---|---|---|
| `0x03` | `ACK` | CI -> ESP32 | `[seq, status]` | 见 6.1，关键命令必须按 status 处理 |
| `0x05` | `PING` | 双向 | `06 13 01` | 收到后回复 `PONG` |
| `0x06` | `PONG` | 双向 | `06 13 01` | CI 标记 peer ready；ESP32 仅解析 |
| `0x10` | `WAKE_DETECTED` | CI -> ESP32 | `wake_id:u16le` | ESP32 进入或重置语音对话 |
| `0x11` | `DING_DONE` | CI -> ESP32 | 空 | ESP32 只记录 |
| `0x12` | `UPLINK_READY` | CI -> ESP32 | `80 3E 00 10 02` | ESP32 校验 16000/16bit/2 physical slots，失败处理见 6.2 |
| `0x16` | `STATE` | CI -> ESP32 | `[state]` | ESP32 记录并比对本地状态，见 6.2 |
| `0x18` | `FIRMWARE_INFO` | CI -> ESP32 | `[format, sw_major, sw_minor, sw_patch, hw_major, hw_minor, hw_patch, active_slot, boot_state]` | CI 首次握手后报告当前完整固件身份；ESP32 仅在字段与目标 FW_V4 一致且 `boot_state=0` 时确认 OTA 成功 |
| `0x22` | `START_DOWNLINK` | ESP32 -> CI | 空 | CI 排空旧 RX 帧、重配 mono DAC、解除静音并打开 PA，成功后 ACK |
| `0x23` | `STOP_DOWNLINK` | ESP32 -> CI | 空 | CI 停止 PCM 转发、静音 DAC、保持 PA 与 IIS RX/TX 常开，回 `STATE=0x02` 并 ACK |
| `0x25` | `ENTER_WAKEUP_WAIT` | ESP32 -> CI | 空 | CI 退出对话态、回 `STATE=0x01` 并 ACK |
| `0x28` | `ENTER_OTA_MODE` | ESP32 -> CI | 空 | 当前 user_code 写入 OTA 标志并返回 ACK，随后软件复位进入原厂 updater |

### 6.1 ACK status 与失败处理

| status | 含义 |
|---:|---|
| `0x00` | OK |
| `0x02` | unsupported command |
| `0x03` | command failed |

ESP32 对以下三类命令的 ACK 必须按 status 分支处理，不能只记录/忽略：

- **`START_DOWNLINK`** 收到非 `0x00` ACK：不得开始写 DOUT 真实 PCM，继续输出静音，记录错误日志并重试一次；重试仍失败则放弃本轮下行播放，直接进入下一轮等待，不等待对端超时。
- **`STOP_DOWNLINK`** 收到非 `0x00` ACK：立即在本地停止写入真实 PCM 并切回静音，记录错误日志、计入异常统计，不依赖 CI 侧确认。
- **`ENTER_WAKEUP_WAIT`** 收到非 `0x00` ACK：记录错误日志并重试一次；仍失败则本地强制回到等待唤醒态，避免两端状态永久不一致。

`约 3000ms` 的对端超时（见 7.1）是链路真正断开时的兜底手段，不是命令执行失败时唯一的恢复路径。

### 6.2 STATE 值与 UPLINK_READY 校验

| state | 含义 |
|---:|---|
| `0x01` | CI 待唤醒 |
| `0x02` | CI 对话监听中 |
| `0x04` | CI 下行播放中 |
| `0x08` | CI OTA 独占维护中（保留；当前实现复位进入 updater，不经过该状态） |

`STATE` 不作为 ESP32 状态机的前置触发条件，但 ESP32 应记录"本地状态 vs 最近一次收到的 CI STATE"是否一致，连续多次不一致时打印告警日志，便于排查两端状态漂移。

`UPLINK_READY` 参数校验失败（收到的不是 `80 3E 00 10 02`）时：

- ESP32 不得据此更新本地对上行参数的假设，继续按 16000/16bit/2 slots 处理上行数据；
- 打印明确错误日志（记录实际收到的原始 payload），并计入统计上报，便于量产阶段发现批量固件配置错误。

## 7. 时序

### 7.1 上电、握手与持续上行

1. CI1306 上电后必须能独立本地工作；即使不接 ESP32，也仍然可以本地唤醒并播放 ding。
2. ESP32 启动后初始化 UART 和 I2S master，持续输出 BCLK/LRCK，开始读取/丢弃 CI 上行 PCM，同时在 DOUT 上输出静音 PCM。
3. CI1306 初始化 UART1，发送 `PING(06 13 01)` 和 `STATE=0x01`。
4. CI 收到 ESP32 的 `PING` 或 `PONG` 后标记 `peer_ready=1`。
5. CI 音频输入任务初始化 AEC/SSP 和 I2S 上行输出 codec；`audio_pre_rslt_write_data()` 只以 `peer_ready` 作为持续写入前置条件，不等待唤醒词。
6. 握手成功后，CI 开始把 AEC/SSP 后 `DST1` PCM 持续写入 I2S 上行；这一步发生在唤醒前。
7. ESP32 在未唤醒时持续读取、丢弃上行 PCM，并学习环境 RMS 参考。
8. CI 每 1000 ms 发送一次 `PING` 维持 heartbeat。
9. CI 如果约 3000 ms 没收到 ESP32 的有效 UART 帧，标记 peer lost，停止下行和上行 codec，回未握手态。
10. ESP32 如果约 3000 ms 没收到 CI 的任何有效 UART 帧，同样标记 CI peer lost：
    - 停止向 HTTP 层提交任何正在进行的语音上传/下行流程；
    - 本地状态强制回到"等待 WAKE_DETECTED"，继续输出静音 PCM、继续读取并丢弃上行；
    - 记录日志（见 8.1），一旦重新收到 CI 的有效帧，按步骤 3-6 正常握手，无需重启 ESP32。

`UPLINK_READY` 不是开麦命令，也不是开始持续上行的触发条件。握手 ready 后上行保持常开。

### 7.2 唤醒只切换对话状态

1. CI 检测到唤醒词。
2. CI 如正在播放本地提示音或下行 TTS，会停止当前播放链路。
3. CI 发送 `WAKE_DETECTED`，并播放本地 ding。
4. CI 进入对话监听态，发送 `STATE=0x02`。
5. CI 发送 `UPLINK_READY(80 3E 00 10 02)` 作为上行参数通知，表示 16000 Hz / 16-bit / 2ch physical slots。
6. `UPLINK_READY` 不要求等待本地 ding 播放完成；也不启动 I2S，因为 I2S 上行已经在握手后持续运行。
7. CI 本地 ding 播放完成后发送 `DING_DONE`；只表示提示音结束，不是 ESP32 启动上行的前置条件。
8. ESP32 收到 `WAKE_DETECTED` 后从"环境学习/丢弃音频"切到"本地 VAD + 云端上传候选"状态。
9. ESP32 收到 `UPLINK_READY` 只校验参数，不发送开麦命令，也不把它当作开始读取 I2S 的条件；校验失败处理见 6.2。
10. 对话期内 CI 不因单轮 HTTP upload 结束或 `STOP_DOWNLINK` 停止上行；ESP32 必须持续读取 I2S，防止上行 DMA 堆积或丢失下一句话开头。
11. ESP32 不把每段用户语音结束映射为上行停止；每段语音的结束只对应云端 voice finish，不对应 CI I2S 上行停止。

### 7.3 ESP32 上云

1. ESP32 从握手后就持续读取 60 ms 左右的上行帧；唤醒后才允许这些帧进入本地 VAD/上传 gate。
2. 每帧计算 RMS、peak、mean、zero-crossing；zero-crossing 仅用于日志/观察，不作为门限。
3. ESP32 使用 ESP-SR VAD 判断 speech/silence。
4. ESP32 使用待唤醒阶段学习到的环境 RMS 计算动态 `minSpeechRms`；对话期、播放期和 follow-up quiet 不更新该基准。
5. 只有 `ESP-SR VAD=speech` 且 `frame.rms > minSpeechRms` 时，才进入本地上传 gate。
6. 连续 speech 达到 `VOICE_VAD_START_MS` 后打开 HTTP upload，并补发 `VOICE_PRE_ROLL_MS`。
7. 连续 silence 达到 `VOICE_VAD_END_SILENCE_MS` 后结束 HTTP upload。
8. CI 上行 I2S 不因单轮 upload 结束而停止。

低延迟默认值（`src/config.rs`）：

| 配置 | 默认值 | 说明 |
|---|---:|---|
| `VAD_START_TIMEOUT_MS` | 8000 | 等待用户开始说话的最长时间；连续对话避免 2 秒短窗口切碎用户语音 |
| `VOICE_PRE_ROLL_MS` | 180 | speech 确认后补发的前置缓存 |
| `VOICE_VAD_START_MS` | 120 | 连续 speech 确认时长 |
| `VOICE_VAD_END_SILENCE_MS` | 420 | 连续 silence 结束时长 |
| `VOICE_VAD_MIN_RMS` | 350 | 环境参考未初始化时的安全下限 |
| `VOICE_WAKE_DING_GUARD_MS` | 900 | 首轮等待 `DING_DONE` 或短超时，避免本地提示音上云 |
| `VOICE_FOLLOWUP_QUIET_MS` | 600 | 后续轮次开始前的声学安静窗口 |
| `VOICE_FOLLOWUP_QUIET_TIMEOUT_MS` | 2000 | 等待安静窗口的最长时间 |
| `VOICE_FOLLOWUP_VAD_MIN_RMS` | 700 | 后续轮次开始上传的最低 RMS，避免播放尾音/低能量残留误上云 |

### 7.4 下行播放

1. ESP32 收到服务端 result 后启动 HTTP downlink worker。
2. ESP32 下载长度前缀 Opus 包，解码为 16 kHz mono PCM。
3. ESP32 使用短预缓冲后发送 `START_DOWNLINK`。
4. ESP32 必须等待收到 CI 对 `START_DOWNLINK` 的 ACK（status=0x00）之后，再等待 `DOWNLINK_START_SETTLE_MS`，才允许开始写入真实 TTS PCM；该延迟锚定在收到 ACK 之后，不是命令发出后的盲等定时器。若 ACK 超时或 status 非 0x00，按 6.1 处理，不写入真实 PCM。
5. CI 丢弃 IIS0 RX 中尚未消费的旧帧，重新配置内部 DAC 的 PCM buffer 和 `16000 Hz / 16-bit / mono` 格式，解除静音，确保 PA 已开启，并设置 SDK `CI_SS_PLAY_STATE=PLAYING` 使 AEC 进入播放打断状态；全部完成后 ACK 并发送 `STATE=0x04`。
6. ESP32 通过 I2S DOUT 写 PCM。
7. 自然播放结束时，ESP32 写完全部 PCM 后必须继续发送 40-100 ms 全零静音 PCM，用于排空 CI 接收和播放缓冲区。
8. 静音排空结束后，ESP32 必须发送 `STOP_DOWNLINK`；CI 收到后停止 RX-to-DAC 的 PCM 转发、静音 DAC，并设置 SDK `CI_SS_PLAY_STATE=IDLE`。由于当前 `PLAYER_CONTROL_PA=0`，PA 保持开启，IIS0 RX 也继续由常驻任务读取并丢弃静音帧。CI 随后返回 ACK 并发送 `STATE=0x02`。`STOP_DOWNLINK` 不得停止 PA、IIS0 RX、麦克风采集、AEC/SSP 或 I2S TX 上行。
9. 播放被唤醒词/真人语音打断、下行 worker 异常退出或整轮对话退出恢复时，ESP32 也必须发送 `STOP_DOWNLINK` 作为强制停止命令。
10. ESP32 播放期间和播放后短沉降期间继续读取并丢弃上行，避免 TTS 尾音污染下一轮。
11. 唤醒词打断本地 ding 或下行 TTS 时，CI 先请求旧播放器停止并等待其进入空闲；下一次 `START_DOWNLINK` 必须重新配置 DAC，不能沿用被 ding/`stop_play()` 改写过的播放参数。
12. `DING_DONE` 只能由提示音真实播放完成回调发送；播放请求失败、尚未开始或被唤醒/下行打断时不得伪报完成。

低延迟默认值：

| 配置 | 默认值 | 说明 |
|---|---:|---|
| `VOICE_HTTP_PLAYBACK_PREBUFFER_MS` | 120 | 下行 PCM 预缓冲，配置读取范围 60..240 ms |
| `VOICE_PLAYBACK_SETTLE_MS` | 400 | 播放结束后的上行沉降 drain |
| `DOWNLINK_START_SETTLE_MS` | 30 | `START_DOWNLINK` ACK 之后 ESP32 等 CI 播放链路稳定的内部延迟 |

`playback_prebuffer_pcm_bytes()` 把预缓冲硬限制在最多 240 ms，即使 `.env` 写了更大的值，也不会超过该上限。

### 7.5 退出对话

1. ESP32 连续对话空闲超时、用户明确结束、系统维护或错误恢复时发送 `ENTER_WAKEUP_WAIT`。
2. CI ACK。
3. CI 清理对话态和下行播放状态，发送 `STATE=0x01`。
4. ESP32 回到等待 `WAKE_DETECTED`，同时继续读取/丢弃上行 PCM 并学习环境参考。
5. 若步骤 2 的 ACK 非 `0x00` 或超时，按 6.1 处理：重试一次，仍失败则本地强制回到等待唤醒态，不等待对端超时。

## 8. 日志检查点

### 8.1 ESP32

关键日志：

```text
CI UART RX frame: type=PING
CI UART TX frame: type=PONG
waiting for CI1306 wake event while continuously draining CI uplink PCM
CI wake detected
CI uplink ready observed in always-on mode
start CI uplink capture gate
CI uplink I2S level: ... noiseRms=... minSpeechRms=... espVad=... acoustic=...
ESP VAD speech confirmed for cloud upload
voice http upload buffering opened
voice http CI upload finished
voice http CI downlink jitter buffer
CI downlink playback start
voice http CI stream downlink starts after prebuffer
voice http CI stream playback completed
CI peer timeout on ESP32 side, stop uplink gate and revert to wakeup-wait
CI ACK status=0x02/0x03 for START_DOWNLINK, retrying
CI ACK status=0x02/0x03 for STOP_DOWNLINK, forcing local mute
CI ACK status=0x02/0x03 for ENTER_WAKEUP_WAIT, forcing local wakeup-wait
UPLINK_READY payload mismatch, expected 80 3E 00 10 02 got ...
local STATE vs CI reported STATE mismatch, count=...
```

如果 `espVad=speech` 但 `acoustic=silence`，优先看 `rms` 是否真的大于 `minSpeechRms`。zero-crossing 不决定是否上传。

### 8.2 CI1306

关键日志：

```text
AI_UART TX PING
AI_UART RX PONG
AI_UART TX WAKE_DETECTED
AI_UART TX UPLINK_READY
AI_UART WAKE_DING play requested
AI_UART TX DING_DONE
AI_UART RX START_DOWNLINK
AI_UART DOWNLINK start ok format=16000/16/mono pa=on
AI_UART RX STOP_DOWNLINK
AI_UART DOWNLINK stop bytes=... rx=drain pa=keep-on
AI_UART RX ENTER_WAKEUP_WAIT
```

如果 CI 约 3 秒未收到 ESP32 有效帧，应看到：

```text
AI_UART peer timeout, stop continuous uplink and restart handshake
```

## 9. 打包与验证约束

- `ci_audio` 只能在 Windows SDK 环境完整打包固件；macOS 只做代码修改和静态检查。
- 不要用未知工具重新生成 CI1306 全量布局。
- CI1306 验证包按固定布局只替换 `user_code` 槽：
  - `user_code` offset：`0x4000`
  - `user_code` slot size：`0x25000` / `151552 bytes`
  - `0x0000..0x3FFF` 与已验证底包一致
  - `0x29000..EOF` 与已验证底包一致

## 10. 设计约束

- 不让 CI 端 VAD 决定云端 upload，上传边界完全由 ESP32 侧 VAD + RMS gate 决定。
- 不在 ESP32 侧把 I2S stereo bytes 直接当 mono 连续样本处理，上下行均按 2-slot 物理帧解析/封装。
- 不用手动 RMS-above-noise 或 zero-crossing 数量调 gate；gate 以环境 RMS 参考为准。
- 不把 `AI_I2S_UPLINK_MONO_EN` 改为 `1`；双 slot 复制方案已验证稳定，且不影响总线带宽。
- 不在任何 ACK 失败场景下把约 3000ms 对端超时当作唯一恢复手段；关键命令必须先走 6.1 的重试/本地强制回退逻辑。
- 播放期间不暂停采音、AEC/SSP 或 I2S 上行（`PAUSE_VOICE_IN_WITH_PLAYING` 保持关闭）。

## 11. 实施检查清单

- [ ] 3.2：ESP32 下行写入前确认 mono 样本已复制到 left/right 两个 physical slot
- [ ] 3.2 / 7.1：ESP32 在非播放期间（含启动后、STOP_DOWNLINK 后）DOUT 持续输出静音 PCM
- [ ] 3.2 / 7.4：CI 在非播放期间持续读取并丢弃 IIS0 RX，不停止 RX DMA
- [ ] 3.2 / 7.4：CI 下行从双 slot 选取单个 slot，写入 DAC 的数据为 16 kHz / 16-bit / mono
- [ ] 7.4：每次 START_DOWNLINK 都重新配置 DAC buffer/格式、解除静音并打开 PA
- [ ] 7.4：START/STOP 分别设置 `CI_SS_PLAY_STATE=PLAYING/IDLE`，确保 AEC 播放中唤醒打断生效
- [ ] 7.4：`PLAYER_CONTROL_PA=0` 时 STOP_DOWNLINK 只停止 PCM 转发并静音 DAC，不关闭 PA，不停止 IIS0 RX/TX
- [ ] 7.4：DING_DONE 只在提示音真实播放完成回调中发送
- [ ] 6.1：START_DOWNLINK / STOP_DOWNLINK / ENTER_WAKEUP_WAIT 的 ACK status 均已接入失败处理分支
- [ ] 6.2：UPLINK_READY 参数校验失败时有日志且不误更新本地参数假设
- [ ] 7.1：ESP32 侧新增约 3000ms 对端超时检测，日志格式与 8.1 一致
- [ ] 7.4：DOWNLINK_START_SETTLE_MS 锚定在收到 ACK 之后，而非命令发出后盲等
- [ ] 2.1：生产接线 SOP / 首件检验已加入 TX1/RX1 歧义提示
- [ ] 8.1：全部关键日志已补齐，便于量产阶段远程排障

## 12. CI1306 FW_V4 OTA

> OTA 线上协议以 `lenwell-firmware/UART_I2S_PROTOCOL.md` 第 12 节为唯一实现定义。本节只规定 CI user_code 的进入和启动确认职责，早期“ESP 自定义双 user_code 槽传输”方案已废弃，不得继续实现。

### 12.1 目标与范围

- ESP32 负责通过 HTTPS 完整下载 CI1306 升级包，并在发送给 CI 前校验云端下发的 SHA-256。
- CI1306 升级期间允许停止全部音频和对话能力，只保留 UART1 OTA、Flash 写入、看门狗和必要错误日志。
- UART1 始终只有一个 owner。正常模式由 Lenwell `A5 5A` parser 持有；OTA 模式由同一 owner 将收到的字节转交启英泰伦 FW_V4 `A5 0F` OTA parser。OTA 代码不得重新注册或覆盖 UART1 ISR。
- 升级产物是原厂 Windows SDK 生成的完整 FW_V4，可包含 user_code、算法模型、提示音和 user_file 等 SDK 资源。
- ESP 不按自定义分区写入；进入原厂 updater 后，完整镜像的擦除、写入、校验和启动选择均由原厂 updater 负责。

### 12.2 不可变区域与升级产物

- 升级产物必须由当前官方 Windows SDK 按 `FW_V4` OTA 格式生成，不接受裸 Flash dump、未知工具生成包或任意地址写入指令。
- ESP 在进入 updater 前校验云端 SHA-256；传输前继续校验 FW_V4 主备分区表、表校验和、各有效分区范围及 CRC，并用 updater `GET_INFO` 校验当前设备的产品、硬件和芯片身份。
- 原厂 updater 是否能在所有擦写阶段掉电后自动恢复，必须通过官方结论或实机断电测试确认，不能由 user_code 或 ESP 软件声明保证。

### 12.3 进入 OTA 模式

1. ESP32 完整下载并校验升级包，不允许边下载边覆盖 CI。
2. ESP32 停止新的 HTTP upload/downlink，并等待当前 CI UART 关键命令结束。
3. ESP32 发送空 payload 的 `ENTER_OTA_MODE(0x28)`。
4. CI 停止当前播放，初始化或写入 `NVDATA_ID_OTA_MCU_STATUS=5`，并回读确认长度和值均正确。持久化成功后先返回原命令 `SEQ` 对应的 `ACK(status=0x00)`，等待 UART 发送完成，再软件复位进入原厂 updater。
5. CI 不能进入时返回 `ACK(status=0x03)`，并保持原正常状态，不得部分擦除 Flash。
6. CI 明确返回非 `0x00` ACK 时，ESP32 必须直接按 CI handoff 失败结果上报，不得用本地超时覆盖该结果。若 ACK 因复位边界未收到，ESP32 只能继续发送 `CHECK_READY` 确认 CI 的实际运行态，不能直接推断 CI 执行失败。
7. `CHECK_READY` 成功 ACK 是 updater 已运行的权威结果；ESP32 只有收到该结果后才能发送 OTA 数据。ESP32 在 15 秒通信截止时间内持续探测，截止时间仅用于报告“CI updater 未响应”，不得表述为 CI 拒绝或执行失败，也不能按普通命令的 3 次重试提前结束。

### 12.4 OTA 数据传输

- updater 帧固定为：`A5 0F + payload_len:u16le + frame_type + command + number + payload + crc16:u16le + FF`。
- `frame_type`：`A0=CMD`、`A1=REQ`、`A2=ACK`、`A3=NOTIFY`。CRC16-CCITT 覆盖 `frame_type..payload`，多项式 `0x1021`、初值 `0`。
- ESP 按 `CHECK_READY -> GET_INFO -> BLOCK_INFO -> ERASE -> WRITE -> WRITE_DONE -> VERIFY -> COMPLETE -> SYSTEM_RESET` 执行完整镜像升级。
- `WRITE` 由 updater 主动请求 `offset:u32le + size:u32le`，ESP 按请求 offset 返回数据；禁止用 ESP 本地自增包号替代 updater 的 offset。
- 完整 FW_V4 从地址 0 写入，长度按 4096 bytes 补 `FF`；`VERIFY` CRC 覆盖补齐后的完整数据。
- 整包写入或校验失败时，ESP 自动从 `BLOCK_INFO + ERASE` 完整重试一次；不得从不确定的本地 offset 继续写。

### 12.5 完成、试运行与回滚

1. updater 完成整包 CRC 校验并收到 `COMPLETE` 后，由 ESP 发送 `SYSTEM_RESET`。
2. 新 user_code 启动 UART，并在首次 PING/PONG 握手建立时发送 `FIRMWARE_INFO(0x18)`：

```text
offset  size  field
0       1     format = 0x01
1       3     software version: major, minor, patch
4       3     hardware version: major, minor, patch
7       1     active_slot: 1 or 2
8       1     boot_state: 0=healthy
```

3. ESP32 只有收到与目标 FW_V4 格式、软件版本、硬件版本一致，且 `active_slot != 0`、`boot_state=0` 的报告，才能向云端报告 CI OTA 成功。

### 12.6 禁止事项

- 禁止收到 `ENTER_OTA_MODE` 后立即擦除任何分区；必须先完成请求校验并成功 ACK。
- 禁止 OTA 模块自行接管 UART1 中断向量。
- 禁止仅凭“升级数据发送完成”向云端报告成功，必须等待新固件启动确认。

### 12.7 后续实施检查清单

- [x] ESP32 实现 CI1306 独立 OTA target、完整下载和 SHA-256 预校验
- [x] 按原厂 updater 源码冻结 FW_V4 `A5 0F` 逐字节协议
- [x] CI UART 单一 owner 在 OTA 调用期间独占 normal/updater 收发
- [x] CI 新 user_code 首次握手上报 `FIRMWARE_INFO`
- [ ] Windows SDK 正式生成包含本次 CI 修改的 FW_V4
- [ ] 验证传输断线、CRC 错误、updater 已运行时重试
- [ ] 在擦除、写入、校验、元数据提交、首次启动各阶段执行断电测试
- [ ] 验证提示音、唤醒词、模型资源、UART/I2S 和再次 OTA
- [x] ESP32 仅在 `FIRMWARE_INFO` 报告目标版本已确认后上报 OTA 成功

## 13. ESP32 本次必须配合的改动

本节是本次 CI1306 全双工稳定性修复对应的 ESP32 必改项；完成这些改动前，不应把整机下行播放流程视为验收完成。

1. 自然播放结束后，ESP32 必须继续输出 40-100 ms 全零静音 PCM，然后发送 `STOP_DOWNLINK`，不能只在本地切回静音而长期不通知 CI。
2. ESP32 必须等待 `STOP_DOWNLINK` 的 ACK；等待期间继续提供 BCLK/LRCK、输出静音 DOUT，并持续读取 CI 上行。
3. `STOP_DOWNLINK` 成功或超时都不得停止 I2S master，也不得停止读取 CI 上行；该命令只结束当前下行播放。
4. 播放被插话、唤醒词、网络错误或 worker 异常打断时，同样发送 `STOP_DOWNLINK`。
5. 收到成功 ACK 后期望 CI 返回 `STATE=0x02`；若状态连续不一致，只记录并恢复状态机，不关闭上行总线。
6. `START_DOWNLINK` 仍须等待成功 ACK，再等待 `DOWNLINK_START_SETTLE_MS=30` 后发送真实 PCM。
7. 非播放期间继续在 DOUT 输出全零 PCM，保持 BCLK/LRCK 连续，避免 CI I2S RX 输入悬空。
8. ESP32 必须按 standard I2S 双 slot 写入下行：同一个 mono 样本同时复制到 left/right slot；不得把连续 mono 字节流直接当成双 slot DMA 数据。
9. ESP32 不得通过停 BCLK/LRCK 或停止 DOUT DMA 表达播放结束；播放状态只由 UART `START_DOWNLINK/STOP_DOWNLINK` 表达。
