#include <liburing.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>

using namespace std;

#define SERVER_PORT 9000
#define MAX_CONNECTIONS_PER_THREAD 50000

#define ACTIVE_BUFFER_POOL_SIZE 4096
#define BUFFER_SIZE 4096

#define QUEUE_ENTRIES 8192

#define BUFFER_GROUP_ID 0


/*
   Single-shot = one SQE produces one I/O completion and the operation is finished.
   Multishot = one SQE starts a longer-lived operation that can produce multiple CQEs.

   1. How we handle very large Multiple packets of variable size for outbound and inbound

    struct io_uring_cqe {
        __u64  user_data,    // Application-defined data associated with the submitted SQE
        __s32  res,          // Result of the completed I/O operation
        __u32  flags         // Additional information about how the operation completed
    };
   
    res > 0    number of bytes received
    res == 0   connection closed gracefully
    res < 0    error, represented as -errno
   
*/


// Global atomic counters for non-blocking real-time monitoring
std::atomic<size_t> g_total_connections{0};
std::atomic<size_t> g_total_messages{0};
std::atomic<size_t> g_total_bytes{0};

enum OpType {
    OP_ACCEPT,
    OP_RECV,
    OP_SEND
};


struct Connection {
    int fd{-1};
    bool multishot_recv_active{false};
};


// Here we Handle different kinds of Requests -- new connection, send data, recv data.
struct RequestContext {
    OpType type;
   
    Connection *conn{nullptr};
   
    unsigned buffer_id{0};     // Its Dynamically picked among group of available buffers
   
    size_t bytes_to_send{0};
    size_t bytes_sent{0};
};

// What's the difference between io_uring_buf_ring and io_uring ?
// Yes — you can have separate buffer pools for recv and send, and in some designs that is useful.

struct BufferPool {
    io_uring_buf_ring *buf_ring{nullptr};   // Pointer to Metadata for the memory pool
    void *memory{nullptr};                  // Pointer to Memory pool.
   
    size_t    total_size{0};
    unsigned  count{0};
    unsigned  size{0};
   
    unsigned  bgid{0};                      // unique identifier for buffer pool
};


struct WorkerThread {
    int thread_id;
    int cpu_core;
   
    io_uring ring;              // I/O operation engine (Submission Queue and Completion Queue)
    BufferPool pool;            // buffers that recv operations can use
   
    int server_fd;
    size_t active_connections{0};
};


static bool init_ring(io_uring *ring, unsigned entries) {
    io_uring_params params{};
    memset(&params, 0, sizeof(params));
   
    params.flags = IORING_SETUP_CQSIZE;       // explicitly specify the size of the Completion Queue.
    params.cq_entries = entries * 4;          // CQ entries = SQ entries × 4
   
    int res = io_uring_queue_init_params(entries, ring, &params);
    if (res < 0) {
        res = io_uring_queue_init(entries, ring, 0);
        if (res < 0)
            return false;
    }
   
    return true;
}


// API can be mapped to Multiple I/O ring buffers and Memory pools

static bool init_provided_buffer_pool(io_uring *ring, BufferPool *pool) {
    pool->count = ACTIVE_BUFFER_POOL_SIZE;
    pool->size = BUFFER_SIZE;
    pool->bgid = BUFFER_GROUP_ID;

//  Here we create an association between io_uring and Memory pool.
//  At this point, create a Pool of Memory buffer ring, but your actual 4096 × 4096-byte data buffers don't.
//  pool->buf_ring represents the buffer-ring metadata/entries that describe the pool of available memory buffers..
    int err = 0;
    pool->buf_ring = io_uring_setup_buf_ring(ring, pool->count, pool->bgid, 0, &err);
    if (!pool->buf_ring)
        return false;

//  Conceptually, the OS/runtime gives your process a chunk of memory with 4096-byte alignment.
    pool->total_size = static_cast<size_t>(pool->count) * pool->size;
    if (posix_memalign(&pool->memory, 4096, pool->total_size) != 0)
        return false;

// Here count can be dynamic, we need to find Appropriate mask for it.
    unsigned mask = io_uring_buf_ring_mask(pool->count);
   
    for (unsigned i = 0; i < pool->count; i++) {
        char * perSlotAddr = static_cast<char *>(pool->memory) + (static_cast<size_t>(i) * pool->size);
       
        // Register each buffer(slot) with the ring
        io_uring_buf_ring_add(pool->buf_ring, perSlotAddr, pool->size, i, mask, i);
    }

// Publish these newly added buffers so the kernel can use them.
// Here we tell the Linux kernel How many slots are available for him.
    io_uring_buf_ring_advance(pool->buf_ring, pool->count);
   
    return true;
}


