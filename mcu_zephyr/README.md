# Zephyr reflex-tick firmware (Configuration C)

This is the 10 ms reflex-plane loop for the STM32U585 (Cortex-M33) on the Arduino UNO Q
(WBS P4.3). A periodic `k_timer` releases a high-priority cooperative thread. Each
cycle that thread:

1. timestamps its wakeup from the hardware cycle counter,
2. samples the latest setpoint ("latest value wins", `setpoint_update()`), and
3. queues the sample to a low-priority logger thread without blocking.

The logger prints CSV to the console with the same columns as the Linux benchmark,
so `benchmarks/run_matrix.sh -c C` and `results/analyze.py` can consume it unchanged:

```
# reflex_tick period_us=10000 cycles=10000 board=arduino_uno_q
cycle,wakeup_ns,period_ns,setpoint_pct
0,10000412,0,100
1,20000398,9999986,100
...
# summary periods=9999 mean_ns=... min_ns=... max_ns=... misses=... overruns=0 dropped=0
```

`period_ns` is measured in the reflex thread. A sample dropped because the log queue
is full leaves a gap in `cycle` and increments `dropped`. It does not distort the
next period. `overruns` counts timer expiries that fired while the previous one was
still pending, which means a whole period was missed.

## Build

With a west workspace (Zephyr v4.4 or later, which includes the `arduino_uno_q` board):

```sh
# Local functional test, no board needed (simulated time, so periods are exact)
west build -b native_sim/native/64 mcu_zephyr -d build/native -p
./build/native/zephyr/zephyr.exe -stop_at=5

# Target build for the UNO Q MCU
west build -b arduino_uno_q mcu_zephyr -d build/uno_q -p
west flash -d build/uno_q
```

You can override the Kconfig options on the command line, for example:
`west build ... -- -DCONFIG_APP_CYCLES=2000 -DCONFIG_APP_PERIOD_US=10000`

| Option | Default | Meaning |
| --- | --- | --- |
| `CONFIG_APP_PERIOD_US` | 10000 | control period |
| `CONFIG_APP_CYCLES` | 10000 | cycles to log (0 = run forever) |
| `CONFIG_APP_LOG_QUEUE_DEPTH` | 128 | samples buffered for the logger |
| `CONFIG_APP_REFLEX_PRIORITY` | -2 | reflex thread priority (cooperative) |
| `CONFIG_APP_LOGGER_PRIORITY` | 10 | logger thread priority |

## Console UART: confirm on the board (P2.1b)

The `arduino_uno_q` board sets `zephyr,console = &usart1`. It also enables
`lpuart1` (with RTS/CTS), which is likely the internal link to the QRB2210. Before
running Config C, confirm which UART reaches the Linux side as a tty, since that is
where `SERIAL_DEV` for `run_matrix.sh` must point. If the log needs to go to
`lpuart1`, add `boards/arduino_uno_q.overlay`:

```dts
/ {
	chosen {
		zephyr,console = &lpuart1;
	};
};
```

At 115200 baud a 10,000-cycle log (about 40 bytes per line, 100 lines/s) uses about
35 % of the link, so the logger keeps up and does not drop samples.
