param(
    [string]$Port = "COM7",
    [int]$Baud = 921600,
    [switch]$List
)

if ($List) {
    [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object | ForEach-Object { $_ }
    exit 0
}

$ErrorActionPreference = "Stop"

$TypeNames = @{
    0x03 = "ACK"; 0x04 = "ERROR"; 0x05 = "PING"; 0x06 = "PONG";
    0x10 = "WAKE_DETECTED"; 0x11 = "DING_DONE"; 0x12 = "UPLINK_READY";
    0x16 = "STATE_REPORT"; 0x17 = "LOCAL_CMD";
    0x22 = "START_DOWNLINK"; 0x23 = "STOP_DOWNLINK";
    0x24 = "SET_VOLUME"; 0x25 = "ENTER_WAKEUP_WAIT"; 0x26 = "PLAY_LOCAL_VOICE"; 0x27 = "DOWNLINK_ABORTED"
}

$States = @{
    0x00 = "booting"; 0x01 = "waiting_wakeup"; 0x02 = "dialogue_listening";
    0x04 = "downlink_playing"; 0x06 = "error"
}

function Crc8([byte[]]$Data, [int]$Offset, [int]$Count) {
    [int]$crc = 0
    for ($i = 0; $i -lt $Count; $i++) {
        $crc = $crc -bxor $Data[$Offset + $i]
        for ($b = 0; $b -lt 8; $b++) {
            if (($crc -band 0x80) -ne 0) { $crc = (($crc -shl 1) -bxor 0x07) -band 0xFF }
            else { $crc = ($crc -shl 1) -band 0xFF }
        }
    }
    return [byte]$crc
}

function HexBytes([byte[]]$Data, [int]$Offset, [int]$Count) {
    if ($Count -le 0) { return "" }
    $items = New-Object string[] $Count
    for ($i = 0; $i -lt $Count; $i++) { $items[$i] = ("{0:X2}" -f $Data[$Offset + $i]) }
    return ($items -join " ")
}

function DescribePayload([int]$Type, [byte[]]$Payload) {
    switch ($Type) {
        0x03 {
            if ($Payload.Length -ge 2) { return "ack_seq=$($Payload[0]) status=$($Payload[1])" }
        }
        0x05 {
            if ($Payload.Length -ge 2) { return "role=$($Payload[0]) proto=$($Payload[1])" }
        }
        0x06 {
            if ($Payload.Length -ge 2) { return "role=$($Payload[0]) proto=$($Payload[1])" }
        }
        0x10 {
            if ($Payload.Length -ge 2) { return "wake_id=$($Payload[0] -bor ($Payload[1] -shl 8))" }
        }
        0x12 {
            if ($Payload.Length -ge 4) { return "sample_rate=$($Payload[0] -bor ($Payload[1] -shl 8)) bits=$($Payload[2]) channels=$($Payload[3])" }
        }
        0x14 {
            if ($Payload.Length -ge 1) { return "reason=$($Payload[0])" }
        }
        0x16 {
            if ($Payload.Length -ge 1) {
                $s = $States[[int]$Payload[0]]
                if (-not $s) { $s = "unknown" }
                return "state=$($Payload[0])/$s"
            }
        }
    }
    return ""
}

function PrintFrame([byte[]]$Frame) {
    $type = [int]$Frame[3]
    $seq = [int]$Frame[4]
    $len = [int]($Frame[5] -bor ($Frame[6] -shl 8))
    $name = $TypeNames[$type]
    if (-not $name) { $name = "TYPE_0x{0:X2}" -f $type }
    $payload = @()
    if ($len -gt 0) { $payload = $Frame[7..(6 + $len)] }
    [byte[]]$pbytes = $payload
    $desc = DescribePayload $type $pbytes
    $hex = HexBytes $Frame 0 $Frame.Length
    $ts = Get-Date -Format "HH:mm:ss.fff"
    if ($desc) { Write-Host "[$ts] $name seq=$seq len=$len $desc | $hex" }
    else { Write-Host "[$ts] $name seq=$seq len=$len | $hex" }
}

Write-Host "CI1303/CI1306 AI UART monitor"
Write-Host "Port=$Port Baud=$Baud  Read-only. Close PACK_UPDATE_TOOL serial window before use."
Write-Host "Looking for frames: A5 5A 04 TYPE SEQ LEN_L LEN_H payload CRC8"
Write-Host "Press Ctrl+C to stop."
Write-Host ""

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 200
$sp.WriteTimeout = 200
$sp.Open()

$state = 0
$buf = New-Object byte[] 80
$pos = 0
$len = 0
$text = New-Object System.Text.StringBuilder

try {
    while ($true) {
        try { $v = $sp.ReadByte() } catch [TimeoutException] { continue }
        if ($v -lt 0) { continue }
        [byte]$b = $v

        switch ($state) {
            0 {
                if ($b -eq 0xA5) { $buf[0] = $b; $pos = 1; $state = 1 }
                else {
                    if (($b -eq 10) -or ($b -eq 13)) {
                        if ($text.Length -gt 0) { Write-Host ("[TEXT] " + $text.ToString()); [void]$text.Clear() }
                    } elseif (($b -ge 32) -and ($b -le 126)) {
                        [void]$text.Append([char]$b)
                    }
                }
            }
            1 {
                if ($b -eq 0x5A) { $buf[$pos++] = $b; $state = 2 }
                elseif ($b -eq 0xA5) { $buf[0] = $b; $pos = 1 }
                else { $state = 0; $pos = 0 }
            }
            2 {
                $buf[$pos++] = $b
                if ($pos -eq 7) {
                    $len = [int]($buf[5] -bor ($buf[6] -shl 8))
                    if (($buf[2] -ne 0x04) -or ($len -gt 64)) { $state = 0; $pos = 0 }
                    elseif ($len -eq 0) { $state = 4 }
                    else { $state = 3 }
                }
            }
            3 {
                $buf[$pos++] = $b
                if ($pos -eq (7 + $len)) { $state = 4 }
            }
            4 {
                $buf[$pos++] = $b
                $calc = Crc8 $buf 2 (5 + $len)
                if ($calc -eq $b) {
                    $frame = New-Object byte[] $pos
                    [Array]::Copy($buf, 0, $frame, 0, $pos)
                    PrintFrame $frame
                } else {
                    $ts = Get-Date -Format "HH:mm:ss.fff"
                    Write-Host "[$ts] BAD_CRC got=$(('{0:X2}' -f $b)) calc=$(('{0:X2}' -f $calc)) raw=$(HexBytes $buf 0 $pos)"
                }
                $state = 0; $pos = 0; $len = 0
            }
        }
    }
}
finally {
    if ($sp.IsOpen) { $sp.Close() }
}

