/*
 * High-Performance Reference & Learning Engine for io_uring Architecture.
 *
 * Features:
 *   - Lock-free index array allocator (Zero-allocation fast path)
 *   - Kernel feature probing & runtime fallback
 *   - Explicit Request State Machine with partial I/O resubmission handling
 *   - Flexible Workload Drivers (Seq/Rand Read/Write, File Copy)
 *
 *   Workload Types: seq_read, seq_write, rand_read, rand_write, copy
 */

#include <liburing.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <new>

using namespace std;

static constexpr unsigned QUEUE_DEPTH       = 512;
static constexpr size_t   BUFFER_SIZE       = 64 * 1024; // 64 KB Block Size
static constexpr unsigned MAX_BATCH_SUBMIT  = 64;
static constexpr unsigned MAX_BATCH_CQE     = 64;
static constexpr size_t   FILE_SIZE         = 1024ULL * 1024ULL * 1024ULL; // 1 GB Target File

// SubType of I/O Request
enum class WorkloadType {
    SEQ_READ,
    SEQ_WRITE,
   
    RAND_READ,
    RAND_WRITE,
   
    FILE_COPY
};

// FSM States for File I/O
enum class ReqState : uint8_t {
    FREE = 0,
    PREPARED,
    SUBMITTED,
    PARTIAL_RETRY,
    COMPLETED
};


// Type of Work
enum class OpType : uint8_t {
    READ,
    WRITE
};


// Forward declaration for WorkerContext
class FreeListAllocator;

// Per I/O Request data set
struct IORequest {
    uint32_t   req_id;
    uint32_t   buf_index;
    void*      buffer_addr;
    ReqState   state;
   
    OpType     type;
    int        fd;
   
    uint64_t   initial_offset;
    uint64_t   current_offset;
   
    size_t     total_bytes;
    size_t     bytes_transferred;
   
    int        error_code;
};


// Worker Context placed directly below IORequest
struct alignas(64) WorkerContext {
    WorkerContext() = default;
    ~WorkerContext() = default;

    // Custom operator new/delete to satisfy alignas(64) during dynamic allocation
    void* operator new(size_t count) {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 64, count) != 0) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    void operator delete(void* ptr) noexcept {
        free(ptr);
    }

    unsigned id = 0;
    int cpu_core = 0;

    // Here we Create Fix size pool of I/O request
    // Here io_uring will take the slotbuffer from request_pool.
    io_uring ring{ };
    array<IORequest, QUEUE_DEPTH> request_pool;         // req_id, buf_index, buffer_addr, state

    alignas(4096) uint8_t* raw_buffer_pool = nullptr;   // Store address of memory holding metadata of memory pool
    vector<iovec> iovecs;                               // iovec contains base address and size of each memory buffer
   
    unique_ptr<FreeListAllocator> slot_allocator;

    // Copy operations : primary_fd -->> dst_fd
    int primary_fd = -1;
    int dst_fd = -1;
    bool buffers_registered = false;
   
    atomic<bool> finished{false};

    WorkloadType mode = WorkloadType::RAND_READ;

    // Metrics Tracking
    uint64_t ops_submitted = 0;
    uint64_t ops_completed = 0;
    uint64_t bytes_processed = 0;
    uint64_t partial_retries = 0;
   
    uint64_t total_errors = 0;
};



// Lock-Free Index Free-List (Pre-allocated Slot Manager)
class FreeListAllocator {
private:
    vector<uint32_t> m_free_stack;
    size_t m_top{0};

public:
    void init(uint32_t capacity) {
        m_free_stack.resize(capacity);
        for (uint32_t i = 0; i < capacity; ++i) {
            m_free_stack[i] = (capacity - 1) - i;
        }
        m_top = capacity;
    }

    inline bool allocate(uint32_t& out_index) {
        if (m_top == 0) return false;
        out_index = m_free_stack[--m_top];
        return true;
    }

