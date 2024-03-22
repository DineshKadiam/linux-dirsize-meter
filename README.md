# dut - The fastest disk usage calculator for Linux

## Features
- Extremely fast. To my knowledge, this is the fastest disk usage calculator in existence.
See benchmarkes below.
- Pure ASCII display which is fully compatible with the plain Linux tty.

## Limitations
- Hard link counting is currently rather naive. Hard links get counted in the place where they're
first found, which can vary from run to run. If you see directories changing sizes but the grand
total is constant, this is probably why.

## How to build

`dut` is a single source file, so all you need is a C11 compiler:
```
gcc -O3 -flto main.c -o dut
```
To install, copy the `dut` executable to a directory on your PATH, e.g. `/usr/local/bin`.

## Benchmarks

Benchmarked programs:
* `du` from coreutils (C)
* [`pdu`](https://github.com/KSXGitHub/parallel-disk-usage) (Rust)
* [`dust`](https://github.com/bootandy/dust) (Rust)
* [`gdu`](https://github.com/dundee/gdu) (Go)

If you know of a faster program, let me know and I'll run my benchmarks again.

On my laptop with the entire filesystem layout loaded into Linux's cache. I added a 10 second sleep
to limit the effects of thermal throttling.
```
$ hyperfine 'dut -Cn 60 /' 'du -sh /' 'pdu /' 'dust -n 60 /' 'gdu --non-interactive /' -s 'sleep 10'
Benchmark 1: dut -Cn 60 /
  Time (mean ± σ):     462.6 ms ±   7.9 ms    [User: 1037.6 ms, System: 4356.7 ms]
  Range (min … max):   443.6 ms … 473.2 ms    10 runs

Benchmark 2: du -sh /
  Time (mean ± σ):      3.108 s ±  0.040 s    [User: 0.534 s, System: 2.529 s]
  Range (min … max):    3.052 s …  3.163 s    10 runs

Benchmark 3: pdu /
  Time (mean ± σ):     642.5 ms ±   8.3 ms    [User: 1694.3 ms, System: 5449.2 ms]
  Range (min … max):   629.3 ms … 658.7 ms    10 runs

Benchmark 4: dust -n 60 /
  Time (mean ± σ):      1.302 s ±  0.037 s    [User: 2.752 s, System: 6.279 s]
  Range (min … max):    1.275 s …  1.399 s    10 runs

Benchmark 5: gdu --non-interactive /
  Time (mean ± σ):      1.152 s ±  0.019 s    [User: 6.314 s, System: 6.029 s]
  Range (min … max):    1.132 s …  1.183 s    10 runs

Summary
  dut -Cn 60 / ran
    1.39 ± 0.03 times faster than pdu /
    2.49 ± 0.06 times faster than gdu --non-interactive /
    2.81 ± 0.09 times faster than dust -n 60 /
    6.72 ± 0.14 times faster than du -sh /
```

On my bloated home directory on an HDD. There are approximately 26k subdirectories.
```
$ echo 1 | sudo tee /proc/sys/vm/drop_caches
$ time dut .
...
 711G .
dut .  0.32s user 2.58s system 7% cpu 36.667 total

$ echo 1 | sudo tee /proc/sys/vm/drop_caches
$ time pdu .
...
765.0G ┌─┴.
pdu .  0.66s user 2.35s system 3% cpu 1:17.05 total

$ echo 1 | sudo tee /proc/sys/vm/drop_caches
$ time dust .
...
711G ┌─┴ .
dust .  1.06s user 3.47s system 4% cpu 1:35.88 total

$ echo 1 | sudo tee /proc/sys/vm/drop_caches
$ time du -sh .
712G	.
du -sh .  0.24s user 1.28s system 3% cpu 45.412 total

$ echo 1 | sudo tee /proc/sys/vm/drop_caches
$ time gdu --non-interactive .
...
gdu --non-interactive .  1.55s user 2.18s system 9% cpu 41.001 total
```

### Why are the Rust programs so bad on HDD?
It's hard to say. My best guess is they have a really HDD-unfriendly access pattern, since they
both use Rayon for multithreading which uses FIFO ordering for tasks. This results in them doing
a breadth-first search of the filesystem, whereas `dut` and `du` both use depth-first search. I
don't know why one ordering is better than the other, but the difference is pretty drastic.