static void recycle_buffer(BufferPool *pool, unsigned i) {
    unsigned mask = io_uring_buf_ring_mask(pool->count);
   
    char *slotAddr = static_cast<char *>(pool->memory) + (static_cast<size_t>(i) * pool->size);
   
    io_uring_buf_ring_add(pool->buf_ring, slotAddr, pool->size, i, mask, i);
    io_uring_buf_ring_advance(pool->buf_ring, 1);
}


static int create_reuseport_listening_socket() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}


static bool submit_accept(io_uring *ring, int server_fd, RequestContext *req) {
    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return false;

/*
    io_uring_prep_multishot_accept(
        sqe,          // SQE to configure with the multishot ACCEPT operation
        server_fd,    // Listening socket FD from which new client connections are accepted
        nullptr,      // Don't request the client's sockaddr/address information
        nullptr,      // No address-length variable because addr is nullptr
        0             // No special accept flags
    );
*/

    io_uring_prep_multishot_accept(sqe, server_fd, nullptr, nullptr, 0);
   
   
// io_uring_sqe_set_data(sqe, req) does not tell the kernel where to put network data.
// It tells io_uring which application object you want associated with the eventual completion.
    io_uring_sqe_set_data(sqe, req);

    return true;
}


static bool submit_multishot_recv(io_uring *ring, Connection *conn, RequestContext *req) {
    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return false;

/*
    io_uring_prep_recv_multishot (
        sqe,        // SQE to configure
        conn->fd,   // socket to receive from
        nullptr,    // buffer address
        0,          // buffer length
        0           // recv flags
    );
*/

    io_uring_prep_recv_multishot(sqe, conn->fd, nullptr, 0, 0);
   
// Select buffer from the provided-buffer pool group id.
// Does this tell the submission request will not contain a buffer explicitly.

    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_GROUP_ID;

// Associate the RequestContext to submission queue entry
// When submission request is completed, you can retrieve it from the CQE:
    io_uring_sqe_set_data(sqe, req);
   
    conn->multishot_recv_active = true;
   
    return true;
}


static bool submit_send(io_uring *ring, RequestContext *req, BufferPool *pool)
{
// You're just getting a slot in which to describe the send operation.  
    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return false;

    char *buf_addr = static_cast<char *>(pool->memory) + (static_cast<size_t>(req->buffer_id) * pool->size);

    io_uring_prep_send(
        sqe,
        req->conn->fd,
        buf_addr + req->bytes_sent,
        req->bytes_to_send - req->bytes_sent,
        MSG_NOSIGNAL   // prevents the process from receiving a SIGPIPE if the peer has closed the connection.
    );

// Associate the RequestContext to submission queue entry
// When submission request is completed, you can retrieve it from the CQE:
    io_uring_sqe_set_data(sqe, req);
   
    return true;
}



static void close_connection(Connection *conn, size_t *active_conns) {
    if (!conn)
        return;
   
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
   
    delete conn;
   
    if (*active_conns > 0) {
        (*active_conns)--;
   
        g_total_connections.fetch_sub(1, std::memory_order_relaxed);
    }
}


