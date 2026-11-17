#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Time and memory of stack_usage_analyzer builds on compile databases, under Linux.

Usage: measure.py --out DIR --reps N --jobs 1,2,4 --deadline-min M
                  --corpus NAME=compile_commands.json ... --variant NAME=analyzer ...

Each run starts in a fresh directory (empty caches) and gives one row of DIR/runs.csv:
  real_s          wall time of the process
  maxrss_mb       peak resident set size, from wait4()
  load_s          wall time of loading: app.shared_loading.total less the phases after loading
  anon_end_mb     RssAnon at the end of loading (first sample of /proc/<pid>/status after it)
  anon_load_mb    highest RssAnon sampled during loading
  threads_load    highest number of threads sampled during loading
  digest          of the report, without the order of its lists or the ids of its diagnostics
The end of loading is the time of the first "Compiling" line plus load_s. /proc is sampled every
10 ms. The variants rotate from one repetition to the next. Stops before a run once the deadline
has passed, and at the first failed run.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

HOTSPOT = re.compile(rb"^\s+\d+\. (\S+) total_ms=([\d.]+) ")
FIELDS = ["corpus", "jobs", "variant", "rep", "exit", "real_s", "user_s", "sys_s", "maxrss_mb",
          "load_s", "after_load_s", "compile_cum_s", "anon_end_mb", "anon_load_mb", "threads_load",
          "threads_drop_s", "load_end_s", "diagnostics", "functions", "digest"]


def digest(path):
    report = json.load(open(path))
    diagnostics = sorted(json.dumps({k: v for k, v in d.items() if k != "id"}, sort_keys=True)
                         for d in report["diagnostics"])
    functions = sorted(json.dumps(f, sort_keys=True) for f in report["functions"])
    payload = json.dumps([diagnostics, functions, report.get("diagnosticsSummary"),
                          sorted(report["meta"]["inputFiles"])])
    return (hashlib.sha256(payload.encode()).hexdigest()[:12], len(report["diagnostics"]),
            len(report["functions"]))


