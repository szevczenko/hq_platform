# ESP CMD Output Configuration

CMD output on ESP32 supports two backends, configured in `Kconfig` under **Command Line**.

## Console mode (default)

Set `CONFIG_CMD_ESP_OUTPUT_CONSOLE=y` in defconfig.

CMD output uses `putchar()` which goes through the ESP-IDF console UART.
You must configure the console UART in your project's `menuconfig`:

```
Component Config → ESP System Settings → Console Output
  CONFIG_ESP_CONSOLE_UART_DEFAULT=y
  CONFIG_ESP_CONSOLE_UART_NUM=0          # match your hardware
  CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
```

## Direct UART mode

Set `CONFIG_CMD_ESP_OUTPUT_UART=y` in defconfig, then configure:

```
CONFIG_CMD_ESP_UART_NUM=1        # 0, 1, or 2 — avoid the console port
CONFIG_CMD_ESP_UART_BAUDRATE=115200
```

CMD initializes the selected UART in `hq_cmd_init()` and writes directly
via `uart_write_bytes()`. This is output-only; no input path is configured.

## Files

| File | Role |
|------|------|
| `Kconfig` | Configuration symbols under "Command Line" |
| `src/cmd/platforms/esp/hq_cmd_platform.c` | ESP output HAL |
| `src/cmd/platforms/posix/hq_cmd_platform.c` | POSIX output HAL |

## Adding new commands

Create a file in `src/cmd/commands/` (e.g. `hq_cmd_foo.c`).
Use `hq_cmd_print()` for output and register via `hq_cmd_register_internal()`.
The build picks up `commands/*.c` automatically via glob.
