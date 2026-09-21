# io_uring

A collection of examples, experiments, and performance-oriented implementations for learning and evaluating Linux `io_uring` for high-performance file and network I/O.

The repository focuses on understanding the `io_uring` programming model, asynchronous I/O, submission/completion queues, buffer management, batching, multishot operations, provided buffers, and high-performance networking.

---

## Repository Structure

```text
IO_uring/
├── README.md
│
├── FileIO/
│   └── ...
│
├── NetworkIO/
│   └── ...
│
└── OpcodeUsage/
    └── TcpNetworking/
        ├── run_test.sh
        ├── tcp_client.cc
        └── tcp_server.cc
```

### FileIO

Examples covering file I/O using `io_uring`, including:

- Asynchronous file reads and writes
- Sequential I/O
- Random I/O
- File copy operations
- Registered buffers
- Fixed-buffer I/O
- Queue-depth experiments
- Batch submission
- Batch completion processing
- Large file workloads

### NetworkIO

Network I/O examples using `io_uring`, including:

- TCP server/client
- Multishot accept
- Multishot receive
- Provided buffer rings
- Buffer selection
- Connection management
- Message processing
- Multi-threaded network processing
- CPU affinity
- `SO_REUSEPORT`
- High connection-count workloads

### OpcodeUsage

Examples demonstrating individual `io_uring` operations/opcodes and their corresponding liburing submitting helpers.

Current examples include:

- TCP networking
- TCP client/server
- Socket operations
- `io_uring` network operations
- Opcode-specific experiments

---

## io_uring Architecture

`io_uring` provides a shared-ring interface between userspace and the Linux kernel for asynchronous I/O.

The basic model consists of:

- Submission Queue (SQ)
- Submission Queue Entries (SQEs)
- Completion Queue (CQ)
- Completion Queue Entries (CQEs)

A simplified flow is:

```text
                    Application
                         |
                         |
                    Prepare SQE
                         |
                         v
                +----------------+
                | Submission     |
                | Queue Entry    |
                |     (SQE)      |
                +----------------+
                         |
                         v
                    io_uring
                         |
                         v
                      Kernel
                         |
                +--------+--------+
                |                 |
                v                 v
             File I/O         Network I/O
                |                 |
                v                 v
             Storage           Socket
                |
                |
                +----------------+
                         |
                         v
                Completion Queue
                         |
                         v
                       CQE
                         |
                         v
                    Application
```

---

## Submission Queue

The Submission Queue is used to submit I/O operations to the kernel.

A typical operation is:

```text
Application
     |
     v
Prepare SQE
     |
     v
Submit SQE
     |
     v
Kernel processes operation
```

The Submission Queue Entry describes the requested operation and contains the parameters required by the kernel.

Examples include:

```cpp
io_uring_prep_read()
io_uring_prep_write()
io_uring_prep_accept()
io_uring_prep_recv()
io_uring_prep_send()
```

---

## Completion Queue

When an operation completes, the kernel places a Completion Queue Entry (CQE) into the Completion Queue.

```text
Kernel
   |
   | Operation completed
   v
Completion Queue Entry
        (CQE)
          |
          v
     Application
```

A CQE contains information such as:

- User data associated with the request
- Result of the operation
- Operation-specific flags

For example:

```cpp
struct io_uring_cqe *cqe;

io_uring_wait_cqe(&ring, &cqe);

int result = cqe->res;
```

After processing the CQE:

```cpp
io_uring_cqe_seen(&ring, cqe);
```

---

## Basic io_uring Lifecycle

A simplified lifecycle is:

```text
Create io_uring instance
          |
          v
Prepare SQE
          |
          v
Submit SQE
          |
          v
Kernel processes operation
          |
          v
CQE generated
          |
          v
Application consumes CQE
          |
          v
Process result
```

---

## Opcode Mapping Reference