    inline void release(uint32_t index) {
        m_free_stack[m_top++] = index;
    }

    inline bool full() const { return m_top == m_free_stack.size(); }
    inline size_t available() const { return m_top; }
};



struct HardwareCapabilities {
    bool supports_fixed_buffers = false;
    bool supports_fixed_files = false;

    // This asks the kernel/liburing which io_uring operations (opcodes) are supported on the current system.
    void probe_system() {
        io_uring_probe* probe = io_uring_get_probe();
        if (!probe) {
            cout << "[Probe] Kernel probe unavailable - standard defaults enabled.\n";
            return;
        }

        if (io_uring_opcode_supported(probe, IORING_OP_READ_FIXED)) {
            supports_fixed_buffers = true;
        }

        if (io_uring_opcode_supported(probe, IORING_OP_NOP)) {
            supports_fixed_files = true;
        }

        io_uring_free_probe(probe);

        cout << "[Probe Capabilities] Fixed Buffers: " << (supports_fixed_buffers ? "YES" : "NO") << "\n";
    }
};



static HardwareCapabilities g_caps;



static bool init_worker(WorkerContext& ctx) {
    io_uring_params params{};
    memset(&params, 0, sizeof(params));
   
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = QUEUE_DEPTH * 2;

    int rc = io_uring_queue_init_params(QUEUE_DEPTH, &ctx.ring, &params);
    if (rc < 0) {
        cerr << "[Worker " << ctx.id << "] Ring init error: " << strerror(-rc) << "\n";
        return false;
    }

    size_t total_mem = QUEUE_DEPTH * BUFFER_SIZE;
    if (posix_memalign((void**)&ctx.raw_buffer_pool, 4096, total_mem) != 0) {
        cerr << "[Worker " << ctx.id << "] Aligned memory allocation failed.\n";
        io_uring_queue_exit(&ctx.ring);
        return false;
    }

    ctx.iovecs.resize(QUEUE_DEPTH);
   
    for (unsigned i = 0; i < QUEUE_DEPTH; ++i) {
        ctx.iovecs[i].iov_base = ctx.raw_buffer_pool + (i * BUFFER_SIZE);
        ctx.iovecs[i].iov_len = BUFFER_SIZE;
       
        ctx.request_pool[i].req_id = i;
        ctx.request_pool[i].buf_index = i;
        ctx.request_pool[i].buffer_addr = ctx.iovecs[i].iov_base;
        ctx.request_pool[i].state = ReqState::FREE;
    }

    ctx.slot_allocator = std::make_unique<FreeListAllocator>();
    ctx.slot_allocator->init(QUEUE_DEPTH);

    // Registers a fixed set of user-space buffers with the io_uring instance.
    if (g_caps.supports_fixed_buffers) {
        rc = io_uring_register_buffers(&ctx.ring, ctx.iovecs.data(), QUEUE_DEPTH);
        if (rc == 0) {
            ctx.buffers_registered = true;
        }
    }

    return true;
}


static bool prepare_sqe(WorkerContext& ctx, IORequest& req) {
    // Here we GET the SLOT in Submission Queue
    io_uring_sqe* sqe = io_uring_get_sqe(&ctx.ring);
    if (!sqe)
        return false;

    // Determine target slice & byte offset within registered block
    uint8_t* current_buf_ptr = static_cast<uint8_t*>(req.buffer_addr) + req.bytes_transferred;
    size_t remaining_bytes = req.total_bytes - req.bytes_transferred;

    if (req.type == OpType::READ) {
        if (ctx.buffers_registered)
        {
            // Uses a buffer that was previously registered with io_uring.
            // req.buf_index tells io_uring which registered buffer to use.
       
            io_uring_prep_read_fixed(sqe, req.fd, current_buf_ptr, remaining_bytes, req.current_offset, req.buf_index);
        }
        else
        {
            // Uses the memory address directly.
            // No registered-buffer index is required.
           
            io_uring_prep_read(sqe, req.fd, current_buf_ptr, remaining_bytes, req.current_offset);
        }
    } else {
        if (ctx.buffers_registered)
        {
            io_uring_prep_write_fixed(sqe, req.fd, current_buf_ptr, remaining_bytes, req.current_offset, req.buf_index);
        }
        else
        {
            io_uring_prep_write(sqe, req.fd, current_buf_ptr, remaining_bytes, req.current_offset);
        }
    }

    // Here we Submit Request in Submission Queue.
    req.state = ReqState::SUBMITTED;
    io_uring_sqe_set_data(sqe, &req);
   
    return true;
}

