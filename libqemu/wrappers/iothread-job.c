/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "iothread-job.h"

struct LibQemuIOThreadJob {
    QemuMutex mutex;
    QemuCond idle;
    QEMUBH *bh;
    LibQemuIOThreadJobFn fn;
    void *opaque;
    bool pending;
    bool running;
    bool stopping;
};

static void libqemu_iothread_job_run(void *opaque)
{
    LibQemuIOThreadJob *job = opaque;

    qemu_mutex_lock(&job->mutex);
    if (job->stopping || !job->pending) {
        qemu_cond_broadcast(&job->idle);
        qemu_mutex_unlock(&job->mutex);
        return;
    }
    job->pending = false;
    job->running = true;
    qemu_mutex_unlock(&job->mutex);

    job->fn(job->opaque);

    qemu_mutex_lock(&job->mutex);
    job->running = false;
    if (job->pending && !job->stopping) {
        qemu_bh_schedule(job->bh);
    }
    qemu_cond_broadcast(&job->idle);
    qemu_mutex_unlock(&job->mutex);
}

LibQemuIOThreadJob *libqemu_iothread_job_new(LibQemuIOThreadJobFn fn,
                                              void *opaque)
{
    LibQemuIOThreadJob *job;

    if (!fn) {
        return NULL;
    }
    job = g_new0(LibQemuIOThreadJob, 1);
    qemu_mutex_init(&job->mutex);
    qemu_cond_init(&job->idle);
    job->fn = fn;
    job->opaque = opaque;
    job->bh = qemu_bh_new(libqemu_iothread_job_run, job);
    return job;
}

bool libqemu_iothread_job_schedule(LibQemuIOThreadJob *job)
{
    bool accepted;

    qemu_mutex_lock(&job->mutex);
    accepted = !job->stopping;
    if (accepted) {
        job->pending = true;
        qemu_bh_schedule(job->bh);
    }
    qemu_mutex_unlock(&job->mutex);
    return accepted;
}

void libqemu_iothread_job_cancel(LibQemuIOThreadJob *job)
{
    qemu_mutex_lock(&job->mutex);
    job->pending = false;
    qemu_bh_cancel(job->bh);
    qemu_cond_broadcast(&job->idle);
    qemu_mutex_unlock(&job->mutex);
}

void libqemu_iothread_job_stop(LibQemuIOThreadJob *job)
{
    qemu_mutex_lock(&job->mutex);
    job->stopping = true;
    job->pending = false;
    qemu_bh_cancel(job->bh);
    qemu_cond_broadcast(&job->idle);
    qemu_mutex_unlock(&job->mutex);
}

void libqemu_iothread_job_drain(LibQemuIOThreadJob *job)
{
    qemu_mutex_lock(&job->mutex);
    while (job->pending || job->running) {
        qemu_cond_wait(&job->idle, &job->mutex);
    }
    qemu_mutex_unlock(&job->mutex);
}

void libqemu_iothread_job_free(LibQemuIOThreadJob *job)
{
    libqemu_iothread_job_stop(job);

    qemu_mutex_lock(&job->mutex);
    while (job->running) {
        qemu_cond_wait(&job->idle, &job->mutex);
    }
    qemu_mutex_unlock(&job->mutex);

    qemu_bh_delete(job->bh);
    qemu_cond_destroy(&job->idle);
    qemu_mutex_destroy(&job->mutex);
    g_free(job);
}
