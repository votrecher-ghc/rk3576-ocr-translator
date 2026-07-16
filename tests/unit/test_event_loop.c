/* Unit tests for the production epoll event loop. */

#include "event_loop.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

typedef struct {
    ocr_event_loop_t *loop;
    atomic_int callbacks;
    int run_result;
} loop_fixture_t;

static void event_callback(int fd, uint32_t events, void *user)
{
    loop_fixture_t *fixture = (loop_fixture_t *)user;
    uint64_t value = 0;
    ssize_t count;
    CHECK((events & EPOLLIN) != 0);
    do {
        count = read(fd, &value, sizeof(value));
    } while (count < 0 && errno == EINTR);
    CHECK(count == (ssize_t)sizeof(value));
    CHECK(value == 1);
    atomic_fetch_add_explicit(&fixture->callbacks, 1, memory_order_relaxed);
    ocr_event_loop_stop(fixture->loop);
}

static void *run_loop(void *arg)
{
    loop_fixture_t *fixture = (loop_fixture_t *)arg;
    fixture->run_result = ocr_event_loop_run(fixture->loop);
    return NULL;
}

static void wait_until_running(ocr_event_loop_t *loop)
{
    const struct timespec delay = {
        .tv_nsec = 1000000,
    };
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (atomic_load_explicit(&loop->running, memory_order_acquire)) return;
        (void)nanosleep(&delay, NULL);
    }
    CHECK(0 && "event loop did not enter running state");
}

int main(void)
{
    ocr_event_loop_t loop;
    CHECK(ocr_event_loop_init(NULL, 4) < 0);
    CHECK(ocr_event_loop_init(&loop, 4) == 0);

    int signal_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(signal_fd >= 0);

    loop_fixture_t fixture = {
        .loop = &loop,
        .run_result = -99,
    };
    atomic_init(&fixture.callbacks, 0);

    ocr_event_t event = {
        .fd = signal_fd,
        .events = EPOLLIN,
        .cb = event_callback,
        .user = &fixture,
    };
    CHECK(ocr_event_loop_add(&loop, &event) == 0);
    CHECK(ocr_event_loop_add(&loop, &event) < 0);
    event.events = EPOLLIN | EPOLLET;
    CHECK(ocr_event_loop_mod(&loop, &event) == 0);

    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, run_loop, &fixture) == 0);
    wait_until_running(&loop);
    uint64_t value = 1;
    CHECK(write(signal_fd, &value, sizeof(value)) == (ssize_t)sizeof(value));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(fixture.run_result == 0);
    CHECK(atomic_load_explicit(&fixture.callbacks, memory_order_relaxed) == 1);

    CHECK(ocr_event_loop_del(&loop, signal_fd) == 0);
    CHECK(ocr_event_loop_del(&loop, signal_fd) < 0);

    fixture.run_result = -99;
    CHECK(pthread_create(&thread, NULL, run_loop, &fixture) == 0);
    wait_until_running(&loop);
    ocr_event_loop_stop(&loop);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(fixture.run_result == 0);

    close(signal_fd);
    ocr_event_loop_destroy(&loop);
    puts("[PASS] production event loop tests");
    return EXIT_SUCCESS;
}