| Opcode | Submitting Helper | Purpose |
|---|---|---|
| `IORING_OP_ACCEPT` | `io_uring_prep_accept` | Asynchronous acceptance of client socket connections |
| `IORING_OP_CONNECT` | `io_uring_prep_connect` | Asynchronous outbound socket connections |
| `IORING_OP_RECV` | `io_uring_prep_recv` | Receiving data from a socket, including use with provided buffers |
| `IORING_OP_SEND` | `io_uring_prep_send` | Sending payload data directly through a socket |
| `IORING_OP_RECVMSG` | `io_uring_prep_recvmsg` | Receiving data using `msghdr` and vectorized `iovec` buffers |
| `IORING_OP_SENDMSG` | `io_uring_prep_sendmsg` | Sending data using `msghdr` and vectorized `iovec` scatter/gather buffers |
| `IORING_OP_NOP` | `io_uring_prep_nop` | No-op operation useful for testing and benchmarking |
| `IORING_OP_TIMEOUT` | `io_uring_prep_timeout` | Asynchronous timers and operation timeouts |
| `IORING_OP_PROVIDE_BUFFERS` | `io_uring_prep_provide_buffers` | Providing a pool of buffers for dynamic kernel selection |
| `IORING_OP_REMOVE_BUFFERS` | `io_uring_prep_remove_buffers` | Removing previously provided buffers |
| `IORING_OP_SPLICE` | `io_uring_prep_splice` | Zero-copy data transfer between file descriptors |

---

## Network I/O

The networking examples explore how `io_uring` can be used to build asynchronous socket applications.

Important operations include:

```text
IORING_OP_ACCEPT
IORING_OP_CONNECT
IORING_OP_RECV
IORING_OP_SEND
IORING_OP_RECVMSG
IORING_OP_SENDMSG
```

The repository also explores:

- Multishot accept
- Multishot receive
- Provided buffers
- Buffer selection
- Batch submission
- Batch completion
- Multiple worker threads
- CPU affinity
- `SO_REUSEPORT`
- Connection scaling
- Message processing

---

## Multishot Operations

Multishot operations allow a single SQE to generate multiple CQEs.

For example, a multishot accept operation can continue accepting connections without requiring a new SQE to be submitted for every connection.

Conceptually:

```text
                    One SQE
                      |
                      v
              Multishot Accept
                      |
          +-----------+-----------+
          |           |           |
          v           v           v
         CQE         CQE         CQE
       Client 1    Client 2    Client 3
```

This can reduce submission overhead for workloads that repeatedly perform the same operation.

---

## Provided Buffers

Provided buffers allow an application to make a pool of buffers available to the kernel.

Conceptually:

```text
Userspace

+-----------------------------+
|         Buffer Pool         |
|                             |
|  Buffer 0                   |
|  Buffer 1                   |
|  Buffer 2                   |
|  Buffer 3                   |
|  ...                        |
|  Buffer N                   |
+--------------+--------------+
               |
               | Provide buffers
               v
            io_uring
               |
               v
             Kernel
               |
               | Select available buffer
               v
          Network Receive
```

This can reduce the need for the application to specify a buffer for every receive operation.

Related operations include:

```text
IORING_OP_PROVIDE_BUFFERS
IORING_OP_REMOVE_BUFFERS
```

---

## Buffer Selection

With provided buffers, the kernel can select a buffer from a provided buffer group.

The CQE can contain information identifying which buffer was selected.

This allows the application to determine which buffer contains the received data and process it accordingly.

---

## Registered and Fixed Buffers

For file I/O workloads, registered buffers can reduce repeated memory-registration overhead.

The repository explores concepts such as:

- Registered buffers
- Fixed-buffer reads
- Fixed-buffer writes
- Buffer reuse
- Large buffer pools

A simplified model is:

```text
Application
    |
    | Registered buffers
    v
+----------------------+
| User Buffer Pool     |
+----------------------+
    |
    v
io_uring
    |
    v
Kernel I/O
```

---

## Batching

Batching is an important performance technique for high-throughput I/O.

Instead of submitting one operation at a time:

```text
Submit
  |
  v
Operation
  |
  v
Submit
  |
  v
Operation
```

