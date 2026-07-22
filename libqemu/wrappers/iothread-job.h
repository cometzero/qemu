/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIBQEMU_WRAPPERS_IOTHREAD_JOB_H
#define LIBQEMU_WRAPPERS_IOTHREAD_JOB_H

typedef struct LibQemuIOThreadJob LibQemuIOThreadJob;
typedef void (*LibQemuIOThreadJobFn)(void *opaque);

LibQemuIOThreadJob *libqemu_iothread_job_new(LibQemuIOThreadJobFn fn,
                                              void *opaque);
bool libqemu_iothread_job_schedule(LibQemuIOThreadJob *job);
void libqemu_iothread_job_cancel(LibQemuIOThreadJob *job);
void libqemu_iothread_job_stop(LibQemuIOThreadJob *job);
void libqemu_iothread_job_drain(LibQemuIOThreadJob *job);
void libqemu_iothread_job_free(LibQemuIOThreadJob *job);

#endif
