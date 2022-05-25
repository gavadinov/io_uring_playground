#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <stdatomic.h>

// Link against a local copy of the latest version of io_uring to get it to compile under WSL
#if defined __has_include && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#else
#include "include/io_uring.h"
#endif

#include "syscall.c"

#define QUEUE_LEN 128

#define store_release(p, v) \
    atomic_store_explicit((_Atomic __typeof__(*(p)) *)(p), (v), __ATOMIC_RELEASE)

#define load_acquire(p) \
    atomic_load_explicit((_Atomic __typeof__(*(p)) *)(p), __ATOMIC_ACQUIRE)

enum
{
    EVENT_ACCEPT
};

struct event
{
    __u8 type;
};

struct event *event_new(__u8 type)
{
    struct event *event;
    event = malloc(sizeof(*event));
    if (!event)
    {
        perror("malloc");
        exit(1);
    }
    memset(event, 0, sizeof(*event));

    event->type = type;

    return event;
}

// Submission Queue
struct sq
{
    // Has to be synchronised
    unsigned *khead;
    unsigned *ktail;

    unsigned *mask;
    unsigned *array;
    unsigned *num_entries;
    struct io_uring_sqe *sq_entries;
    void *ring_ptr;

    // Internal tracking of the queue. Has to call uring_sq_submit() to become visible to the kernel
    unsigned inner_head;
    unsigned inner_tail;
};

// Completion Queue
struct cq
{
    // Has to be synchronised
    unsigned *khead;
    unsigned *ktail;

    unsigned *mask;
    struct io_uring_cqe *cq_entries;
    void *ring_ptr;
};

struct uring
{
    int ring_fd;
    struct sq sq;
    struct cq cq;
};

struct uring *uring_new()
{
    struct uring *uring;
    uring = malloc(sizeof(*uring));
    if (!uring)
    {
        perror("malloc");
        exit(1);
    }
    memset(uring, 0, sizeof(*uring));
}

int uring_init(struct uring *uring, struct io_uring_params params)
{
    struct sq *sq = &uring->sq;
    struct cq *cq = &uring->cq;

    uring->ring_fd = __sys_io_uring_setup(QUEUE_LEN, &params);
    if (uring->ring_fd < 0)
    {
        perror("__sys_io_uring_setup");
        return 1;
    }

    int sq_size = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    int cq_size = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);

    if (params.features & IORING_FEAT_SINGLE_MMAP)
    {
        if (cq_size > sq_size)
        {
            sq_size = cq_size;
        }
        cq_size = sq_size;
    }

    sq->ring_ptr = mmap(0,
                        sq_size,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        uring->ring_fd,
                        IORING_OFF_SQ_RING);
    if (sq->ring_ptr == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    if (params.features & IORING_FEAT_SINGLE_MMAP)
    {
        cq->ring_ptr = sq->ring_ptr;
    }
    else
    {
        cq->ring_ptr = mmap(0,
                            cq_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE,
                            uring->ring_fd,
                            IORING_OFF_CQ_RING);
        if (cq->ring_ptr == MAP_FAILED)
        {
            perror("mmap");
            return 1;
        }
    }

    sq->sq_entries = mmap(0,
                          params.sq_entries * sizeof(struct io_uring_sqe),
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE,
                          uring->ring_fd,
                          IORING_OFF_SQES);
    if (sq->sq_entries == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    sq->khead = sq->ring_ptr + params.sq_off.head;
    sq->ktail = sq->ring_ptr + params.sq_off.tail;
    sq->mask = sq->ring_ptr + params.sq_off.ring_mask;
    sq->array = sq->ring_ptr + params.sq_off.array;
    sq->num_entries = sq->ring_ptr + params.sq_off.ring_entries;

    cq->khead = cq->ring_ptr + params.cq_off.head;
    cq->ktail = cq->ring_ptr + params.cq_off.tail;
    cq->mask = cq->ring_ptr + params.cq_off.ring_mask;
    cq->cq_entries = cq->ring_ptr + params.cq_off.cqes;

    return 0;
}

// Returns a vacant SQE or NULL if the queue is full
struct io_uring_sqe *uring_next_sqe(struct uring *uring)
{
    struct sq *sq = &uring->sq;
    unsigned head = load_acquire(sq->khead);
    struct io_uring_sqe *sqe = NULL;
    unsigned next = sq->inner_tail + 1;

    if (next - head <= *sq->num_entries)
    {
        sqe = &sq->sq_entries[sq->inner_tail & *sq->mask];
        sq->inner_tail = next;
        // TODO: Can we skip this :?
        memset(sqe, 0, sizeof(*sqe));
    }

    return sqe;
}

// Sync the internal state with the kernel state
void uring_sq_submit(struct uring *uring)
{
    struct sq *sq = &uring->sq;
    unsigned pending = sq->inner_tail - sq->inner_head;
    // Safe to read because we are the only writer
    unsigned ktail = *sq->ktail;
    unsigned mask = *sq->mask;

    if (pending > 0)
    {
        while (pending)
        {
            sq->array[ktail & mask] = sq->inner_head & mask;
            ktail++;
            sq->inner_head++;
            pending--;
        }
    }

    // Make the changes visible to the kernel
    store_release(sq->ktail, ktail);

    // Notify the Kernel
    unsigned to_submit = ktail - *sq->khead;
    __sys_io_uring_enter(uring->ring_fd, to_submit, 0, 0);
}

void sqe_set_event(struct io_uring_sqe *sqe, struct event *event)
{
    sqe->user_data = (unsigned long)event;
}

void sqe_accept(struct io_uring_sqe *sqe, struct uring *uring, int sock)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // TODO - check if we got an SQE

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = sock;
    sqe->addr = (unsigned long)&client_addr;
    sqe->off = (__u64)client_addr_len;
}

void submit_accept(struct uring *uring, int sock)
{
    struct event *event = event_new(EVENT_ACCEPT);
    struct io_uring_sqe *sqe = uring_next_sqe(uring);
    sqe_accept(sqe, uring, sock);
    sqe_set_event(sqe, event);
    uring_sq_submit(uring);
}

void event_loop(struct uring *uring, int sock)
{
    submit_accept(uring, sock);
    while (1)
    {
        // TODO: poll cqe
    }
}

int socket_init(int port)
{
    int sock;
    struct sockaddr_in srv_addr;
    int enable = 1;

    sock = socket(PF_INET, SOCK_STREAM, 0);
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    listen(sock, 10);

    return sock;
}

int main(int argc, char *argv[])
{
    struct io_uring_params params;
    int sock;
    struct uring *uring = uring_new();

    memset(&params, 0, sizeof(params));

    uring_init(uring, params);

    sock = socket_init(8080);

    event_loop(uring, sock);
}