multiple operations can be prepared and submitted together:

```text
SQE 1
SQE 2
SQE 3
SQE 4
SQE 5
  |
  v
Batch Submit
  |
  v
Kernel
```

The same principle can be applied when processing completion events.

The examples in this repository use batching where appropriate to study its effect on throughput and CPU efficiency.

---

## File I/O

The `FileIO` directory contains experiments using `io_uring` for file operations.

Topics include:

- Sequential reads
- Sequential writes
- Random reads
- Random writes
- File copy
- Queue depth
- Buffer size
- Registered buffers
- Fixed-buffer operations
- Batch submission
- Batch completion

A simplified file I/O path is:

```text
Application
     |
     v
Prepare SQE
     |
     v
io_uring
     |
     v
Kernel
     |
     v
Filesystem
     |
     v
Storage
```

---

## Network I/O Data Path

A simplified network I/O path is:

```text
Application
     |
     v
io_uring
     |
     v
Kernel
     |
     v
Socket
     |
     v
Network Stack
     |
     v
NIC
```

For receive operations:

```text
NIC
 |
 v
Kernel Network Stack
 |
 v
Socket
 |
 v
io_uring
 |
 v
Provided / User Buffer
 |
 v
Application
```

---

## TCP Networking Example

The `OpcodeUsage/TcpNetworking` directory contains a TCP client/server example using `io_uring`.

```text
OpcodeUsage/
└── TcpNetworking/
    ├── run_test.sh
    ├── tcp_client.cc
    └── tcp_server.cc
```

The example demonstrates asynchronous TCP networking using `io_uring` operations.

To run the example:

```bash
cd OpcodeUsage/TcpNetworking
./run_test.sh
```

The test script is responsible for building and running the example according to the implementation in that directory.

---

## Performance Considerations

`io_uring` performance depends on more than simply replacing traditional system calls.

Important factors include:

- Workload characteristics
- I/O size
- Buffer size
- Queue depth
- Batch size
- Connection count
- CPU affinity
- Polling
- Registered buffers
- Provided buffers
- Multishot operations
- Memory locality
- Cache behavior
- Storage characteristics
- Network characteristics
- Application architecture

For networking workloads, additional factors include:

- Packet size
- Packets per second
- Connection distribution
- CPU locality
- RSS configuration
- NIC capabilities
- Network stack behavior
- Socket configuration
- Number of worker threads

Performance should therefore be evaluated using representative workloads rather than assuming that one `io_uring` feature will always provide the same improvement.

---

## Important Performance Concepts

### Queue Depth

Queue depth determines how many operations can be outstanding.

Higher queue depth can improve device utilization for some workloads, but excessive queue depth can also increase memory usage, latency, and contention.

### Batch Size

Submitting and completing operations in batches can reduce per-operation overhead.

```text
Small Batch:

SQE -> Submit -> Kernel
SQE -> Submit -> Kernel
SQE -> Submit -> Kernel


Large Batch:

SQE
SQE
SQE
SQE
 |
 +----> Submit ----> Kernel
```

The optimal batch size depends on the workload and system architecture.

### Buffer Size

Buffer size directly affects the number of I/O operations required to move a given amount of data.

For example:

```text
Large Buffer
    |
    +--> Fewer I/O operations
    |
    +--> More data per operation


Small Buffer
    |
    +--> More I/O operations
    |
    +--> More per-operation overhead
```

The appropriate buffer size depends on the workload.

### Connection Count

Network workloads can behave very differently at:

```text
10 connections
100 connections
1,000 connections
10,000 connections
100,000 connections
```

Connection count affects:

- Memory usage
- Socket state
- Scheduling
- CPU utilization
- Cache locality
- Connection management overhead

### CPU Affinity

CPU affinity can improve locality and reduce unnecessary migration of work between CPUs.

A network application may use:

```text
NIC RX Queue
      |
      v
    CPU 0
      |
      v
 Worker 0
```

and:

```text
NIC RX Queue
      |
      v
    CPU 1
      |
      v
 Worker 1
```

