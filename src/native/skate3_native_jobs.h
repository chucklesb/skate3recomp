#pragma once
// EA::Jobs worker wake-ups without the thundering herd (RE-7; battery and
// heat on handhelds).
//
// The job system (Skate 2 names, EA::Jobs::LocalBackend): each worker thread
// loops in sub_8290B750: RunSingleJob (sub_8290AFA0, backend vtable +0x60)
// scans the job slots and claims one; when none is runnable it waits on its
// own event (worker+0x2C) through sub_82EDFEC0 (WaitForSingleObject) with
// GetJobThreadSleepTimeoutMS (backend+1592; 1 ms in TU3). WakeThreads
// (sub_8290B320, vtable) SetEvents every active worker's event (32 slots,
// stride 48, event at backend+76) after work is added, unless backend+1597
// is set (then the caller runs the jobs itself).
//
// Measured (Windows plaza, 30 fps, skate3_native_job_stats): WakeThreads runs
// ~33,000 times a second, and only 31% of signalled wakes find a job; timed-out
// wakes find one ~1% of the time. Each wasted wake costs a host event signal,
// a futex wake and a slot scan (docs/re/hot-functions.md).
//
// skate3_native_job_wake = on: a worker is "armed" from just before each scan
// until a waker consumes it; WakeThreads signals only armed workers, once per
// arming. Ordering (seq_cst): the worker arms, then scans; the waker's job is
// published before it reads the arm flag. So either the scan sees the job or
// the waker sees the arm and signals. Workers this code has not seen are
// always signalled; the game's 1 ms timeout stays as a safety net
// (`late` counts timed-out wakes that then found a job).
// skate3_native_jobs_replace (default on): native bodies (RE-7 Body
// families j_*) for the worker loop, WakeThreads, the worker wait, the
// sleep-timeout getter/setter and the spin-wait sub_824741F0, plus the
// wait underneath: WaitForSingleObjectEx sub_82EE7880 (j_waitex) and its
// helpers sub_82EEEA98 (k_timeout: ms to a kernel timeout) and sub_82EE6260
// (k_lasterr: NTSTATUS to the thread's last error). RunSingleJob stays
// recompiled (docs/re/replacements.md).
namespace skate3::native_jobs {

// Frame tail: the stats line, cvar refresh.
void OnFrameBuilt();

}  // namespace skate3::native_jobs
