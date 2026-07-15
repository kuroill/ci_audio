# CI1306 OTA V4.1.5 串口协议

本文描述 `ci_audio` 与 `lenwell-firmware` 当前共同实现的 CI1306 UART OTA 协议。升级使用启英泰伦 V4.1.5 updater，ESP32 负责下载、校验和按 CI 请求返回升级数据，CI updater 负责版本判断、Flash 擦写、分区校验及最终结果判定。

## 1. 固件产物

UART 升级文件是原厂 V4 制作工具从完整 `Firmware_V*.bin` 转换生成的 `ota_firmware.bin`，不是完整固件、裸 Flash 镜像或 updater code1。

文件布局：

```text
0x0000..0x0fff  OTA header 和分区清单
0x1000..0x1fff  278-byte 分区表及 padding
0x2000..         按清单顺序拼接的分区数据
```

可包含的分区名称依次为：

```text
user_code2 / asr / dnn / voice / user_file
```

ESP32 在切换 CI updater 前完成以下校验：

- OTA header 长度和 CRC16；
- 分区数量、名称和顺序；
- 分区表 checksum；
- 芯片型号以 `CI130` 开头；
- 软件版本、硬件版本和分区布局；
- 根据分区表计算出的精确文件长度；
- 云端命令携带的 SHA-256。

每个传输包最多携带 4096 字节固件数据，包总数为 `ceil(file_size / 4096)`。

## 2. UART 所有权和模式切换

UART 配置为 `921600 8N1`、无流控、3.3 V TTL。整个升级过程由 ESP32 的单一 `ci-uart` worker 独占 UART。

正常运行使用 `A5 5A` 业务协议。ESP32 发送空 payload 的 `ENTER_OTA_MODE(0x28)` 后，CI user_code：

1. 停止当前下行播放；
2. 写入并回读 `NVDATA_ID_OTA_MCU_STATUS=5`；
3. 返回业务协议 ACK；
4. 等待 UART 发送完成；
5. 软件复位进入 V4.1.5 updater。

复位后的 updater 独占 UART，并切换为 `A5 0F` 协议。ESP32 在同一个 handoff 等待循环中同时解析业务 ACK 和 updater 状态帧。业务 ACK 若处于复位边界而丢失，只要收到 updater 的 `WAITING`，升级仍按 CI 的实际状态继续。

## 3. V4.1.5 帧格式

```text
offset  size       field
0       2          header = A5 0F
2       2          length，u16 大端
4       1          message type
5       N          payload
5+N     2          CRC16，u16 大端
7+N     1          tail = FF
```

长度字段：

```text
length = type(1) + payload(N) + CRC(2) + tail(1)
frame_size = length + 4
```

CRC 使用 CRC16-CCITT，`poly=0x1021`、`init=0x0000`、不反射、`xorout=0x0000`。覆盖范围为：

```text
A5 0F + length + type + payload
```

长度、CRC、包序号均使用大端字节序。

## 4. 消息定义

### 4.1 A2 STATUS

CI updater 启动后周期发送：

```text
type    = A2
payload = [state, result]
```

状态值：

| state | 名称 | 含义 |
|---:|---|---|
| `0x00` | FAILED | 升级失败，`result` 为原厂错误码 |
| `0x01` | SUCCEEDED | 升级成功，`result=0` |
| `0x02` | WAITING | updater 等待 host |
| `0x03` | UPDATING_CI | 开始升级 CI 本体 |
| `0x04` | UPDATING_DEVICE | 外部设备升级状态 |
| `0x05` | ENTERING | 正在进入升级模式 |

实机 `WAITING` 帧：

```text
A5 0F 00 06 A2 02 01 FB 8D FF
```

### 4.2 A0 VERSION_PARAMETERS

ESP32 在收到 `WAITING` 后发送：

```text
type    = A0
payload = [
  check_software_version,
  check_hardware_version,
  check_product_id,
  check_chip_type,
  check_flash_size,
  check_partition_version,
  target_version_major,
  target_version_minor,
  target_version_patch,
  baudrate:u32be
]
```

当前参数规则：

- 非强制升级：`check_software_version=1`；
- 强制升级：`check_software_version=0`；
- 硬件、Product ID、芯片和 Flash 容量检查均为 `1`；
- 分区独立版本检查为 `0`；
- `baudrate=0`，保持已建立的 921600 链路。

