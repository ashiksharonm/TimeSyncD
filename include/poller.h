#ifndef POLLER_H
#define POLLER_H

/*
 * Readiness notification behind one interface: epoll on Linux, poll() everywhere
 * (and as a selectable fallback).  Both are level-triggered, so an event that is
 * not fully drained is simply reported again on the next wait.
 */
#define PE_READ 1
#define PE_WRITE 2

typedef struct {
    void *ud;      /* pointer given to poller_add */
    int readable;  /* includes hang-up, so the owner discovers EOF with recv() */
    int writable;
    int error;     /* POLLERR / EPOLLERR / invalid fd */
} pevent_t;

typedef struct poller poller_t;

/* backend: "auto" (epoll if available, else poll), "epoll" or "poll". NULL on failure. */
poller_t *poller_create(const char *backend);
const char *poller_name(const poller_t *p);
int poller_add(poller_t *p, int fd, void *ud, int events);
int poller_set(poller_t *p, int fd, void *ud, int events); /* change interest */
void poller_del(poller_t *p, int fd);
/* Returns the number of events stored in out (0 on timeout), -1 on error (errno set). */
int poller_wait(poller_t *p, pevent_t *out, int max, int timeout_ms);
void poller_destroy(poller_t *p);

#endif