// Helper to reliably resubmit a request when the SQ is full during completion handling
static void submit_and_retry_sqe(WorkerContext& ctx, IORequest& req) {
    while (!prepare_sqe(ctx, req)) {
        io_uring_submit(&ctx.ring);
    }
}

static void process_completions(WorkerContext& ctx) {
    io_uring_cqe* cqes[MAX_BATCH_CQE];
    unsigned count = io_uring_peek_batch_cqe(&ctx.ring, cqes, MAX_BATCH_CQE);

    for (unsigned i = 0; i < count; ++i) {
        io_uring_cqe* cqe = cqes[i];
        auto* req = static_cast<IORequest*>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;

        if (req) {
            if (res < 0) {
                if (res == -EINTR) {
                    req->state = ReqState::PARTIAL_RETRY;
                    ctx.partial_retries++;
                    submit_and_retry_sqe(ctx, *req);
                } else {
                    req->error_code = -res;
                    req->state = ReqState::COMPLETED;
                    ctx.total_errors++;
                    ctx.ops_completed++;
                    ctx.slot_allocator->release(req->buf_index);
                }
            } else {
                size_t bytes_transferred = static_cast<size_t>(res);
                req->bytes_transferred += bytes_transferred;
                req->current_offset += bytes_transferred;
                ctx.bytes_processed += bytes_transferred;

                if (req->bytes_transferred < req->total_bytes && bytes_transferred > 0) {
                    req->state = ReqState::PARTIAL_RETRY;
                    ctx.partial_retries++;
                    submit_and_retry_sqe(ctx, *req);
                } else {
                    // FILE_COPY State Machine Transition (READ -> WRITE)
                    if (ctx.mode == WorkloadType::FILE_COPY && req->type == OpType::READ) {
                        req->type = OpType::WRITE;
                        req->fd = ctx.dst_fd;
                        req->bytes_transferred = 0;
                        req->current_offset = req->initial_offset; // Reliably reset to original block offset
                        req->state = ReqState::PREPARED;
                        submit_and_retry_sqe(ctx, *req);
                        continue;
                    }

                    req->state = ReqState::FREE;
                    ctx.ops_completed++;
                    ctx.slot_allocator->release(req->buf_index);
                }
            }
        }
    }

    if (count > 0) {
        io_uring_cq_advance(&ctx.ring, count);
    }
}

