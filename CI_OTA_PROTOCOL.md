# CI1306 FW_V4 OTA 串口对接协议（ESP32）

> 本文是当前 `offline_asr_llm_aiot_iis_sample` 工程实际使用的 OTA 协议。
>
> 依据：`firmware/config.ini` 的 `firmware_version=FW_V4`、业务侧 `ENTER_OTA_MODE(0x28)` 启动标志，以及 `components/ota/flash_update.c/.h` 中的 updater 实现。
>
> 旧版 `app/app_ota` 的 `A0~A4 VERSION/START/FIRMWARE/FINISH` 协议是遗留代码，不是本工程 ESP32 OTA 对接协议。

## 1. 整体流程

```text
ESP32 下载完整 Firmware_V2.0.0.bin 并校验 SHA-256
  ↓
普通业务协议 A5 5A：ESP32 发送 ENTER_OTA_MODE(0x28)
  ↓
CI 返回 ACK(status=0)，写 NVDATA_ID_OTA_MCU_STATUS=5 后复位
  ↓
CI 进入 FW_V4 updater，UART 切换为 A5 0F 协议
  ↓
CHECK_READY → GET_INFO → BLOCK_INFO → ERASE
  ↓
CI 主动发送 WRITE 请求(offset,size)
  ↓
ESP32 按请求返回 offset + 固件数据
  ↓
CI 反复请求，完成后发送 WRITE_DONE
  ↓
ESP32 发送 VERIFY → COMPLETE → SYSTEM_RESET
  ↓
CI 启动新固件并通过普通 A5 5A 协议上报 FIRMWARE_INFO
  ↓
版本、硬件、活动槽和健康状态全部正确后，ESP32 上报 OTA 成功
```

关键方向：**WRITE 数据不是 ESP32 主动连续推送，而是 CI 每次请求 offset 和 size，ESP32 再返回对应数据。**

## 2. 串口与模式切换

当前工程 UART 配置为 `921600 8N1`、无流控、3.3 V TTL。实际量产固件若修改 `AI_UART_CONTROL_BAUDRATE` 或 updater 启动波特率，应同步修改 ESP32。

升级期间 UART 只能有一个 owner：

- 正常模式：`A5 5A` 业务协议；
- updater 模式：`A5 0F` FW_V4 协议。

ESP32 收到进入 OTA 的成功 ACK 后停止发送业务帧。CI 复位后，ESP32在 15 秒内持续发送 FW_V4 `CHECK_READY`，收到正确 ACK 才能开始升级。

## 3. FW_V4 帧格式

```text
offset  size     field
0       1        0xA5
1       1        0x0F
2       2        data_length，u16 little-endian
4       1        type
5       1        command
6       1        number
7       N        data
7+N     2        CRC16，u16 little-endian
9+N     1        0xFF
```

总帧长度：

```text
frame_size = 10 + data_length
```

注意：`data_length` 仅表示 data 长度，不包含 type、command、number、CRC 和帧尾。

### 3.1 字节序

以下所有多字节字段均为小端：

- `data_length:u16le`
- `CRC16:u16le`
- Flash 地址、offset、size：`u32le`
- block CRC：`u16le`

### 3.2 CRC16

```text
poly   = 0x1021
init   = 0x0000
refin  = false
refout = false
xorout = 0x0000
```

CRC 覆盖：

```text
type + command + number + data
```

CRC 不覆盖 `A5 0F`、长度字段和 `FF`。

