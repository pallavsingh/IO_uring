#include <liburing.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <thread>
#include <cstring>
#include <cstdlib>


#define PORT               9000
#define QUEUE_ENTRIES      4096
#define BUF_SIZE           2048
#define BUF_COUNT          256
#define BUFFER_GROUP_ID    1


/*
Asynchronous opcodes:

OP_NOP: Submitted right after ring initialization to benchmark ring latency and 
        verify SQE submission pipeline integrity.

OP_TIMEOUT: A non-blocking periodic timer (e.g., 5-second tick) re-armed upon completion 
            to handle heartbeats, session timeouts, or ring maintenance.

OP_PROVIDE_BUFFERS: Pre-registers a contiguous block of memory to an io_uring buffer group (bgid) 
            so the kernel can automatically select buffers during dynamic I/O (IOSQE_BUFFER_SELECT).

OP_REMOVE_BUFFERS: Unregisters and frees buffer pool allocations during clean shutdown or teardown steps.
*/


enum OpCode {
    OP_NOP,
    OP_ACCEPT,
    OP_SEND,
    OP_RECV,
    OP_TIMEOUT,
    OP_PROVIDE_BUFFERS,
    OP_REMOVE_BUFFERS
};



struct Request {
	OpCode type;
    int client_fd{-1};
    unsigned buffer_id{0};
    size_t bytes_transferred{0};
    __kernel_timespec ts{};
};



struct BufferPool {
    void * memory{nullptr};
    size_t total_size{0};
};


static void submit_nop(io_uring * ring){
    io_uring_sqe * sqe = io_uring_get_sqe(ring);
    Request * req = new Request{OpCode::OP_NOP};

    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, req);
};


static void submit_timeout(io_uring * ring, unsigned long seconds){
    io_uring_sqe * sqe = io_uring_get_sqe(ring);
    Request * req = new Request{OpCode::OP_TIMEOUT};

    req->ts.tv_sec = seconds;
    req->ts.tv_nsec = 0;

    io_uring_prep_timeout(sqe, &req->ts, 0, 0);
    io_uring_sqe_set_data(sqe, req);
};


/*
UserSpace ==>>
io_uring_prep_provide_buffers(): doesn't immediately provide the buffers. 
Itself is an SQE operation. uses the IORING_OP_PROVIDE_BUFFERS SQE operation.
Here is a memory region containing BUF_COUNT buffers, each BUF_SIZE bytes. 
Put these buffers into buffer group BUFFER_GROUP_ID.

You still need to submit the SQE: io_uring_submit(ring); and process its CQE.


io_uring_prep_provide_buffers(
    sqe,
    addr,
    len,
    nr,
    bgid,
    bid
);

bgid = buffer group ID
bid = starting buffer ID within that group

A single io_uring instance can have multiple io_uring_prep_provide_buffers() operations, 
and they can populate the same or different buffer groups And also diffrent starting bid.


Request* req = ...;
io_uring_sqe_set_data(sqe, req);

Important: Application-context pointer(req) is attached to the SQE, 
and io_uring carries that same 64-bit user_data value into the corresponding CQE.

KernelSpace ==>>
io_uring_setup_buf_ring() :  uses the provided buffer ring (io_uring_buf_ring), 
which is the newer/more efficient mechanism for managing provided buffers.
*/


static bool submit_provide_buffers(io_uring* ring, BufferPool* pool) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return false;

    pool->total_size = BUF_COUNT * BUF_SIZE;
    if (posix_memalign(&pool->memory, 4096, pool->total_size) != 0) 
        return false;

    io_uring_prep_provide_buffers(sqe, pool->memory, BUF_SIZE, BUF_COUNT, BUFFER_GROUP_ID, 0);

	Request* req = new Request{OP_PROVIDE_BUFFERS};
    io_uring_sqe_set_data(sqe, req);
    
    return true;
}


static void submit_remove_buffers(io_uring* ring) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    
    io_uring_prep_remove_buffers(sqe, BUF_COUNT, BUFFER_GROUP_ID);

	Request* req = new Request{OP_REMOVE_BUFFERS};
    io_uring_sqe_set_data(sqe, req);
}


static void recycle_buffer(io_uring *ring, BufferPool *pool, unsigned buf_id) {
    char *slot_addr = static_cast<char *>(pool->memory) + (static_cast<size_t>(buf_id) * BUF_SIZE);

    io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe) {
        io_uring_prep_provide_buffers(sqe, slot_addr, BUF_SIZE, 1, BUFFER_GROUP_ID, buf_id);
        io_uring_sqe_set_data(sqe, nullptr);
    }
}

// Kernel, wait for a new TCP connection on server_fd and, when one arrives, create a new connected socket.
// Accept the connection, but I don't need the client's sockaddr information.

static void submit_accept(io_uring* ring, int server_fd) {
	io_uring_sqe* sqe = io_uring_get_sqe(ring);

    io_uring_prep_accept(sqe, server_fd, nullptr, nullptr, 0);

	Request* req = new Request{OP_ACCEPT};
    io_uring_sqe_set_data(sqe, req);
}


// Select buffer from the provided-buffer pool group id.
// Does this tell the submission request will not contain a buffer explicitly.

static void submit_recv(io_uring* ring, int client_fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    io_uring_prep_recv(sqe, client_fd, nullptr, 0, 0);

	Request* req = new Request{OP_RECV, client_fd};

    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_GROUP_ID;

    io_uring_sqe_set_data(sqe, req);
}


