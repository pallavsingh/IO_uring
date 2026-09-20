#include <liburing.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <cstdlib>


#define PORT              9000
#define BUF_SIZE          2048
#define BUF_COUNT         16
#define BUFFER_GROUP_ID   1


enum OpType { 
    OP_NOP, 
    OP_TIMEOUT, 
    OP_PROVIDE_BUFFERS, 
    OP_REMOVE_BUFFERS, 
    OP_CONNECT, 
    OP_SEND, 
    OP_RECV 
};


struct Request {
    OpType type;
    int fd{-1};
    unsigned buffer_id{0};
    __kernel_timespec ts{};
};


struct BufferPool {
    void *memory{nullptr};
    size_t total_size{0};
};


static void submit_nop(io_uring* ring) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    io_uring_prep_nop(sqe);

    Request* req = new Request{OP_NOP};
    io_uring_sqe_set_data(sqe, req);
}



static void submit_timeout(io_uring* ring, unsigned long seconds) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    Request* req = new Request{OP_TIMEOUT};
    req->ts.tv_sec = seconds;
    req->ts.tv_nsec = 0;

    io_uring_prep_timeout(sqe, &req->ts, 0, 0);
    io_uring_sqe_set_data(sqe, req);
}



static bool submit_provide_buffers(io_uring* ring, BufferPool* pool) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) return false;

    pool->total_size = BUF_COUNT * BUF_SIZE;
    if (posix_memalign(&pool->memory, 4096, pool->total_size) != 0) return false;

    Request* req = new Request{OP_PROVIDE_BUFFERS};
    io_uring_prep_provide_buffers(sqe, pool->memory, BUF_SIZE, BUF_COUNT, BUFFER_GROUP_ID, 0);
    io_uring_sqe_set_data(sqe, req);

    return true;
}



static void submit_remove_buffers(io_uring* ring) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    io_uring_prep_remove_buffers(sqe, BUF_COUNT, BUFFER_GROUP_ID);

    Request* req = new Request{OP_REMOVE_BUFFERS};
    io_uring_sqe_set_data(sqe, req);
}



int main() {
    io_uring ring;
    io_uring_queue_init(128, &ring, 0);

    BufferPool pool;
    submit_provide_buffers(&ring, &pool);
    submit_nop(&ring);
    submit_timeout(&ring, 10);

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // Submit Connect
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    Request* conn_req = new Request{OP_CONNECT, sock};
    io_uring_prep_connect(sqe, sock, (sockaddr*)&addr, sizeof(addr));
    io_uring_sqe_set_data(sqe, conn_req);
    io_uring_submit(&ring);

    bool running = true;
    while (running) {
        io_uring_cqe* cqe;
        io_uring_wait_cqe(&ring, &cqe);

        Request* req = static_cast<Request*>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;

        if (req) {
            switch (req->type) {
                case OP_NOP:
                    std::cout << "[TCP Client] OP_NOP execution complete.\n";
                    break;
                case OP_TIMEOUT:
                    std::cout << "[TCP Client] OP_TIMEOUT guard expired.\n";
                    break;
                case OP_PROVIDE_BUFFERS:
                    std::cout << "[TCP Client] OP_PROVIDE_BUFFERS registered.\n";
                    break;
                case OP_REMOVE_BUFFERS:
                    if (pool.memory) free(pool.memory);
                    break;
                case OP_CONNECT: {
                    if (res < 0) {
                        std::cerr << "[TCP Client] Connect failed: " << strerror(-res) << "\n";
                        running = false;
                        break;
                    }
                    std::cout << "[TCP Client] Connected to TCP Server!\n";

                    const char* msg = "Hello TCP Server via io_uring!";
                    std::memcpy(pool.memory, msg, strlen(msg));

                    // Print message before sending
                    std::cout << "[TCP Client] Sending message to server: \"" << msg << "\"\n";

                    sqe = io_uring_get_sqe(&ring);

                    io_uring_prep_send(sqe, sock, pool.memory, strlen(msg), 0);

                    Request* send_req = new Request{OP_SEND, sock, 0};
                    io_uring_sqe_set_data(sqe, send_req);
                    io_uring_submit(&ring);

                    break;
                }
                case OP_SEND: {
                    std::cout << "[TCP Client] Message successfully sent (" << res << " bytes). Awaiting response...\n";
                    sqe = io_uring_get_sqe(&ring);

                    io_uring_prep_recv(sqe, sock, nullptr, 0, 0);

                    Request* recv_req = new Request{OP_RECV, sock};
                    sqe->flags |= IOSQE_BUFFER_SELECT;
                    sqe->buf_group = BUFFER_GROUP_ID;

                    io_uring_sqe_set_data(sqe, recv_req);
                    io_uring_submit(&ring);

                    break;
                }
                case OP_RECV: {
                    if (res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
                        unsigned buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                        char *slot = static_cast<char *>(pool.memory) + (buf_id * BUF_SIZE);
                        slot[res] = '\0';
                        std::cout << "[TCP Client] Received Echo back from server: \"" << slot << "\"\n";
                    }

                    running = false;
                    break;
                }
            }

            delete req;
        }

        io_uring_cqe_seen(&ring, cqe);
    }


    submit_remove_buffers(&ring);
    close(sock);
    io_uring_queue_exit(&ring);

    return 0;
}
