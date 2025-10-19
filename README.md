# serdemux
extremely simple linux serial demux


Compile: gcc -std=c11 -O2 -pthread -o serdemux serdemux.c

Create the streams directory and start: ./serdemux -d /dev/ttyUSB0 -b 115200 -n 16

This will create streams/0 … streams/15 named FIFOs and spawn a reader thread for each.


Example: If the serial device sends the bytes {0x02}Hello\n (where 0x02 is the first binary byte), serdemux will write Hello\n into streams/2 (so any process reading that FIFO will receive Hello\n).

Conversely, if a process writes OK\n to the FIFO streams/2, the program will send \x02OK\n to the serial device.


Notes / caveats

The program expects line-delimited serial input (lines end with \n) per your specification. If you need arbitrary binary framing rather than newline-delimited frames, the code can be adapted to forward on length-prefix or on fixed-size packets—tell me and I’ll change it.

The current implementation creates N FIFOs and spawns N reader threads (default N=16). This avoids tricky polling and blocking semantics with FIFOs. You may increase or decrease -n as needed (max 256).

If a FIFO has no reader, attempts to write to it from the serial side will be dropped silently (open will fail with ENXIO). This behavior prevents the serial reading path from blocking. If you prefer blocking behavior (i.e., block until a reader opens the FIFO), tell me and I can change open(..., O_NONBLOCK) to a blocking open.

The program uses blocking reads from the serial device and blocking open() in each stream thread — that is simple and robust on Linux. On program termination we try to cancel threads but may exit before they fully clean up.
