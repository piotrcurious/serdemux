# serdemux v2
Robust Linux serial (de)multiplexer.

## Overview

`serdemux` allows multiple local processes to communicate over a single serial device. Each stream is represented by a pair of named pipes (FIFOs) in a `streams` directory.

### Improvements in v2
- **Dual-FIFO Architecture**: Each stream now has separate `.tx` and `.rx` FIFOs. This eliminates race conditions and potential loopback issues present in v1.
- **Improved Reliability**: `serdemux` keeps persistent file descriptors for RX FIFOs, ensuring that data received from the serial port is buffered even if no local process is currently reading.
- **Graceful Shutdown**: Robust signal handling ensures all threads and file descriptors are properly cleaned up on termination.
- **Configurable Line Length**: Supports a configurable maximum line length for demultiplexing.

## Compilation

```bash
cd v2
gcc -std=c11 -O2 -pthread -o serdemux serdemux.c
```

## Usage

```bash
./serdemux [-d device] [-b baud] [-n streams] [-l maxline]
```

### Options
- `-d`: Serial device path (default: `/dev/ttyUSB0`)
- `-b`: Baud rate (default: `115200`)
- `-n`: Number of streams (default: `16`)
- `-l`: Maximum line length in bytes (default: `16384`)

## How it works

### Streams Directory
When started, `serdemux` creates a `streams` directory containing:
- `0.rx`, `0.tx`
- `1.rx`, `1.tx`
- ...
- `N.rx`, `N.tx`

### Sending Data (FIFO -> Serial)
To send data to the serial port on stream `ID`, write to `streams/ID.tx`.
`serdemux` will read the data, prefix it with a single byte representing the stream ID, and send it to the serial port.

Example:
```bash
echo "Hello" > streams/2.tx
```
The serial port will receive: `\x02Hello\n`

### Receiving Data (Serial -> FIFO)
`serdemux` expects newline-terminated lines from the serial port. The first byte of each line must be the stream ID.

Example: If the serial device sends `\x03World\n`, `serdemux` will write `World\n` into `streams/3.rx`.

To read the data:
```bash
cat streams/3.rx
```

## Testing

A comprehensive Python test suite is included in `test_serdemux.py`. It requires `socat`.

```bash
cd v2
python3 test_serdemux.py
```