static void perThreadExecution(WorkerContext& ctx, WorkloadType mode, uint64_t target_ops) {
    ctx.mode = mode;

    // Pin the worker to a CPU
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx.cpu_core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    // Initialize the worker
    if (!init_worker(ctx)) {
        ctx.finished = true;
        return;
    }

    // Create random-number generator and random offset distribution (4096-aligned)
    mt19937_64 rng(1337 + ctx.id);
    uint64_t max_block_idx = (FILE_SIZE > BUFFER_SIZE) ? ((FILE_SIZE - BUFFER_SIZE) / 4096) : 0;
    uniform_int_distribution<uint64_t> rand_dist(0, max_block_idx);

    uint64_t ops_dispatched = 0;
    uint64_t seq_offset = 0;
    uint64_t max_seq_offset = (FILE_SIZE >= BUFFER_SIZE) ? (FILE_SIZE - BUFFER_SIZE) : 0;

    // Continue while there are still operations that need to be dispatched OR
    // there are outstanding requests that haven't completed.
    while (ops_dispatched < target_ops || !ctx.slot_allocator->full())
    {
        // Step 1 — Dispatch a batch
        unsigned batch = 0;
       
        while (batch < MAX_BATCH_SUBMIT && ops_dispatched < target_ops) {
            uint32_t slot_idx;
           
            // Allocate a request slot
            if (!ctx.slot_allocator->allocate(slot_idx)) {
                break;
            }

            // Get the request and Reset request state
            IORequest& req = ctx.request_pool[slot_idx];
            req.bytes_transferred = 0;
            req.total_bytes = BUFFER_SIZE;
            req.error_code = 0;

            switch (mode) {
                case WorkloadType::SEQ_READ:
                    req.type = OpType::READ;
                    req.fd = ctx.primary_fd;
                    req.initial_offset = seq_offset;
                    req.current_offset = seq_offset;
                    seq_offset = (seq_offset + BUFFER_SIZE > max_seq_offset) ? 0 : seq_offset + BUFFER_SIZE;
                    break;

                case WorkloadType::SEQ_WRITE:
                    req.type = OpType::WRITE;
                    req.fd = ctx.primary_fd;
                    req.initial_offset = seq_offset;
                    req.current_offset = seq_offset;
                    seq_offset = (seq_offset + BUFFER_SIZE > max_seq_offset) ? 0 : seq_offset + BUFFER_SIZE;
                    break;

                case WorkloadType::RAND_READ:
                    req.type = OpType::READ;
                    req.fd = ctx.primary_fd;
                    req.initial_offset = rand_dist(rng) * 4096;
                    req.current_offset = req.initial_offset;
                    break;

                case WorkloadType::RAND_WRITE:
                    req.type = OpType::WRITE;
                    req.fd = ctx.primary_fd;
                    req.initial_offset = rand_dist(rng) * 4096;
                    req.current_offset = req.initial_offset;
                    break;

                // First step of pipeline (Read src -> Write dst)
                case WorkloadType::FILE_COPY:
                    req.type = OpType::READ;
                    req.fd = ctx.primary_fd;
                    req.initial_offset = seq_offset;
                    req.current_offset = seq_offset;
                    seq_offset = (seq_offset + BUFFER_SIZE > max_seq_offset) ? 0 : seq_offset + BUFFER_SIZE;
                    break;
            }

            req.state = ReqState::PREPARED;

            if (prepare_sqe(ctx, req)) {
                ops_dispatched++;
                ctx.ops_submitted++;
                batch++;
            } else {
                ctx.slot_allocator->release(slot_idx);
                break;
            }
        }

        // Step 2: Flush SQEs to Kernel Space
        if (batch > 0) {
            io_uring_submit(&ctx.ring);
        }

        // Step 3: Process CQE Engine Queue
        process_completions(ctx);
    }

    // Step 4: Drain Remaining In-Flight Requests
    while (!ctx.slot_allocator->full()) {
        io_uring_submit_and_wait(&ctx.ring, 1);
        process_completions(ctx);
    }


    if (ctx.buffers_registered) {
        io_uring_unregister_buffers(&ctx.ring);   //Removes registered-buffer relationship
    }

    free(ctx.raw_buffer_pool);                    // Releases your allocated I/O memory
    io_uring_queue_exit(&ctx.ring);               // Destroys the io_uring instance
    ctx.finished = true;                          // Marks worker as finished
}