static void handle_accept(io_uring *ring, io_uring_cqe *cqe, size_t *active_conns) {
//For an accept completion, cqe->res contains the result of the accept() operation.
    int client_fd = cqe->res;
    if (client_fd < 0)
        return;

    if (*active_conns >= MAX_CONNECTIONS_PER_THREAD) {
        close(client_fd);
        return;
    }

    Connection *conn = new Connection{client_fd, false};
    RequestContext *recv_ctx = new RequestContext{OP_RECV, conn, 0, 0, 0};

    (*active_conns)++;
    g_total_connections.fetch_add(1, std::memory_order_relaxed);

//  So now the connection is ready to receive data asynchronously.
    if (!submit_multishot_recv(ring, conn, recv_ctx)) {
        delete recv_ctx;
        close_connection(conn, active_conns);
    }
}

// for a single multishot SQE, its CQEs are produced in the order the individual completions occur for that operation.
//     RECV SQE A --> A1 --> A2 --> A3
//     SEND SQE B --> B1 --> B2
// the CQ may contain : A1, B1, A2, B2, A3
// The completions can be interleaved with completions from other requests.

static void handle_recv(io_uring *ring, BufferPool *pool, io_uring_cqe *cqe, RequestContext *req, size_t *active_conns)
{
//  So how do you know CQEs belong to the same multishot operation? You use user_data inside CQE entry to identify it,
    Connection *conn = req->conn;
    int res = cqe->res;

    bool has_more = (cqe->flags & IORING_CQE_F_MORE);
    if (!has_more)
        conn->multishot_recv_active = false;

    if (res <= 0) {
        if (cqe->flags & IORING_CQE_F_BUFFER)
        {
//  How does your application know which buffer Linux selected?
//  The kernel puts the buffer ID into the CQE flags.

            unsigned kernel_used_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            recycle_buffer(pool, kernel_used_id);
        }
       
        // Attempt recovery if buffer pool was temporarily exhausted
        if (res == -ENOBUFS && conn->fd >= 0) {
            submit_multishot_recv(ring, conn, req);
            return;
        }

        // Multishot finished or errored; cleanup request and socket
        if (!has_more) {
            delete req;
        }
       
        close_connection(conn, active_conns);
        return;
    }

// Data was received ... Handling scenario where res > 0
   
// If the buffer flag isn't present inside completion queue entry.
// your application doesn't know where the received data is.

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        delete req;
       
        close_connection(conn, active_conns);
        return;
    }

    // Increment message counters
    g_total_messages.fetch_add(1, std::memory_order_relaxed);
    g_total_bytes.fetch_add(res, std::memory_order_relaxed);

    unsigned buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

// create a SEND RequestContext
    RequestContext *send_req = new RequestContext{ OP_SEND, conn, buf_id, static_cast<size_t>(res), 0};

    if (!submit_send(ring, send_req, pool)) {
       
        // send preparation fails -- We need to handle that scenario
        recycle_buffer(pool, buf_id);
       
        delete send_req;
        delete req;
       
        close_connection(conn, active_conns);
       
        return;
    }
   
// Re-arm multishot RECV if the kernel terminated the existing multishot stream
    if (!has_more && conn->fd >= 0) {
        RequestContext *new_recv_req = new RequestContext{OP_RECV, conn, 0, 0, 0};
       
        if (!submit_multishot_recv(ring, conn, new_recv_req))
            delete new_recv_req;
       
        delete req;
    }
}



static void handle_send(io_uring *ring, BufferPool *pool, io_uring_cqe *cqe, RequestContext *req, size_t *active_conns) {
    Connection *conn = req->conn;
   
    int res = cqe->res;
    if (res < 0)
    {
        // Handling send failure
       
        recycle_buffer(pool, req->buffer_id);
        delete req;
        close_connection(conn, active_conns);
       
        return;
    }
    else
    {
        // Handling partial send
        req->bytes_sent += res;
        if (req->bytes_sent < req->bytes_to_send) {
       
            //  We still have data remaining. Submit the remaining data
            if (!submit_send(ring, req, pool)) {
                recycle_buffer(pool, req->buffer_id);
                delete req;
                close_connection(conn, active_conns);
            }
       
            return;
        }

        recycle_buffer(pool, req->buffer_id);
   
        delete req;
    }
}