static void submit_send(io_uring* ring, int client_fd, BufferPool* pool, unsigned buf_id, size_t len) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    char *buf_addr = static_cast<char *>(pool->memory) + (static_cast<size_t>(buf_id) * BUF_SIZE);

// Now you cannot immediately put buffer 42 back into the provided-buffer pool.
// while the send is still pending, you have a buffer-lifetime/race problem.

// When this SQE is executed, send len bytes from buf_addr to client_fd.
// Processed by your application does not necessarily mean available for reuse.
    io_uring_prep_send(sqe, client_fd, buf_addr, len, MSG_NOSIGNAL);

	Request* req = new Request{OP_SEND, client_fd, buf_id, len};
    io_uring_sqe_set_data(sqe, req);
}


void worker() {
    io_uring ring;
    io_uring_queue_init(QUEUE_ENTRIES, &ring, 0);

    BufferPool pool;
    submit_provide_buffers(&ring, &pool);
    submit_nop(&ring);
    submit_timeout(&ring, 5);

    int server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, SOMAXCONN);

    submit_accept(&ring, server_fd);

    while (true) {
        io_uring_submit_and_wait(&ring, 1);
        io_uring_cqe* cqe;
        unsigned head;

// Request* req = io_uring_cqe_get_data(cqe) : "Which application operation was this?"
// cqe->res  = 4096  : "What was the result of the I/O?"

        io_uring_for_each_cqe(&ring, head, cqe) {
            Request* req = static_cast<Request*>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;

            if (req) {
                switch (req->type) {
                    case OP_NOP:
                        std::cout << "[TCP Server] OP_NOP completed successfully.\n";
                        delete req;
                        break;
                    case OP_TIMEOUT:
                        std::cout << "[TCP Server] OP_TIMEOUT tick (5s expired). Re-arming timer.\n";
                        submit_timeout(&ring, 5);
                        delete req;
                        break;
                    case OP_PROVIDE_BUFFERS:
                        std::cout << "[TCP Server] OP_PROVIDE_BUFFERS memory pool initialized.\n";
                        delete req;
                        break;
                    case OP_REMOVE_BUFFERS:
                        if (pool.memory) free(pool.memory);
                        delete req;
                        break;
                    case OP_ACCEPT:

                        // We need to have a buffer pool available before posting a receive operation, 
                        // because incoming application data needs a destination buffer. 
                        // Accepting a connection itself does not require an application data buffer.

                        if (res >= 0) {
                            std::cout << "[TCP Server] New client connected on fd " << res << "\n";
                            submit_recv(&ring, res);
                        }

                        submit_accept(&ring, server_fd);
                        delete req;
                        break;
                    case OP_RECV: {
                        int client_fd = req->client_fd;

                        if (res <= 0)
                        {
                            std::cout << "[TCP Server] Client on fd " << client_fd << " disconnected.\n";
                            if (cqe->flags & IORING_CQE_F_BUFFER) {
                                unsigned buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                                recycle_buffer(&ring, &pool, buf_id);
                            }

                            close(client_fd);
                        } 
                        else 
                        {
                            unsigned buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                            char *slot_addr = static_cast<char *>(pool.memory) + (static_cast<size_t>(buf_id) * BUF_SIZE);

                            // Processing Recieved Buffer : Print received message string
                            std::string recv_msg(slot_addr, res);
                            std::cout << "[TCP Server] Received from client (fd " << client_fd << "): \"" << recv_msg << "\"\n";
                            
                            // Send the data I just received back to the client.
                            submit_send(&ring, client_fd, &pool, buf_id, res);

                            // I'm ready to receive the client's next data.
                            // What happens if you DON'T call submit_recv()?
                            submit_recv(&ring, client_fd);
                        }

                        delete req;
                        break;
                    }
                    case OP_SEND:
                        std::cout << "[TCP Server] Echoed response back to client (fd " << req->client_fd << ").\n";

                        // Processing CQE for the SEND operation.
                        // The send operation has completed, so the kernel no longer needs
                        // this application buffer. It is now safe to recycle the buffer.

                        recycle_buffer(&ring, &pool, req->buffer_id);
                        delete req;
                        break;
                }
            }

            // I have consumed this CQE; you can mark it as seen/reclaim it from the CQ.
            io_uring_cqe_seen(&ring, cqe);
        }
    }


    // 1. Stop accepting/submitting new I/O.
    // 2. Stop the listening socket.
    close(server_fd);

    // 3. Cancel/drain outstanding ACCEPT/RECV/SEND operations.
    //    They must no longer be using application buffers.
    // 4. Remove provided buffers.
    submit_remove_buffers(&ring);

    // 5. Submit the REMOVE_BUFFERS SQE.
    io_uring_submit(&ring);

    // 6. Wait for REMOVE_BUFFERS CQE.
    io_uring_cqe* cqe;
    io_uring_wait_cqe(&ring, &cqe);

    // Check cqe->res here.
    io_uring_cqe_seen(&ring, cqe);

    // 7. Now the remove-buffer operation itself is complete.
    // 8. Free buffer memory, if applicable.
    // wait for all my outstanding operations to finish.
    // 9. Destroy io_uring.
    io_uring_queue_exit(&ring);
}


int main() {
    // Disable std::cout buffering so every write flushes to disk immediately
    std::cout << std::unitbuf;

    std::cout << "Starting TCP Server on port " << PORT << "...\n";
    worker();
    return 0;
}


