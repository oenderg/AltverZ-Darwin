# ScarletNX cold translation: a progress callback for the caller's window

Draft for an upstream change; not sent to ScarletNX yet.

## What

`snx_recompiler_set_progress(r, fn, user)` registers a callback:

```c
typedef void (*snx_progress_fn)(void* user, unsigned long long weight_done,
                                unsigned long long weight_total,
                                unsigned long long functions_done,
                                unsigned long long functions_total);
```

It is called once with the totals and no work done, after the code units are made, and then by the worker
thread that finished each unit. The weight is the unit's instructions plus four per basic-block leader, the same
weight the units are sized by, so a bar weighted by it follows the work and not the unit count. The callback runs
under the queue lock; it must be quick and thread-safe. The generated code and the module cache are unchanged.

Our Windows x64 driver owns the counters (C11 atomics) and the preparing window reads them from the UI thread.
Nothing in the recompiler knows about Windows.

## Why

PvZ2's first Windows start translates 100,308 functions (11.19M weighted instructions) into one module, which takes
about 536 s in the VM (289 s on the 24-thread PC). The host's window had a marquee bar and no numbers: it could not
say how far along the translation was or how long was left. The window now shows a percentage, a time estimate and
a log line every 5 s.

The percentage is the completed unit weight, capped at 100%: 0.0% at the start, 11.19M of 11.19M at the end of the
units. With 30,000-instruction units that is 0.3% per unit, small enough for a smooth bar. The first units are the
largest and the slowest per weight. The window now reveals its first estimate after at least 5 s and 5% of the
weight, bootstrapping from the recent completed-weight rate, then using the average rate since that mark and an
elapsed-time smoothing (nominally 80/20 per five seconds) once twenty seconds of post-mark samples exist. The first
estimate is already visible during that sample.
Early estimates can be conservative; a five-second log and the bounds keep that visible without promising hours.

Measured in the original VM cold run with this callback and the former 10% plus 20 s estimate rule: the window logged
1.5% at 18 s, 10.3% at 78 s, 42.4% at
250 s, 73.1% at 396 s, 100.0% at 517 s, and the game drew its first frame at 530 s. The estimate said about 5 minutes
at 41%, about 2 minutes at 71%, and less than a minute at 88%. The log's lines are in the task report.

## Upstream

The change is the struct fields, `snx_recompiler_set_progress`, the queue's totals and the callback in `worker()`,
with no API removal and no game files. A caller that does not set it pays nothing. Our profile logs
(`TL_X64_PROFILE`) are separate and can stay out of an upstream patch.
