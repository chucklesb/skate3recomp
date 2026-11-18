# EA::Jobs (worker threads, backend, job handles)

- **Item:** part of RE-7 ([all items](README.md#the-re-items))
- **Goal:** the job system as our own code: the worker loop, waking, waiting.
- **Done when:** the job system's functions are native and its guesses gone.
- **Status:** the worker loop, WakeThreads, the waits and the spin-wait run as native bodies; waking signals only workers that may wait (2026-11-18)
- **Next:** RunSingleJob's slot layout

The layouts (`JobBackend`, `JobWorker`, `JobHandle`) are in
[types.md](types.md#eajobs-worker-threads-backend-job-handles); the native
bodies in `src/native/skate3_native_jobs.cpp`; the measurements (no idle
spin, two thirds of wakes finding no job, the wake fix) in
[hot-functions.md](hot-functions.md).
