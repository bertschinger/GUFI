/*
 * A growable ring buffer that is used to implement a queue interface.
 *
 * Data consists of pointers which MUST be non-NULL.
 *
 * XXX: maybe change the rbq_pop() signature to:
 *     int rbq_pop(struct rbq *q, void **data);
 * to allow storing NULLs?
 *
 * XXX caller locking or callee locking?
 */

struct rbq {
    /* head points at earliest used slot */
    size_t head;

    /* tail points at next free slot */
    size_t tail;

    size_t capacity;
    void **data;
};

struct rbq *rbq_create(void);
void rbq_init(struct rbq *q);
void rbq_destroy(struct rbq *q);
void rbq_exit(struct rbq *q);

void rbq_push(struct rbq *q, void * data);
void *rbq_pop(struct rbq *q);

size_t rbq_avail(struct rbq *q);
size_t rbq_used(struct rbq *q);

// XXX: are these APIs necessary?
void rbq_replace_n(struct rbq *dst, struct rbq *src, size_t n);
void rbq_replace(struct rbq *dst, struct rbq *src);

void rbq_append_n(struct rbq *dst, struct rbq *src, size_t n);
void rbq_append(struct rbq *dst, struct rbq *src);
