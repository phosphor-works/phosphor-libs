<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-channel

phosphor-channel sends ordered frames of opaque bytes from one process to another, with backpressure. It links no
Qt and no other Phosphor library, and it runs on Linux and FreeBSD.

## Responsibility

A channel is unidirectional: one writer feeds one reader. Each channel has a codec tag, such as
`afterglow.state.v1`, that names the format of its payload. The library stores the tag and never reads the payload.
The consumer owns the versioning of the payload.

The first consumer is the mux daemon of Afterglow. It sends terminal state to a window, and the window sends input
back. The library links no Qt, so that daemon does not need Qt. The library builds with the rest of phosphor-libs.

The library needs Linux or FreeBSD. Sealed memfds, SCM_RIGHTS and MSG_NOSIGNAL have no portable replacement. On any
other system, the configure step stops.

phosphor-channel is a separate library from phosphor-ipc. phosphor-ipc carries JSON calls and events.
phosphor-channel carries bytes at a high rate.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorChannel::WriteResult` | `Written`, `WouldBlock`, `TooLarge` or `Closed` |
| `PhosphorChannel::ReadResult` | `Frame`, `Empty`, `Closed` or `Corrupt` |
| `PhosphorChannel::RingWriter` | Creates a ring and writes frames into it |
| `PhosphorChannel::RingReader` | Maps a ring that a handshake brought, and reads frames |
| `PhosphorChannel::offerRing` / `acceptRing` | The handover of a ring over a Unix socket |
| `PhosphorChannel::AcceptOptions` | The largest ring and the codecs that a reader takes |
| `PhosphorChannel::Stream` | Several channels over a byte stream |
| `PhosphorChannel::StreamWriter` / `StreamReader` | One channel of a stream |

## Use

The writer creates a ring and offers it over a connected Unix stream socket. The reader accepts it on the other
end of the socket. Both calls block until the handshake ends or the timeout (in milliseconds) expires.

```cpp
#include <PhosphorChannel/Handshake.h>

using namespace PhosphorChannel;

// Writer process: a 64 KiB ring for one codec.
std::string error;
auto writer = RingWriter::create(1 << 16, "afterglow.state.v1", &error);
if (!writer || !offerRing(socket, *writer, 5000, &error)) {
    // Close the socket and destroy the ring.
}
if (writer->tryWrite("through the ring") == WriteResult::WouldBlock) {
    // Wait until spaceFd() is readable, then write again.
}

// Reader process: accept any codec, up to the default largest ring.
auto reader = acceptRing(socket, AcceptOptions(), 5000, &error);
std::string frame;
while (reader->read(frame) == ReadResult::Frame) {
    // Use the frame.
}
// read() returned Empty: wait until dataFd() is readable.
```

## Transports

| Transport | Use | Cost |
| --- | --- | --- |
| `RingWriter` / `RingReader` | Two processes on one computer | A copy in and a copy out. No system call for each frame while the reader keeps up. |
| `Stream` over a Unix socket | The fallback on one computer | Through the kernel, with a buffer copy at each end |
| `Stream` over two pipes | The standard input and output of `ssh` | The same, plus ssh |

### The shared-memory ring

- The ring is a memfd that both processes map. It is sealed with `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_SEAL`.
  The reader refuses a memfd that is not plain shared memory, that lacks the size seals, or that is larger than
  `AcceptOptions::maxCapacity`. Thus the writer can never cut the mapping short. Plain shared memory is a tmpfs file
  on Linux, and a shared memory object without large pages on FreeBSD.
- The write position, the read position and the wake request of each end are each on their own cache line. The
  closed flags share one cache line. Records are 8-byte aligned, and a skip record pads the wrap.
- The largest frame is half the data area less 8 bytes. Thus a frame fits an empty ring at every wrap position.
- A connected pair of Unix sockets wakes the ends, one socket in each process. The writer wakes the reader only
  after the reader's `read()` returned `Empty`. Thus a reader reads until `Empty` before it waits. The reader wakes
  the writer only when the space that its `WouldBlock` asked for is free.
- When the other process exits, its socket closes and the wake fd becomes readable. After an end drains it, or
  fails to signal, its writes return `Closed`. Its reads return `Closed` after the last frame.
- The reader checks each position and length that it reads, and copies each frame out. A ring that breaks the
  layout gives `ReadResult::Corrupt`.
- `offerRing` and `acceptRing` send the memfd and the reader's wake socket over a raw Unix socket with
  `SCM_RIGHTS`, together with the version and the codec tag. A ring is offered one time. The reader can limit the
  size of the ring and the codecs that it takes. The writer learns why the reader refused it. After a failed
  handshake, close the socket and destroy the ring.

### The stream

- A stream carries several channels over one connected byte stream, or over one fd for input and another for
  output. No file descriptor crosses it. ssh must not allocate a terminal (no `-t`), because a terminal changes
  the bytes.
- Each end starts with a hello. The hello carries the protocol version, the number of channels that the end reads
  at a time (`Options::maxChannels`), and the limit of the channel IDs that it reads (`Options::maxChannelId`, 4096
  by default). Owners use the IDs below the limit again.
- The reader grants each channel a window of bytes (`Options::window`, 4 MiB by default). Each frame costs its size
  plus `Stream::FrameCost` against the window. Thus the largest frame is the window less that cost.
- A writer that has used its window gets `WouldBlock`, and `onWritable` tells when it can write again. A full
  channel does not stop any other channel.
- A channel ends when its writer closes it and its reader has read every frame. Then the writer can open its ID
  again.
- Frames go out in fragments (`Options::fragment`, 16 KiB), one fragment of each channel in turn. A small frame
  waits for a few fragments of a large frame on another channel. It also waits for the data that the kernel and
  ssh already hold.
- `tryWrite()`, `readable()` and `writable()` never block. `write()`, `pump()`, `flush()` and `shutdown()` block
  with their own poll.
- Poll `readFd()`. Also poll `writeFd()` while `wantsWrite()` is true. After each call, or in `onReadable`, read
  every reader until `read()` returns a value other than `Frame`.
- An end that fails tells the other end why before it stops writing. `shutdown()` ends a stream cleanly.
  Destroying a stream closes it immediately.
- Values on the wire are little-endian.

## Versioning

`ProtocolVersion` covers the handshakes, the packets of the stream and the layout of the ring. Two ends with
different versions refuse each other, with a message that names both versions. While the version is 0.x, the
soname carries the minor version. Thus a consumer built against 0.1 does not load 0.2.

## Tests

`phosphorchannel_test_ring`, `phosphorchannel_test_handshake` and `phosphorchannel_test_stream` use Qt Test, as the
other libraries do. They run with the rest of the tier under `BUILD_TESTING`.

## Dependencies

- `Threads::Threads` (private)
- No Qt and no other Phosphor library. Only the tests use Qt Test.