void worker_thread_loop(WorkerThread *worker) {
   
// Pin the worker thread to a CPU
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker->cpu_core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

// Initialize io_uring and provided-buffer pool

    if (!init_ring(&worker->ring, QUEUE_ENTRIES)) return;
    if (!init_provided_buffer_pool(&worker->ring, &worker->pool)) return;

    worker->server_fd = create_reuseport_listening_socket();
    if (worker->server_fd < 0)
        return;

    RequestContext accept_req{OP_ACCEPT, nullptr, 0, 0, 0};

    if (!submit_accept(&worker->ring, worker->server_fd, &accept_req))
        return;

    while (true)
    {
        //  Wait for at least one completion
        int res = io_uring_submit_and_wait(&worker->ring, 1);
        if (res < 0) {
            if (res == -EINTR)  //  If the thread is interrupted by a signal
                continue;
           
            break;
        }

        io_uring_cqe *cqe = nullptr;
        unsigned head = 0;

        io_uring_for_each_cqe(&worker->ring, head, cqe) {
       
//  Give me back the user-data pointer associated with the I/O operation that produced this CQE.
            RequestContext *req = static_cast<RequestContext *>(io_uring_cqe_get_data(cqe));
           
            if (req) {
                switch (req->type) {
                    case OP_ACCEPT:
                        handle_accept(&worker->ring, cqe, &worker->active_connections);
                       
                        if (!(cqe->flags & IORING_CQE_F_MORE))
                            submit_accept(&worker->ring, worker->server_fd, &accept_req);
                        break;
                       
                    case OP_RECV:
                        handle_recv(&worker->ring, &worker->pool, cqe, req, &worker->active_connections);
                        break;
                   
                    case OP_SEND:
                        handle_send(&worker->ring, &worker->pool, cqe, req, &worker->active_connections);
                        break;
                }
            }
           
            // I have consumed this CQE; you can mark it as seen/reclaim it from the CQ.
            io_uring_cqe_seen(&worker->ring, cqe);
        }
    }
}



// Background Monitor Thread: Prints non-blocking metrics every 1 second
void stats_monitor_thread() {
    size_t prev_msgs = 0;
    size_t prev_bytes = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        size_t current_conns  =  g_total_connections.load(std::memory_order_relaxed);
        size_t total_msgs     =  g_total_messages.load(std::memory_order_relaxed);
        size_t total_bytes    =  g_total_bytes.load(std::memory_order_relaxed);

        size_t mps  = total_msgs - prev_msgs;
        double mbps = static_cast<double>(total_bytes - prev_bytes) / (1024.0 * 1024.0);

        prev_msgs  = total_msgs;
        prev_bytes = total_bytes;

        if (current_conns > 0 || mps > 0) {
            cout << "[STATS] Active Conns: " << current_conns
                 << " | Throughput: " << mps << " msgs/sec ("
                 << fixed << std::setprecision(2) << mbps << " MB/s)"
                 << " | Total Msgs: " << total_msgs << endl;
        }
    }
}



int main()
{
    unsigned num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0)
        num_cores = 4;

    cout << "\n========================================================" << endl;
    cout << "     IO_URING HIGH-PERFORMANCE SERVER STARTED           " << endl;
    cout << "========================================================" << endl;
    cout << "Listening Port        : " << SERVER_PORT << endl;
    cout << "Worker Threads        : " << num_cores << endl;
    cout << "========================================================\n" << endl;


    // Start background metrics monitor thread
    std::thread monitor(stats_monitor_thread);
    monitor.detach();

    vector<WorkerThread> workers(num_cores);
    vector<thread> threads;

    for (unsigned i = 0; i < num_cores; ++i) {
        workers[i].thread_id = i;
        workers[i].cpu_core = i;

        threads.emplace_back(worker_thread_loop, &workers[i]);
    }


    for (auto &t : threads) {
        if (t.joinable())
            t.join();
    }

    return 0;
}