```c
uint16_t ci_fw_v4_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000)
                ? (uint16_t)((crc << 1) ^ 0x1021)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

### 3.3 类型

| type | 名称 | 含义 |
|---:|---|---|
| `0xA0` | CMD | 命令 |
| `0xA1` | REQ | 请求；WRITE 请求主要由 CI 发起 |
| `0xA2` | ACK | 对命令或请求的响应 |
| `0xA3` | NOTIFY | 进度通知 |

`number` 当前 updater 固定使用 `0x00`。ESP32发送时固定为 0，接收时仍应解析并记录。

## 4. 命令表

| command | 名称 | 发起方 | CMD/REQ data | 响应 |
|---:|---|---|---|---|
| `0x03` | UPDATE_REQ | CI | 空 | 当前流程不依赖 |
| `0x04` | GET_INFO | ESP32 | 空 | CI ACK，设备信息 |
| `0x05` | CHECK_READY | ESP32 | 空 | CI ACK，空 data |
| `0x06` | BLOCK_INFO | ESP32 | addr:u32 + size:u32 + crc:u16 | CI ACK，空 data |
| `0x07` | ERASE | ESP32 | 空 | CI ACK，然后擦除并发 WRITE REQ |
| `0x08` | WRITE | CI 请求 | offset:u32 + size:u32 | ESP32 ACK：offset:u32 + bytes |
| `0x09` | WRITE_DONE | CI | 空 | ESP32随后发 VERIFY |
| `0x0A` | VERIFY | ESP32 | addr:u32 + size:u32 + crc:u16 | CI ACK：result:u8 |
| `0x0B` | TRY_FAST_BAUD | ESP32 | baud:u32 | CI ACK 后切波特率；首版可不使用 |
| `0x0C` | FAST_BAUD_TEST | ESP32 | size:u32 | CI 返回测试数据；首版可不使用 |
| `0x0D` | READ | ESP32 | addr:u32 + size:u32 | CI ACK：addr:u32 + bytes |
| `0x0E` | COMPLETE | ESP32 | 建议 `01` | CI ACK，空 data |
| `0x11` | PROGRESS | CI NOTIFY | ASCII进度字符串 | 仅展示/记录 |
| `0xA1` | SYSTEM_RESET | ESP32 | 空 | CI ACK 后软件复位 |

## 5. 进入 FW_V4 updater

进入 updater 使用正常 `A5 5A` 业务协议的 `ENTER_OTA_MODE(0x28)`，不是直接发送 `A5 0F`。

CI 收到后：

1. 停止下行播放；
2. 写入 `NVDATA_ID_OTA_MCU_STATUS=5`；
3. 回读确认长度和值；
4. 返回原业务命令 seq 对应的 `ACK(status=0x00)`；
5. 等待 UART 发送完成；
6. 软件复位进入 FW_V4 updater。

若返回 `status=0x03`，ESP32立即终止；若 ACK 因复位边界丢失，则持续尝试 `CHECK_READY`，不能立即判定失败。

## 6. CHECK_READY（0x05）

ESP32发送：

```text
A5 0F 00 00 A0 05 00 69 42 FF
```

CI返回：

```text
A5 0F 00 00 A2 05 00 09 2C FF
```

只有收到上述 CRC 正确的 ACK，才能确认 CI 已进入 updater。

## 7. GET_INFO（0x04）

ESP32发送：

```text
A5 0F 00 00 A0 04 00 58 71 FF
```

CI返回：

```text
A5 0F LL HH A2 04 00 [data...] CRC_L CRC_H FF
```

data布局：

```text
0..3    updater版本：release, minor, major, 0
4..7    SPI Flash JEDEC ID/保留，按原始字节保存
8..     FileConfig_Struct（packed）
末尾16  SPI Flash unique ID
```

当前 updater 版本字节为：

```text
02 01 02 00   // V2.1.2
```

ESP32至少校验：

- 返回 type=`A2`、command=`04`、CRC正确；
- ManufacturerID/ProductID 与目标产品一致；
- HWVersion 与升级包一致；
- Flash容量能容纳本次 block；
- updater版本满足当前协议要求。

`FileConfig_Struct` 是 packed 结构，字段定义见 `components/ota/flash_update.h`；ESP32不要按本机编译器默认对齐直接强转。

## 8. 当前镜像的 block 参数

当前文件：

```text
Firmware_V2.0.0.bin
raw_size   = 1,828,441 = 0x001BE659
SHA-256    = 6A79CA86293F9B53F1564B6D08CD24B1BC0F229BDFABDDA444289A192E07A127
```

FW_V4以完整镜像作为一个 block，从 Flash 地址 0 写入。擦除大小必须按4096字节向上对齐，尾部填 `0xFF`：

```text
block_addr = 0x00000000
block_size = align_up(raw_size, 4096)
           = 1,830,912
           = 0x001BF000
pad_size   = 2,471 bytes of 0xFF
block_crc  = CRC16(padded_image)
           = 0x2A5C
