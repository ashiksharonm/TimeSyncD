#include "../include/poller.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#endif

struct poller {
    int use_epoll;
    /* epoll */
    int epfd;
    /* poll */
    struct pollfd *pfds;
    void **uds;
    int n, cap;
    int *slot_of_fd; /* fd -> index in pfds, -1 if absent */
    int slot_cap;
    int cursor;      /* poll backend: rotate the scan start so high fds are not starved */
};

poller_t *poller_create(const char *backend) {
    poller_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    int want_epoll = 1;
    if (backend && !strcmp(backend, "poll")) want_epoll = 0;
    else if (backend && strcmp(backend, "auto") && strcmp(backend, "epoll")) goto fail;
#ifdef HAVE_EPOLL
    if (want_epoll) {
        p->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (p->epfd < 0) goto fail;
        p->use_epoll = 1;
        return p;
    }
#else
    if (backend && !strcmp(backend, "epoll")) goto fail; /* explicitly requested, unavailable */
#endif
    (void)want_epoll;
    return p;
fail:
    free(p);
    return NULL;
}

const char *poller_name(const poller_t *p) { return p->use_epoll ? "epoll" : "poll"; }

#ifdef HAVE_EPOLL
static unsigned to_epoll(int ev) { return (ev & PE_READ ? EPOLLIN : 0u) | (ev & PE_WRITE ? EPOLLOUT : 0u); }
#endif

static int slot_reserve(poller_t *p, int fd) {
    if (fd < 0) return -1;
    if (fd >= p->slot_cap) {
        int ncap = p->slot_cap ? p->slot_cap : 1024;
        while (ncap <= fd) ncap *= 2;
        int *ns = realloc(p->slot_of_fd, (size_t)ncap * sizeof(int));
        if (!ns) return -1;
        for (int i = p->slot_cap; i < ncap; i++) ns[i] = -1;
        p->slot_of_fd = ns;
        p->slot_cap = ncap;
    }
    return 0;
}

int poller_add(poller_t *p, int fd, void *ud, int events) {
#ifdef HAVE_EPOLL
    if (p->use_epoll) {
        struct epoll_event e = {.events = to_epoll(events), .data.ptr = ud};
        return epoll_ctl(p->epfd, EPOLL_CTL_ADD, fd, &e);
    }
#endif
    if (slot_reserve(p, fd) < 0) return -1;
    if (p->n == p->cap) {
        int ncap = p->cap ? p->cap * 2 : 256;
        struct pollfd *nf = realloc(p->pfds, (size_t)ncap * sizeof(*nf));
        if (!nf) return -1;
        p->pfds = nf;
        void **nu = realloc(p->uds, (size_t)ncap * sizeof(*nu));
        if (!nu) return -1;
        p->uds = nu;
        p->cap = ncap;
    }
    p->pfds[p->n] = (struct pollfd){.fd = fd, .events = (short)((events & PE_READ ? POLLIN : 0) | (events & PE_WRITE ? POLLOUT : 0))};
    p->uds[p->n] = ud;
    p->slot_of_fd[fd] = p->n++;
    return 0;
}

int poller_set(poller_t *p, int fd, void *ud, int events) {
#ifdef HAVE_EPOLL
    if (p->use_epoll) {
        struct epoll_event e = {.events = to_epoll(events), .data.ptr = ud};
        return epoll_ctl(p->epfd, EPOLL_CTL_MOD, fd, &e);
    }
#endif
    if (fd < 0 || fd >= p->slot_cap || p->slot_of_fd[fd] < 0) return -1;
    p->pfds[p->slot_of_fd[fd]].events = (short)((events & PE_READ ? POLLIN : 0) | (events & PE_WRITE ? POLLOUT : 0));
    p->uds[p->slot_of_fd[fd]] = ud;
    return 0;
}

void poller_del(poller_t *p, int fd) {
#ifdef HAVE_EPOLL
    if (p->use_epoll) {
        epoll_ctl(p->epfd, EPOLL_CTL_DEL, fd, NULL);
        return;
    }
#endif
    if (fd < 0 || fd >= p->slot_cap || p->slot_of_fd[fd] < 0) return;
    int i = p->slot_of_fd[fd], last = --p->n;
    p->slot_of_fd[fd] = -1;
    if (i != last) {
        p->pfds[i] = p->pfds[last];
        p->uds[i] = p->uds[last];
        p->slot_of_fd[p->pfds[i].fd] = i;
    }
}

int poller_wait(poller_t *p, pevent_t *out, int max, int timeout_ms) {
#ifdef HAVE_EPOLL
    if (p->use_epoll) {
        struct epoll_event ev[256];
        int r = epoll_wait(p->epfd, ev, max < 256 ? max : 256, timeout_ms);
        if (r <= 0) return r;
        for (int i = 0; i < r; i++) {
            out[i].ud = ev[i].data.ptr;
            out[i].readable = (ev[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0;
            out[i].writable = (ev[i].events & EPOLLOUT) != 0;
            out[i].error = (ev[i].events & EPOLLERR) != 0;
        }
        return r;
    }
#endif
    int r = poll(p->pfds, (nfds_t)p->n, timeout_ms);
    if (r <= 0) return r;
    int k = 0;
    for (int j = 0; j < p->n && k < max; j++) {
        int i = (p->cursor + j) % p->n;
        short re = p->pfds[i].revents;
        if (!re) continue;
        out[k].ud = p->uds[i];
        out[k].readable = (re & (POLLIN | POLLHUP)) != 0;
        out[k].writable = (re & POLLOUT) != 0;
        out[k].error = (re & (POLLERR | POLLNVAL)) != 0;
        k++;
    }
    p->cursor = p->n ? (p->cursor + 1) % p->n : 0;
    return k;
}

void poller_destroy(poller_t *p) {
    if (!p) return;
    if (p->use_epoll) close(p->epfd);
    free(p->pfds);
    free(p->uds);
    free(p->slot_of_fd);
    free(p);
}
