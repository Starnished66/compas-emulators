#include "present.h"

#include <stdlib.h>
#include <time.h>

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static void * presenter_main(void * arg) {
    present_t * p = arg;
    pthread_mutex_lock(&p->lock);
    for (;;) {
        while (!p->pending && !p->stop) pthread_cond_wait(&p->work, &p->lock);
        if (!p->pending) break; /* stop, and nothing left to show */
        int t = p->show;
        p->show = p->wait;
        p->wait = t;
        int rows = p->wait_rows;
        p->pending = false;
        p->busy = true;
        pthread_mutex_unlock(&p->lock);
        uint64_t t0 = mono_ns();
        p->show_fn(p->ctx, p->buf[p->show], rows);
        uint64_t dt = mono_ns() - t0;
        pthread_mutex_lock(&p->lock);
        p->busy = false;
        p->shown++;
        p->show_ns_total += dt;
        if (dt > p->show_ns_max) p->show_ns_max = dt;
        pthread_cond_broadcast(&p->idle);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

bool present_start(present_t * p, size_t buf_bytes, present_show_fn fn, void * ctx) {
    *p = (present_t) { .draw = 0, .wait = 1, .show = 2, .show_fn = fn, .ctx = ctx };
    for (int i = 0; i < 3; i++) {
        p->buf[i] = calloc(1, buf_bytes);
        if (!p->buf[i]) {
            for (int j = 0; j < i; j++) free(p->buf[j]);
            return false;
        }
    }
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->work, NULL);
    pthread_cond_init(&p->idle, NULL);
    if (pthread_create(&p->thread, NULL, presenter_main, p) != 0) {
        for (int i = 0; i < 3; i++) free(p->buf[i]);
        return false;
    }
    p->started = true;
    return true;
}

uint16_t * present_buffer(const present_t * p) {
    return p->buf[p->draw];
}

void present_submit(present_t * p, int rows) {
    pthread_mutex_lock(&p->lock);
    p->submitted++;
    if (p->pending) p->dropped++; /* the waiting frame was never shown: this one replaces it */
    int t = p->wait;
    p->wait = p->draw;
    p->draw = t;
    p->wait_rows = rows;
    p->pending = true;
    pthread_cond_signal(&p->work);
    pthread_mutex_unlock(&p->lock);
}

void present_claim(present_t * p) {
    pthread_mutex_lock(&p->lock);
    p->pending = false;
    while (p->busy) pthread_cond_wait(&p->idle, &p->lock);
}

void present_release(present_t * p) {
    pthread_mutex_unlock(&p->lock);
}

void present_stop(present_t * p) {
    if (!p->started) return;
    pthread_mutex_lock(&p->lock);
    p->stop = true;
    pthread_cond_signal(&p->work);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    p->started = false;
    for (int i = 0; i < 3; i++) {
        free(p->buf[i]);
        p->buf[i] = NULL;
    }
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->work);
    pthread_cond_destroy(&p->idle);
}
