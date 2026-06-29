# Binary UDP Protocol

The ESP32-S3 listens on IPv4 UDP port `44444` by default. Multi-byte fields use network byte order. UDP delivery is not reliable, so clients should use a timeout and retry policy appropriate for the operation.

The wire protocol exposes only GPIO line primitives. Higher-level flows such as reset and Maskrom entry are client-side helpers built from these primitives.

## Line Semantics

The target-side K5/K6 buttons short each signal to ground. A protocol line is `asserted` when the target line is being pulled low, equivalent to pressing the button.

Default hardware configuration assumes the ESP32 drives external MOSFET gates:

```text
asserted=true   ESP32 GPIO high   MOSFET on    target line low
asserted=false  ESP32 GPIO low    MOSFET off   target line released
```

For direct wiring to the target button node, use open-drain mode and assert level `0` in project configuration. Detailed GPIO18/GPIO19 wiring assumptions are in [hardware_wiring.md](hardware_wiring.md).

Line bit assignments:

| Bit | Mask | Line |
| --- | ---: | --- |
| 0 | `0x01` | K6 `RESETn` |
| 1 | `0x02` | K5 `SARADC0_BOOT` |

## Request Packet

Python struct format:

```python
"!4sHBBHH"
```

Binary layout:

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PSW2` |
| 4 | 2 | sequence | Client-selected `uint16` echoed by the ack |
| 6 | 1 | command | `1=status`, `2=set_lines`, `3=pulse_lines` |
| 7 | 1 | flags | Must be `0` |
| 8 | 2 | arg0 | Command-specific `uint16` |
| 10 | 2 | arg1 | Command-specific `uint16` |

Unknown flags are rejected with `bad_flags`.

## Commands

### `status` (`1`)

Reads the current commanded line state.

```text
arg0 ignored
arg1 ignored
```

The ack `state_bits` and `detail` both contain the current asserted line bits.

### `set_lines` (`2`)

Atomically updates selected line states.

```text
arg0 = mask of lines to update
arg1 = asserted line bits after the update
```

Only bits `0x01` and `0x02` are valid. The firmware rejects any other bits with `bad_mask`.

Examples:

| Operation | arg0 | arg1 |
| --- | ---: | ---: |
| Hold K6 `RESETn` low | `0x01` | `0x01` |
| Release K6 `RESETn` | `0x01` | `0x00` |
| Hold K5 `SARADC0_BOOT` low | `0x02` | `0x02` |
| Release K5 `SARADC0_BOOT` | `0x02` | `0x00` |
| Release both lines | `0x03` | `0x00` |
| Hold both lines low | `0x03` | `0x03` |

The ack `detail` contains the resulting `state_bits`.

### `pulse_lines` (`3`)

Pulses selected lines low for a bounded duration, then restores each pulsed line to its previous state.

```text
arg0 = mask of lines to pulse
arg1 = pulse duration in milliseconds
```

The mask must be non-zero and may only contain bits `0x01` and `0x02`. Duration is clamped by firmware to `50..10000 ms`.

Examples:

| Operation | arg0 | arg1 |
| --- | ---: | ---: |
| Pulse K6 `RESETn` for 300 ms | `0x01` | `300` |
| Pulse K5 `SARADC0_BOOT` for 500 ms | `0x02` | `500` |
| Pulse both lines for 100 ms | `0x03` | `100` |

The ack `detail` contains the effective clamped pulse duration.

## Ack Packet

Python struct format:

```python
"!4sHBBH"
```

Binary layout:

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `PSA1` |
| 4 | 2 | sequence | Copied from the request when available |
| 6 | 1 | status | Status code |
| 7 | 1 | state_bits | Current asserted line bits |
| 8 | 2 | detail | Status/command-specific `uint16` |

Status codes:

| Code | Name | Meaning |
| ---: | --- | --- |
| 0 | `ok` | Command accepted |
| 1 | `bad_magic` | Request magic was not `PSW2` |
| 2 | `bad_length` | Request length was not 12 bytes |
| 3 | `bad_command` | Command was unknown |
| 4 | `internal_error` | Reserved for internal failures |
| 5 | `bad_flags` | Request flags were non-zero |
| 6 | `bad_mask` | Line mask/asserted bits were invalid |

## Primitive Client Examples

Hold K5 low, pulse K6, then release K5:

```sh
python3 tools/power_switch_client.py <esp32-ip> assert boot
python3 tools/power_switch_client.py <esp32-ip> pulse reset --duration-ms 300
python3 tools/power_switch_client.py <esp32-ip> release boot
```

Release everything:

```sh
python3 tools/power_switch_client.py <esp32-ip> release-all
```