CI 返回：

```text
type    = A0
payload = [allowed, current_major, current_minor, current_patch, result]
```

仅当 `allowed=1` 且 `result=0` 时继续。

### 4.3 A3 START

ESP32 请求升级 CI 本体：

```text
type    = A3
payload = [0]
```

CI 接受后返回 `A2 [UPDATING_CI, 0]`。ESP32 收到该状态后才进入固件传输。

### 4.4 A1 FIRMWARE

固件传输由 CI 的请求驱动。CI 请求：

```text
type    = A1
payload = [result, current:u16be, next:u16be]
```

- `result=0` 表示上一包处理失败；
- `current` 是 CI 当前处理的包序号；
- `next` 是 CI 下一次要求的包序号；
- `next >= packet_count` 表示 CI 已确认全部数据包。

ESP32 返回 CI 指定的包：

```text
type    = A1
payload = [packet:u16be, data<=4096]
```

ESP32 只按 `next` 取包，不在本地自行递增、跳包或推断 CI 的写入进度。超时重发时仍发送 CI 上一次请求的包。

### 4.5 A2 END

CI 返回 `next >= packet_count` 后，ESP32 发送：

```text
type    = A2
payload = empty
```

CI 完成剩余校验后返回最终 `A2 [SUCCEEDED,0]` 或 `A2 [FAILED,result]`。

## 5. 完整时序

```text
ESP32                         CI user_code / updater
  |                                  |
  |-- ENTER_OTA_MODE (A5 5A) ------->|
  |<--------------- ACK -------------|  user_code 写 OTA 标志并复位
  |                                  |
  |<-------- A2 [WAITING,reason] -----|  updater
  |-- A0 VERSION_PARAMETERS -------->|
  |<--------- A0 version result ------|
  |-- A3 [0] ------------------------>|
  |<--------- A2 [UPDATING_CI,0] -----|
  |                                  |
  |<--------- A1 [ok,current,next] ----|
  |-- A1 [next,data] ---------------->|
  |                 ...               |
  |<--- A1 [ok,current,next>=count] ---|
  |-- A2 empty END ------------------>|
  |<--------- A2 [SUCCEEDED,0] --------|  或 FAILED + 原厂错误码
  |                                  |
  |<------ FIRMWARE_INFO (A5 5A) ------|  新 user_code 启动观测
```

## 6. 超时和重试

- handoff 总通信截止时间为 20 秒；`ENTER_OTA_MODE` 最多发送 3 次；
- A0、A3 和 A2 END 单次响应等待 5 秒，最多 3 次；
- A1 固件请求单次等待 8 秒，兼容首包触发的 Flash 擦除，最多 3 次；首个请求超时重发 A3，后续请求超时重发上一固件包；
- CRC 错误、截断帧和无关帧不改变升级状态；parser 重新同步后继续等待当前阶段的有效响应；
- CI 返回显式 `FAILED` 时立即结束，不用超时覆盖原厂错误码；
- 所有重试均保持 UART 单一所有权，不切回普通业务收发。

## 7. 结果语义

升级成功的唯一执行结果是 CI updater 返回：

```text
A2 [SUCCEEDED, 0]
```

以下事件均不代表升级成功：

- ESP32 已下载完文件；
- ESP32 已发送最后一个数据包；
- ESP32 已发送空 A2 END；
- CI 发生复位；
- ESP32 本地等待时间结束。

`A2 [FAILED,result]` 中的 `result` 原样映射为原厂 OTA 错误。新 user_code 启动后的 `FIRMWARE_INFO` 用于记录实际版本和运行恢复情况，是 CI 最终结果之后的二次观测，不替代或反向覆盖 updater 已返回的结果。

## 8. 云端命令字段

CI1306 OTA 命令继续使用现有 MQTT `start_ota_job`，CI 产物字段为：

```json
{
  "target": "ci1306",
  "artifactFormat": "ci1306_ota_v4",
  "fileName": "ci1306-ota-v4-2.0.4.bin",
  "fileSize": 1697375,
  "version": "2.0.4",
  "url": "https://.../ci1306-ota-v4-2.0.4.bin",
  "sha256": "...",
  "force": false
}
```

`fileName`、`fileSize`、包内版本和 SHA-256 均在进入 CI updater 前完成校验。