int main(int argc, char* argv[]) {
   
    string filename = "io_engine_target.dat";
    WorkloadType mode = WorkloadType::RAND_READ;

    unsigned num_workers = 4;
    uint64_t ops_per_worker = 100000;

    if (argc > 1)
        filename = argv[1];
   
    if (argc > 2) {
        string m_str = argv[2];
        if (m_str == "seq_read") mode = WorkloadType::SEQ_READ;
        else if (m_str == "seq_write") mode = WorkloadType::SEQ_WRITE;
        else if (m_str == "rand_read") mode = WorkloadType::RAND_READ;
        else if (m_str == "rand_write") mode = WorkloadType::RAND_WRITE;
        else if (m_str == "copy") mode = WorkloadType::FILE_COPY;
    }
   
    if (argc > 3)
        num_workers = static_cast<unsigned>(atoi(argv[3]));
   
    if (argc > 4)
        ops_per_worker = static_cast<uint64_t>(atoll(argv[4]));

    cout << "================ Setup Strategy ================"   << endl;
    cout << "Target File      : " <<   filename   << endl;
    cout << "Worker Threads   : " <<   num_workers   << endl;
    cout << "Ops per Worker   : " <<   ops_per_worker   << endl;
    cout << "Block Size       : " <<   (BUFFER_SIZE / 1024)   << " KB "   << endl;

    g_caps.probe_system();

    // O_DIRECT means the application talks directly to the SSD.
    // O_DIRECT is mainly about avoiding Linux's normal page cache
       
    int fd = open(filename.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        fd = open(filename.c_str(), O_RDWR | O_CREAT, 0644);
    }
   
    // ftruncate() changes the file's logical size.
    // It does not necessarily allocate all physical blocks immediately.
   
    if (fd >= 0) {
        ftruncate(fd, FILE_SIZE);
    }

    int dst_fd = -1;
    if (mode == WorkloadType::FILE_COPY) {
        string dst_filename = filename + ".copy";
       
        dst_fd = open(dst_filename.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
        if (dst_fd < 0) {
            dst_fd = open(dst_filename.c_str(), O_RDWR | O_CREAT, 0644);
        }
       
        if (dst_fd >= 0) {
            ftruncate(dst_fd, FILE_SIZE);
        }
    }

    vector<unique_ptr<WorkerContext>> workers;
    vector<thread> threads;

    for (unsigned i = 0; i < num_workers; ++i) {
        auto w = std::make_unique<WorkerContext>();
        w->id = i;
        w->cpu_core = i % thread::hardware_concurrency();
        w->primary_fd = fd;
        w->dst_fd = dst_fd;
        workers.push_back(move(w));
    }

    auto start_time = chrono::high_resolution_clock::now();

    for (unsigned i = 0; i < num_workers; ++i) {
        threads.emplace_back(perThreadExecution, ref(*workers[i]), mode, ops_per_worker);
    }

    for (auto& t : threads) {
        if (t.joinable())
            t.join();
    }

    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double> diff = end_time - start_time;

    uint64_t total_ops = 0;
    uint64_t total_bytes = 0;
    uint64_t total_retries = 0;
    uint64_t total_errors = 0;

    for (const auto& w : workers) {
        total_ops += w->ops_completed;
        total_bytes += w->bytes_processed;
        total_retries += w->partial_retries;
        total_errors += w->total_errors;
    }

    double total_gb = static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0);
    double iops = static_cast<double>(total_ops) / diff.count();

    cout << "\n================ Pipeline Execution Summary ================\n";
    cout << "Elapsed Time     : " << fixed << setprecision(3) << diff.count() << " s\n";
    cout << "Throughput       : " << fixed << setprecision(2) << (total_gb / diff.count()) << " GB/s\n";
    cout << "IOPS             : " << fixed << setprecision(0) << iops << "\n";
    cout << "Completed Ops    : " << total_ops << "\n";
    cout << "Partial Retries  : " << total_retries << "\n";
    cout << "Total Errors     : " << total_errors << "\n";
    cout << "============================================================\n";

    if (fd >= 0) close(fd);
    if (dst_fd >= 0) close(dst_fd);

    return 0;
}

