/* Unit tests for the production eventfd-backed message bus. */

#include "msgbus.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

int main(void)
{
    ocr_msgbus_t bus;
    CHECK(ocr_msgbus_init(NULL, 3) < 0);
    CHECK(ocr_msgbus_init(&bus, 0) < 0);
    CHECK(ocr_msgbus_init(&bus, 3) == 0);
    CHECK(ocr_msgbus_get_fd(&bus) >= 0);

    ocr_msg_t received;
    CHECK(ocr_msgbus_recv(&bus, &received, 0) == 1);

    struct pollfd pfd = {
        .fd = ocr_msgbus_get_fd(&bus),
        .events = POLLIN,
    };
    CHECK(poll(&pfd, 1, 0) == 0);

    for (unsigned int i = 0; i < 3; ++i) {
        uint32_t expected_type = (uint32_t)MSG_USER + i;
        ocr_msg_t message = {
            .type = expected_type,
            .seq = 100 + i,
            .param_i = -10 - (int)i,
            .timestamp = 1000 + i,
        };
        CHECK(ocr_msgbus_post(&bus, &message) == 0);
    }
    ocr_msg_t overflow = {.type = MSG_SHUTDOWN};
    CHECK(ocr_msgbus_post(&bus, &overflow) == 1);
    CHECK(poll(&pfd, 1, 0) == 1);
    CHECK((pfd.revents & POLLIN) != 0);

    for (unsigned int i = 0; i < 3; ++i) {
        uint32_t expected_type = (uint32_t)MSG_USER + i;
        CHECK(ocr_msgbus_recv(&bus, &received, 0) == 0);
        CHECK(received.type == expected_type);
        CHECK(received.seq == 100 + i);
        CHECK(received.param_i == -10 - (int)i);
        CHECK(received.timestamp == 1000 + i);
    }
    CHECK(ocr_msgbus_recv(&bus, &received, 10) == 1);
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 0) == 0);

    ocr_msgbus_destroy(&bus);
    CHECK(ocr_msgbus_get_fd(&bus) == -1);
    puts("[PASS] production message bus tests");
    return EXIT_SUCCESS;
}
