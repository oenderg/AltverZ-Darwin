#!/usr/bin/env python3
"""Exercise the host's actual estimate block without a Windows UI or game files."""

import os
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).parents[1] / "tools/pvz2-windows.c").read_text()
start = source.index("    double percent =", source.index("static void progress_tick("))
end = source.index("    wchar_t estimate[", start)
block = source[start:end]
formatter = source[source.index("static void progress_estimate_text("):
                   source.index("static void progress_tick(")]

# Compile the production block, rather than a second implementation that could
# pass while the host regresses. Only the UI clock and progress inputs are faked.
harness = r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
typedef unsigned long long ULONGLONG;
static unsigned long long g_progress_base_weight;
static ULONGLONG g_progress_base_time, g_progress_samples_start, g_progress_estimate_updated;
static int g_progress_have_base, g_progress_have_estimate;
static double g_progress_estimate, g_progress_bootstrap;
static void reset(void) {
    g_progress_base_weight = g_progress_base_time = 0;
    g_progress_have_base = g_progress_have_estimate = 0;
    g_progress_estimate = g_progress_bootstrap = 0;
    g_progress_samples_start = 1000;
    g_progress_estimate_updated = 0;
}
""" + formatter + r"""
static int step(double seconds, unsigned long long weight_done, double weight_rate) {
    unsigned long long weight_total = 1000000;
    ULONGLONG now = 1000 + (ULONGLONG)(seconds * 1000 + 0.5);
""" + block + r"""
    assert(isfinite(g_progress_estimate) && g_progress_estimate >= 0);
    return estimate_valid;
}
int main(void) {
    const int durations[] = {100, 120, 540};
    for (unsigned n = 0; n < sizeof durations / sizeof durations[0]; n++) {
        int duration = durations[n];
        double first = -1;
        reset();
        for (int i = 0; i <= duration * 10; i++) {
            double t = i / 10.0;
            unsigned long long weight = (unsigned long long)(1000000.0 * t / duration + 0.5);
            int valid = step(t, weight, 1000000.0 / duration);
            if (t < 5 || weight < 50000) assert(!valid);
            if (valid) {
                if (first < 0) first = t;
                assert(fabs(g_progress_estimate - (duration - t)) < 0.6);
            }
        }
        double gate = duration * 0.05 > 5 ? duration * 0.05 : 5;
        assert(fabs(first - gate) < 0.11);
        printf("linear %d s: first estimate %.1f s, correct remaining time within 0.6 s\n", duration, first);
    }
    reset();
    assert(!step(6, 49000, 10000));
    assert(step(7, 50000, 10000));
    for (int i = 80; i < 1000; i++) assert(step(i / 10.0, 50000, 0));
    assert(step(101, 600000, 100000));
    assert(step(110, 1000000, 100000));
    puts("gates, long stall, resume and completion: finite, nonnegative, estimate stays visible");
    reset();
    assert(step(5, 50000, 10000));
    assert(step(24.9, 59000, 10000));
    double before = g_progress_estimate;
    assert(step(25, 60000, 10000));
    assert(fabs(g_progress_estimate - before) < 10);
    wchar_t text[96];
    progress_estimate_text(text, 96, 0, 100);
    assert(!wcscmp(text, L"estimating..."));
    progress_estimate_text(text, 96, 1, 1801);
    assert(!wcscmp(text, L"estimating..."));
    progress_estimate_text(text, 96, 1, 0);
    assert(!wcscmp(text, L"less than a minute left"));
    puts("slow-unit handoff is smoothed; unknown and over-30-minute values are hidden");
}
"""

with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "progress.c"
    exe = Path(tmp) / "progress-test"
    c.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(c), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
