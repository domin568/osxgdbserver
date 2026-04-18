# osxgdbserver — Darwin/PowerPC

A standalone GDB remote server for **Mac OS X up to 10.5** on **PowerPC**,
designed to work with IDA Pro and other GDB remote protocol clients.

Based on the gdbserver from [Apple's GDB fork (gdb-437, circa 2005)](https://github.com/apple-oss-distributions/gdb/tree/gdb-437),
with the following additions:

- **Darwin/Mach backend** — uses `task_for_pid`, Mach VM and PPC thread-state
  APIs instead of Linux ptrace
- **Server-side conditional breakpoints** — stop only when a register, memory,
  or string condition is satisfied (evaluated on the target, no round-trips)
- **No-ack mode** (`QStartNoAckMode`) for faster communication
- **Single-register read/write** (`p`/`P` packets) for IDA compatibility
- **Verbose logging** via the `LOG=1` environment variable

## Building

Requirements: any C compiler on Mac OS X PowerPC (GCC 4.0 that ships
with Xcode 2.x works fine).

```
make                        # → bin/gdbserver
sudo make install           # → /usr/local/bin/gdbserver
sudo make install PREFIX=/opt  # custom prefix
make clean                  # remove obj/ and bin/
sudo make uninstall         # remove from /usr/local/bin
```

Object files go to `obj/`, the binary to `bin/gdbserver`.

## Usage

```
# Launch a new process under gdbserver
sudo ./bin/gdbserver HOST:PORT /path/to/program [args ...]

# Attach to an already-running process
sudo ./bin/gdbserver HOST:PORT --attach PID
```

`sudo` is required because `task_for_pid` needs root (or a signed entitlement).

On the host (IDA Pro, GDB, etc.):

```
(gdb) target remote <target-ip>:<port>
```

### Verbose logging

```
LOG=1 sudo ./bin/gdbserver :1234 ./my_app
```

Prints every GDB remote packet and its interpretation to stderr.

## Conditional breakpoints

Conditions are evaluated entirely on the target, so the debugger does not have
to be consulted for every hit — very useful for high-frequency breakpoints.

Set them via the GDB monitor command (`qRcmd`).  In IDA's GDB debugger console
or GDB itself:

```
monitor help
```

### Syntax

```
monitor cond_bp <addr> reg <register> <op> <hex_value>
monitor cond_bp <addr> mem <hex_addr> <size> <op> <hex_value>
monitor cond_bp <addr> str <expr> ==|!= <string>
monitor remove_cond_bp <addr>
```

All addresses and values are in **hexadecimal** (no `0x` prefix needed).

### Operators

| Operator | Meaning          |
|----------|------------------|
| `==`     | equal            |
| `!=`     | not equal        |
| `>`      | greater than     |
| `<`      | less than        |
| `>=`     | greater or equal |
| `<=`     | less or equal    |

### Examples

**Register condition** — break at `0x4c4dc` only when `r3 == 0`:

```
monitor cond_bp 4c4dc reg r3 == 0
```

**Memory condition** — break at `0x4c4dc` only when the 4-byte big-endian value
at address `0xbffeff40` is not zero:

```
monitor cond_bp 4c4dc mem bffeff40 4 != 0
```

**String condition** — break at `0x803e0` only when the null-terminated string
pointed to by `r9 + 0xa` equals `GetMenuItemHierarchicalID`:

```
monitor cond_bp 803e0 str r9+0xa == GetMenuItemHierarchicalID
```

The `<expr>` for `str` can be:

- A register: e.g. `r3`
- A register with offset: e.g. `r9+0xa`, `r3-4`
- An absolute hex address: `bffeff40`

**Remove** a conditional breakpoint:

```
monitor remove_cond_bp 4c4dc
```

### Limits

- Up to **32** simultaneous conditional breakpoints (`MAX_COND_BPS`)
- String comparisons read up to **256** bytes from the inferior (`MAX_COND_STR`)

This code is derived from GDB and is licensed under the
**GNU General Public License v2** (or later).  See the source file headers for
the full copyright notice.