```

ESP32下载完成后必须在本地重新计算 SHA-256、补齐长度和 CRC16，结果不一致不得进入 updater。

## 9. BLOCK_INFO（0x06）

data固定10字节：

```text
00..03  block_addr:u32le
04..07  block_size:u32le
08..09  block_crc:u16le
```

当前镜像发送：

```text
A5 0F 0A 00 A0 06 00
00 00 00 00
00 F0 1B 00
5C 2A
E2 67 FF
```

合并为：

```text
A5 0F 0A 00 A0 06 00 00 00 00 00 00 F0 1B 00 5C 2A E2 67 FF
```

CI返回：

```text
A5 0F 00 00 A2 06 00 5A 79 FF
```

未收到 ACK 不得擦除。

## 10. ERASE（0x07）

ESP32发送：

```text
A5 0F 00 00 A0 07 00 0B 24 FF
```

CI先返回：

```text
A5 0F 00 00 A2 07 00 6B 4A FF
```

然后执行擦除。期间 CI 可能发送 `A3/0x11 PROGRESS`。擦除完成后，CI主动发送第一个 WRITE 请求。

## 11. WRITE请求与数据响应（0x08）

### 11.1 CI请求

CI的REQ data为8字节：

```text
offset:u32le
size:u32le
```

第一次请求固定为 offset=0、size=4096：

```text
A5 0F 08 00 A1 08 00 00 00 00 00 00 10 00 00 B1 F2 FF
```

### 11.2 ESP32响应

ESP32返回 `A2/0x08`：

```text
data = echoed_offset:u32le + image_bytes[size]
data_length = 4 + size
```

第一次响应结构：

```text
A5 0F 04 10 A2 08 00
00 00 00 00
[padded_image offset 0 开始的4096字节]
CRC_L CRC_H FF
```

这里 `04 10` 是 `0x1004` 的小端表示。当前镜像第一次 WRITE ACK 的帧 CRC 为 `0x657E`，线上发送 `7E 65`。

ESP32必须逐项检查请求：

```text
size > 0
size <= 4096
offset + size 不溢出
offset + size <= block_size
offset 为 CI 请求的准确值
```

当 offset 落入 raw image 之外、padded image 以内时，返回 `0xFF`。

CI收到 WRITE ACK 后：

1. 先根据已接收长度发出下一次 WRITE 请求；
2. 将 `data[4..]` 写入 `block_addr + offset`；
3. 回读 Flash；
4. 累计写入 CRC。

因此 ESP32必须允许“下一次REQ紧跟在本次ACK之后”，接收解析不能被本地Flash/网络读取阻塞。

### 11.3 最后一块

当前镜像：

```text
last_offset = 0x001BE000
size        = 4096
其中前1625字节来自原始镜像，后2471字节为0xFF
WRITE ACK frame CRC = 0x37D6，线上 D6 37
```

## 12. WRITE_DONE（0x09）

写满 `block_size` 后，CI发送：

```text
A5 0F 00 00 A1 09 00 34 30 FF
```

ESP32收到后停止发送WRITE数据，转入VERIFY。若本地认为尚未发送完整block，应立即失败并保存日志。

## 13. VERIFY（0x0A）

VERIFY data与BLOCK_INFO相同：addr、size、crc，共10字节。

当前镜像发送：

```text
A5 0F 0A 00 A0 0A 00 00 00 00 00 00 F0 1B 00 5C 2A 9E 71 FF
```

成功返回：

```text
A5 0F 01 00 A2 0A 00 01 FE D0 FF
```

其中 data[0]=1 表示 CRC 一致；data[0]=0 表示失败。失败时不得发送 COMPLETE，可从 BLOCK_INFO + ERASE 完整重试一次，不能从不确定offset续传。

## 14. COMPLETE（0x0E）

建议按头文件定义发送1字节成功状态：

```text
A5 0F 01 00 A0 0E 00 01 56 E1 FF
```

CI处理代码当前不读取该字节，也兼容空data版本：

```text
A5 0F 00 00 A0 0E 00 93 9E FF
```

ESP32实现固定使用第一种 `data=01`，与 `MSG_LEN_UPDATE_COMPLETE=1` 一致。

CI返回：

```text
A5 0F 00 00 A2 0E 00 F3 F0 FF
```

## 15. SYSTEM_RESET（0xA1）

ESP32发送：

```text
A5 0F 00 00 A0 A1 00 D3 93 FF
```

CI返回：

```text
A5 0F 00 00 A2 A1 00 B3 FD FF
```

CI等待UART TX FIFO发送完成后软件复位。ACK可能因物理链路或复位边界丢失；ESP32随后必须切回普通 `A5 5A` 协议，探测新固件启动状态。

## 16. 完整传输序列

```text
ESP32 --A5 5A ENTER_OTA_MODE(0x28)--> CI user_code
ESP32 <--A5 5A ACK(status=0)--------- CI user_code
CI reset into FW_V4 updater

ESP32 --CMD CHECK_READY--------------> CI
ESP32 <--ACK CHECK_READY-------------- CI
ESP32 --CMD GET_INFO-----------------> CI
ESP32 <--ACK GET_INFO----------------- CI
ESP32 校验产品、硬件、Flash和updater版本