def run_once(analyzer, db, directory, jobs):
    shutil.rmtree(directory, ignore_errors=True)
    os.makedirs(os.path.join(directory, "tmp"))
    env = dict(os.environ, TMPDIR=os.path.join(directory, "tmp") + "/", CTRACE_HOTSPOT_TOP="200")
    samples = []  # (seconds since the start, RssAnon in kB, threads)
    with open(os.path.join(directory, "report.json"), "wb") as report:
        start = time.monotonic()
        process = subprocess.Popen(
            [analyzer, "--compile-commands=" + db, "--format=json", "--timing", "--jobs=%d" % jobs],
            cwd=directory, env=env, stdout=report, stderr=subprocess.PIPE)

    def sample():
        status = "/proc/%d/status" % process.pid
        while True:
            try:
                text = open(status).read()
            except OSError:
                return
            now = time.monotonic() - start
            anon = re.search(r"^RssAnon:\s+(\d+)", text, re.M)
            threads = re.search(r"^Threads:\s+(\d+)", text, re.M)
            if not anon:  # a zombie: the process has ended
                return
            samples.append((now, int(anon.group(1)), int(threads.group(1))))
            time.sleep(0.01)

    sampler = threading.Thread(target=sample)
    sampler.start()
    first_compiling = None
    hotspots = {}
    with open(os.path.join(directory, "timing.txt"), "wb") as log:
        for line in process.stderr:
            if first_compiling is None and b" Compiling " in line and b"InputPipeline" in line:
                first_compiling = time.monotonic() - start
            match = HOTSPOT.match(line)
            if match:
                hotspots[match.group(1).decode()] = float(match.group(2)) / 1000
            log.write(line)
    _, status, usage = os.wait4(process.pid, 0)
    real = time.monotonic() - start
    process.returncode = os.waitstatus_to_exitcode(status)
    sampler.join()

    if process.returncode == 0 and (first_compiling is None
                                    or "app.shared_loading.total" not in hotspots):
        sys.exit("no loading marker in %s/timing.txt: the end of loading is unknown" % directory)
    total = hotspots.get("app.shared_loading.total", 0.0)
    # The phases after loading run one after the other on the main thread.
    after = sum(seconds for name, seconds in hotspots.items()
                if name.startswith("app.shared_loading.")
                and name not in ("app.shared_loading.total", "app.shared_loading.load_module"))
    load = total - after
    load_end = (first_compiling or 0.0) + load
    during = [s for s in samples if s[0] <= load_end] or samples[:1]
    at_end = next((s for s in samples if s[0] >= load_end), samples[-1])
    threads_load = max(s[2] for s in during)
    # With loading workers: when the process is back to one thread, as a check of load_end.
    drop = next((s[0] for i, s in enumerate(samples)
                 if s[2] == 1 and i and samples[i - 1][2] > 1), 0.0)
    row = dict(exit=process.returncode, real_s="%.2f" % real, user_s="%.2f" % usage.ru_utime,
               sys_s="%.2f" % usage.ru_stime, maxrss_mb="%.0f" % (usage.ru_maxrss / 1024),
               load_s="%.2f" % load, after_load_s="%.2f" % after,
               compile_cum_s="%.2f" % (hotspots.get("input.compiler.invoke", 0.0)
                                       + hotspots.get("input.compiler.invoke.cwd", 0.0)),
               anon_end_mb="%.0f" % (at_end[1] / 1024),
               anon_load_mb="%.0f" % (max(s[1] for s in during) / 1024),
               threads_load=threads_load, threads_drop_s="%.2f" % drop,
               load_end_s="%.2f" % load_end)
    if process.returncode != 0 or os.path.getsize(os.path.join(directory, "report.json")) == 0:
        sys.exit("run failed (%s, exit %s): see %s/timing.txt" % (
            analyzer, process.returncode, directory))
    row["digest"], row["diagnostics"], row["functions"] = digest(
        os.path.join(directory, "report.json"))
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--reps", type=int, required=True)
    parser.add_argument("--jobs", default="1,2,4")
    parser.add_argument("--deadline-min", type=float, required=True)
    parser.add_argument("--corpus", action="append", required=True)
    parser.add_argument("--variant", action="append", required=True)
    parser.add_argument("--scratch", default="/work/runs")
    args = parser.parse_args()

    began = time.monotonic()
    corpora = [c.split("=", 1) for c in args.corpus]
    variants = [v.split("=", 1) for v in args.variant]
    jobs_list = [int(j) for j in args.jobs.split(",")]
    os.makedirs(args.out, exist_ok=True)
    csv_path = os.path.join(args.out, "runs.csv")
    new_file = not os.path.exists(csv_path)
    rows = []
    with open(csv_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        if new_file:
            writer.writeheader()
        for rep in range(args.reps):
            order = variants[rep % len(variants):] + variants[:rep % len(variants)]
            for corpus, db in corpora:
                for jobs in jobs_list:
                    for name, analyzer in order:
                        if time.monotonic() - began > args.deadline_min * 60:
                            print("DEADLINE passed: stopped before %s j%d %s r%d" % (
                                corpus, jobs, name, rep), flush=True)
                            return summarize(rows)
                        directory = os.path.join(args.scratch, "%s-j%d-%s-r%d" % (
                            corpus, jobs, name, rep))
                        row = run_once(analyzer, db, directory, jobs)
                        row.update(corpus=corpus, jobs=jobs, variant=name, rep=rep)
                        writer.writerow(row)
                        f.flush()
                        rows.append(row)
                        # Keep the report and the log of the first repetition only.
                        kept = os.path.join(args.out, "runs", os.path.basename(directory))
                        if rep == 0:
                            os.makedirs(kept, exist_ok=True)
                            for name_ in ("report.json", "timing.txt"):
                                shutil.copy(os.path.join(directory, name_), kept)
                        shutil.rmtree(directory, ignore_errors=True)
                        print("%s j%d %s r%d: real %ss load %ss rss %sMB anon_end %sMB "
                              "anon_load %sMB threads %s drop %ss/%ss %s (%s diagnostics)" % (
                                  corpus, jobs, name, rep, row["real_s"], row["load_s"],
                                  row["maxrss_mb"], row["anon_end_mb"], row["anon_load_mb"],
                                  row["threads_load"], row["threads_drop_s"], row["load_end_s"],
                                  row["digest"], row["diagnostics"]), flush=True)
    summarize(rows)


def summarize(rows):
    print("\nmedians (min..max of real_s):")
    cells = {}
    for row in rows:
        cells.setdefault((row["corpus"], row["jobs"], row["variant"]), []).append(row)
    for (corpus, jobs, variant), runs in sorted(cells.items()):
        def median(field):
            return statistics.median(float(run[field]) for run in runs)
        reals = [float(run["real_s"]) for run in runs]
        print("%-9s j%d %-13s n=%d real %6.2f (%.2f..%.2f) load %6.2f rss %5.0f anon_end %5.0f "
              "anon_load %5.0f threads %d" % (
                  corpus, jobs, variant, len(runs), median("real_s"), min(reals), max(reals),
                  median("load_s"), median("maxrss_mb"), median("anon_end_mb"),
                  median("anon_load_mb"), max(int(run["threads_load"]) for run in runs)))
    for corpus in sorted({row["corpus"] for row in rows}):
        digests = {row["digest"] for row in rows if row["corpus"] == corpus}
        print("%s: %s" % (corpus, "one report for every run: " + next(iter(digests))
                          if len(digests) == 1 else "REPORTS DIFFER: %s" % sorted(digests)),
              flush=True)


if __name__ == "__main__":
    main()