Maintaining locality can be important for high-throughput workloads.

---

## High-Performance I/O

This repository also explores the broader Linux I/O path:

```text
Application
     |
     v
io_uring
     |
     v
Linux Kernel
     |
     +-------------------+
     |                   |
     v                   v
   Storage              NIC
     |                   |
     v                   v
   Device             Network
```

Important performance considerations include:

- CPU overhead
- Memory bandwidth
- Cache locality
- DMA
- PCIe
- Storage latency
- Network packet rate
- Kernel processing
- Application architecture

---

## io_uring vs Traditional I/O

Traditional synchronous I/O often follows a model such as:

```text
Application
     |
     | System Call
     v
    Kernel
     |
     v
    I/O
     |
     v
 Application
```

`io_uring` provides a shared submission/completion mechanism:

```text
Application
     |
     | SQE
     v
Submission Queue
     |
     v
    Kernel
     |
     v
    I/O
     |
     v
Completion Queue
     |
     v
Application
```

The goal is to reduce unnecessary transitions and per-operation overhead while allowing multiple operations to remain outstanding.

---

## Requirements

The examples are intended to run on Linux.

Required software:

- Linux
- GCC/G++
- C++17 or later
- `liburing`

On Ubuntu:

```bash
sudo apt update
sudo apt install liburing-dev
```

Verify the installation:

```bash
dpkg -l | grep liburing
```

You can also check the compiler:

```bash
g++ --version
```

---

## Building

Most examples can be compiled with `g++` and `liburing`.

For example:

```bash
g++ tcp_server.cc -luring -o tcp_server
g++ tcp_client.cc -luring -o tcp_client
```

For C++17:

```bash
g++ -std=c++17 tcp_server.cc -luring -o tcp_server
g++ -std=c++17 tcp_client.cc -luring -o tcp_client
```

If additional libraries or compiler flags are required, refer to the individual example directory.

---

## Useful liburing APIs

Common APIs used throughout the examples include:

```cpp
io_uring_queue_init()
io_uring_queue_exit()

io_uring_get_sqe()
io_uring_submit()

io_uring_wait_cqe()
io_uring_peek_cqe()
io_uring_cqe_seen()

io_uring_prep_read()
io_uring_prep_write()

io_uring_prep_accept()
io_uring_prep_connect()

io_uring_prep_recv()
io_uring_prep_send()

io_uring_prep_recvmsg()
io_uring_prep_sendmsg()

io_uring_prep_timeout()

io_uring_prep_provide_buffers()
io_uring_prep_remove_buffers()

io_uring_prep_splice()
```

---

## Learning Goals

The primary goals of this repository are to understand:

- Linux asynchronous I/O
- `io_uring` architecture
- Submission Queue Entries
- Completion Queue Entries
- File I/O
- Network I/O
- Asynchronous socket operations
- Multishot operations
- Provided buffers
- Buffer selection
- Registered buffers
- Fixed-buffer I/O
- Batch submission
- Batch completion
- Queue depth
- CPU affinity
- Network scalability
- High-performance networking
- Zero-copy data movement
- Linux I/O performance characteristics

---

## Experimental Approach

The repository is intended to be used for hands-on experimentation.

For performance experiments, relevant parameters can include:

```text
Workload
I/O Size
Buffer Size
Queue Depth
Batch Size
Connection Count
CPU Affinity
Polling
Buffer Registration
Provided Buffers
Multishot Operations
```

The objective is to understand how these parameters affect:

- Throughput
- Latency
- CPU utilization
- Memory utilization
- Scalability

Performance results should always be interpreted in the context of the specific hardware, kernel version, workload, and application architecture.

---

## Repository Status

This repository is a learning and experimentation environment.

Examples are added incrementally as different `io_uring` operations, Linux I/O mechanisms, and performance techniques are explored.

The code is primarily intended for:

- Learning
- Experimentation
- Performance analysis
- Understanding Linux I/O internals
- Studying high-performance networking