ESP32 --CMD BLOCK_INFO(addr=0,size=0x1BF000,crc=0x2A5C)--> CI
ESP32 <--ACK BLOCK_INFO----------------------------------- CI
ESP32 --CMD ERASE----------------------------------------> CI
ESP32 <--ACK ERASE---------------------------------------- CI
ESP32 <--NOTIFY PROGRESS（0次或多次）--------------------- CI

loop:
ESP32 <--REQ WRITE(offset,size<=4096)-------------------- CI
ESP32 --ACK WRITE(offset,data[size])--------------------> CI
until offset+size == block_size

ESP32 <--REQ WRITE_DONE---------------------------------- CI
ESP32 --CMD VERIFY(addr=0,size=0x1BF000,crc=0x2A5C)-----> CI
ESP32 <--ACK VERIFY(result=1)---------------------------- CI
ESP32 --CMD COMPLETE(data=1)----------------------------> CI
ESP32 <--ACK COMPLETE------------------------------------ CI
ESP32 --CMD SYSTEM_RESET--------------------------------> CI
ESP32 <--ACK SYSTEM_RESET-------------------------------- CI
CI reset into new user_code

ESP32 --A5 5A PING/握手-------------------------------> CI
ESP32 <--A5 5A FIRMWARE_INFO---------------------------- CI
ESP32校验 format、软件版本、硬件版本、active_slot、boot_state
全部一致后上报 OTA success
```

## 17. 新固件成功判据

新固件建立普通业务握手后发送 `FIRMWARE_INFO(0x18)`，payload：

```text
0     FirmwareFormatVer
1..3  software major, minor, patch
4..6  hardware major, minor, patch
7     active_slot：1或2
8     boot_state：0表示健康
```

只有同时满足以下条件才成功：

- `FirmwareFormatVer` 与目标 FW_V4 镜像一致；
- 软件版本为目标版本；
- 硬件版本一致；
- `active_slot != 0`；
- `boot_state == 0`；
- 普通PING/PONG和基本业务恢复。

收到 VERIFY/COMPLETE/RESET ACK 都不能单独作为最终成功结果。

## 18. 超时、重试和错误处理

| 阶段 | 建议截止时间 | 处理 |
|---|---:|---|
| 进入updater/CHECK_READY | 15 s | 持续探测；不要只试3次 |
| GET_INFO/BLOCK_INFO | 2 s | 同一CMD最多重试3次 |
| ERASE | 60 s | 接收PROGRESS和WRITE REQ；不要重复ERASE |
| 单次WRITE请求响应 | 2 s | ESP32必须及时响应 |
| 等待下一WRITE/WRITE_DONE | 5 s | 超时报错并保存最后offset |
| VERIFY | 30 s | 失败后整block最多重试一次 |
| COMPLETE/RESET | 2 s | RESET ACK丢失后转启动探测 |
| 新固件启动确认 | 15 s | 版本不一致或超时即失败 |

解析器必须拒绝：

- `data_length > 4100`（当前CI接收上限）；
- CRC错误；
- 帧尾不是 `FF`；
- 当前状态不允许的type/command；
- WRITE size为0或大于4096；
- offset/size越过block；
- GET_INFO身份不匹配；
- VERIFY result不是1。

ESP32应记录每帧时间、方向、type、command、number、length、offset、size、CRC结果和状态迁移。

## 19. 关于样例 `A5 0F 00 06 A2 02 01 FB 8D FF`

该帧属于旧版协议解释方式，不是FW_V4合法完整帧。FW_V4会按小端把 `00 06` 解释为 data_length=`0x0600`，并继续等待1536字节data；同时FW_V4的CRC只覆盖type/cmd/number/data，而不是帧头和长度。

因此ESP32当前实现不得发送或期待此帧。

## 20. 版本基线

```text
PACK_UPDATE_TOOL.exe
version = 4.0.5.000
SHA-256 = EA5F7720B101B9153135D70573701FB924A674FF7E786C3FE7515241146AD5BE

Firmware_V2.0.0.bin
size    = 1,828,441
SHA-256 = 6A79CA86293F9B53F1564B6D08CD24B1BC0F229BDFABDDA444289A192E07A127

firmware/config.ini
firmware_version = FW_V4
chipid           = CI1306
hardware version = 2.0.0
software version = 2.0.0
```

工具、配置和镜像必须作为一组归档；更换任何一个文件后，重新计算镜像补齐长度、整block CRC16及SHA-256，不能复用本文示例常量。
