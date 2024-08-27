#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "RbQueue.h"

#define QUEUE_INITIAL_SIZE 8

static void panic(char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/*
 * Allocate and initialize a new queue.
 */
struct rbq *rbq_create(void) {
    struct rbq *q = malloc(sizeof(*q));
    if (!q)
        panic("Could not create queue: out of memory.");

    rbq_init(q);

    return q;
}

void rbq_init(struct rbq *q) {
    q->head = 0;
    q->tail = 0;
    q->data = calloc(QUEUE_INITIAL_SIZE, sizeof *(q->data));
    q->capacity = QUEUE_INITIAL_SIZE;
}

/*
 * Free a queue's data as well as the queue itself.
 */
void rbq_destroy(struct rbq *q) {
    rbq_exit(q);

    free(q);
}

void rbq_exit(struct rbq *q) {
    if (q)
        free(q->data);
}

/*
 * rbq_used() -
 *     Number of used slots in the queue.
 */
size_t rbq_used(struct rbq *q) {
    if (q->tail >= q->head)
        return q->tail - q->head;

    return q->tail + q->capacity - q->head;
}

/*
 * rbq_avail() -
 *     Number of available slots in the queue.
 *     This is one less than capacity - used because the last empty slot
 *     is used to signal that the queue is full.
 */
size_t rbq_avail(struct rbq *q) {
    /* usable capacity is always 1 less than allocated space */
    return q->capacity - rbq_used(q) - 1;
}

/*
 * Grow a queue, moving items if necessary to keep them contiguous after.
 */
static void rbq_grow(struct rbq *q) {
    size_t new_capacity;
    if (__builtin_mul_overflow(q->capacity, 2, &new_capacity))
        panic("Queue would overflow. Aborting.");

    void *new_data = realloc(q->data, new_capacity * sizeof *(q->data));
    if (!new_data)
        panic("Could not allocate memory for queue.");

    q->data = new_data;

    /* Move items: */
    if (q->tail >= q->head) {
        /* Nothing to do, used slots are already all contiguous. */
    } else {
        // TODO: make this check if copying head or tail would use less mem.

        /* q->tail is the number of items to copy from start of array, might be 0. */
        memcpy(q->data + q->capacity, q->data, q->tail * sizeof *(q->data));
        /* Update q->tail to point to appropriate distance into new array. */
        q->tail = q->capacity + q->tail;
    }

    q->capacity = new_capacity;

}

void rbq_push(struct rbq *q, void *data) {
    size_t old_tail = q->tail;  /* First free slot. */

    size_t new_tail = (q->tail + 1) % q->capacity;

    /* Queue is full when there is one free slot before the head. */
    if (new_tail == q->head) {
        rbq_grow(q);
        new_tail = (q->tail + 1) % q->capacity;
        old_tail = q->tail;
    }

    q->data[old_tail] = data;

    q->tail = new_tail;
}

void *rbq_pop(struct rbq *q) {
    /* Queue empty: */
    if (q->head == q->tail) {
        return NULL;
    }

    void *data = q->data[q->head];
    if (!data)
        panic("Tried to pop an empty slot.");

    q->data[q->head] = NULL;

    size_t new_head = (q->head + 1) % q->capacity;
    q->head = new_head;
    return data;
}

/*
 * Drop n items from the queue without handling them.
 * Panics if the queue has fewer items than that.
 */
static void rbq_drop_n(struct rbq *q, size_t n) {
    if (n > rbq_used(q))
        panic("trying to drop too many items!");

    q->head = (q->head + n) % q->capacity;
}

/*
 * Copy n items from src into the queue dst.
 * Panics if there is not enough room in dst.
 */
static void rbq_push_n(struct rbq *dst, void *src, size_t n) {
    if (n > rbq_avail(dst))
        panic("trying to copy too many items into queue!");

    if (dst->tail + n > dst->capacity) {
        /* Not enough room, need to wrap around: */
        size_t first = dst->capacity - dst->tail;
        memcpy(&dst->data[dst->tail], src, first * sizeof *(dst->data));
        size_t rest = n - first;
        memcpy(&dst->data[0], src + (first * sizeof *(dst->data)), rest * sizeof *(dst->data));
    } else {
        memcpy(&dst->data[dst->tail], src, n * sizeof *(dst->data));
    }

    dst->tail = (dst->tail + n) % dst->capacity;
}

/*
 * Append n items in queue src to queue dst and remove them from src.
 */
void rbq_append_n(struct rbq *dst, struct rbq *src, size_t n) {
    while (rbq_avail(dst) < n)
        rbq_grow(dst);

    size_t tmp_tail = (src->head + n) % src->capacity;

    /* Source queue range is a single contiguous chunk: */
    if (tmp_tail >= src->head) {
        size_t n = tmp_tail - src->head;
        rbq_push_n(dst, &src->data[src->head], n);
        
    /* Source queue range wraps around: */
    } else {
        size_t n = src->capacity - src->head;
        rbq_push_n(dst, &src->data[src->head], n);

        n = tmp_tail;
        rbq_push_n(dst, &src->data[0], n);
    }

    rbq_drop_n(src, n);
}

/*
 * Append all items in queue src to queue dst and remove them from src.
 */
void rbq_append(struct rbq *dst, struct rbq *src) {
    rbq_append_n(dst, src, rbq_used(src));
}
