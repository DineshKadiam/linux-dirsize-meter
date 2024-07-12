# dut - A disk usage calculator for Linux

## Features
- Accurate counting of hard links with an output inspired by NCDU.
- ASCII-art disk usage tree that is fully compatible with the Linux tty.
- Configurable output format. Changing the maximum depth of files shown is a simple command-line
argument.

## Examples

By default, `dut` will output a tree of the biggest directories it finds under your current directory.

```
$ dut -n 10
 2.4G    0B       /- Local
 2.4G    0B     /- AppData
 2.4G    0B   /- NetworkService
 2.4G    0B |- ServiceProfiles
 2.5G   63M |- servicing
 5.2G  423M |   /- FileRepository
 5.2G  426M | /- DriverStore
 9.6G  2.5G |- System32
  12G  7.2G /- WinSxS
  29G  225M .
```

The `-n 10` option limits it to 10 rows. To limit the depth shown, use `-d <n>`.

```
$ dut -n 10 -d 1
 964M    0B |- MEMORY.DMP
1010M    0B |- SoftwareDistribution
 1.2G  1.0G |- SysWOW64
 1.3G  208M |- assembly
 1.8G  1.8G |- SystemApps
 2.4G    0B |- ServiceProfiles
 2.5G   63M |- servicing
 9.6G  2.5G |- System32
  12G  7.2G /- WinSxS
  29G  225M .
```

The first column in the output tells you how much space a given entry takes up on your disk.
This can be an overcount, however, because of hard links (identical files that are only stored
once on the disk). Hard links under a directory are deduplicated in the first column's number,
but hard links that go outside of a directory to somewhere else will still be counted here.

That's where the second column comes in. It tells you how much of an entry's size is shared
with other entries outside of it because of hard links. In the output above, we can see that
most of the entries have a lot of data shared with other entries, but the root directory only
has 225M shared with others. This tells us that there's a lot of hard links going between all
of the entries shown above.

If you want to see how much of an entry's size is unique to just it, you can subtract the second
column from the first one.

The full list of options can be seen with `dut -h`.

## How to build

`dut` comes with a Makefile, so to install it on your system run:
```
git clone https://codeberg.org/201984/dut.git
sudo make install
```

The default install location is `/usr/local/bin`, but this can be changed by specifying a
`PREFIX` value. For example, to install to `~/.local/bin`:
```
make install PREFIX=$HOME/.local
```

## Benchmarks

`dut` is remarkably fast, but it doesn't win in all cases. It loses to a couple programs when
Linux's disk caches aren't populated yet, which is usually the first time you run it on a certain
directory. On subsequent runs, `dut` beats everything else by a significant margin.

Benchmarked programs:
* `du` from [coreutils](https://www.gnu.org/software/coreutils/coreutils.html) (C)
* [`dua`](https://github.com/byron/dua-cli) (Rust)
* [`pdu`](https://github.com/KSXGitHub/parallel-disk-usage) (Rust)
* [`dust`](https://github.com/bootandy/dust) (Rust)
* [`gdu`](https://github.com/dundee/gdu) (Go)

If you know of a faster program, let me know and I'll add it to these benchmarks.

### Benchmark 1: Measuring performance from Linux's disk cache

The first benchmark is calculating the total disk usage of both of the SSDs in my laptop. I did
warm-up runs beforehand to make sure everything is cached, so this benchmark doesn't touch the disk
at all.

#### Specs
- CPU: i5-10500h
- RAM: 16 GB
- OS: Arch Linux, kernel 6.8.4

In order to make things fair, I forced `dut` and `dust` to output in color and show 60 rows. I also
added a 10 second sleep between each program's run to limit the effects of thermal throttling.

```
$ hyperfine 'dut -Cn 60 /' 'du -sh /' 'pdu /' 'dust -n 60 /' 'gdu --non-interactive /' 'dua /' -s 'sleep 10' -i
Benchmark 1: dut -Cn 60 /
  Time (mean ± σ):     467.4 ms ±  11.7 ms    [User: 410.3 ms, System: 4595.4 ms]
  Range (min … max):   442.5 ms … 485.4 ms    10 runs

Benchmark 2: du -sh /
  Time (mean ± σ):      3.566 s ±  0.049 s    [User: 0.775 s, System: 2.743 s]
  Range (min … max):    3.486 s …  3.615 s    10 runs

  Warning: Ignoring non-zero exit code.

Benchmark 3: pdu /
  Time (mean ± σ):     732.1 ms ±  13.8 ms    [User: 1887.3 ms, System: 6123.5 ms]
  Range (min … max):   717.6 ms … 755.8 ms    10 runs

Benchmark 4: dust -n 60 /
  Time (mean ± σ):      1.438 s ±  0.031 s    [User: 3.068 s, System: 6.962 s]
  Range (min … max):    1.397 s …  1.481 s    10 runs

Benchmark 5: gdu --non-interactive /
  Time (mean ± σ):      1.361 s ±  0.103 s    [User: 7.556 s, System: 7.034 s]
  Range (min … max):    1.298 s …  1.569 s    10 runs

  Warning: Statistical outliers were detected. Consider re-running this benchmark on a quiet system without any interferences from other programs. It might help to use the '--warmup' or '--prepare' options.

Benchmark 6: dua /
  Time (mean ± σ):      1.459 s ±  0.133 s    [User: 4.054 s, System: 9.640 s]
  Range (min … max):    1.346 s …  1.659 s    10 runs

Summary
  dut -Cn 60 / ran
    1.57 ± 0.05 times faster than pdu /
    2.91 ± 0.23 times faster than gdu --non-interactive /
    3.08 ± 0.10 times faster than dust -n 60 /
    3.12 ± 0.30 times faster than dua /
    7.63 ± 0.22 times faster than du -sh /
```

The warning about a non-zero exit code was due to `du` reporting an error for not being able
to access directories in /proc and /root.

### Benchmark 2: SSD Performance

This bechmark is operating on the same filesystem as above, except I'm flushing the disk caches
in-between runs. This results in having to read all the data from the SSD each time instead of
getting it from RAM.

This is a more niche use-case since most of the time `dut` will be running from the cache. It
only has to read from the disk on its first run in a particular directory.

#### Drives:
* Intel 660p 512G
* SX8200PNP-512GT-S

```
$ sudo hyperfine 'dut -Cn 60 /' 'du -sh /' 'pdu /' 'dust -n 60 /' 'gdu --non-interactive /' 'dua /' -s 'sleep 10' -i -M 3 -p 'echo 1 > /proc/sys/vm/drop_caches'
Benchmark 1: dut -Cn 60 /
  Time (mean ± σ):      5.773 s ±  0.184 s    [User: 0.406 s, System: 4.694 s]
  Range (min … max):    5.561 s …  5.881 s    3 runs

  Warning: Statistical outliers were detected. Consider re-running this benchmark on a quiet system without any interferences from other programs. It might help to use the '--warmup' or '--prepare' options.

Benchmark 2: du -sh /
  Time (mean ± σ):     20.779 s ±  0.058 s    [User: 0.767 s, System: 3.709 s]
  Range (min … max):   20.712 s … 20.819 s    3 runs

  Warning: Ignoring non-zero exit code.

Benchmark 3: pdu /
  Time (mean ± σ):      4.279 s ±  0.292 s    [User: 1.701 s, System: 5.543 s]
  Range (min … max):    4.072 s …  4.613 s    3 runs

Benchmark 4: dust -n 60 /
  Time (mean ± σ):      5.009 s ±  0.348 s    [User: 2.608 s, System: 6.211 s]
  Range (min … max):    4.726 s …  5.397 s    3 runs

Benchmark 5: gdu --non-interactive /
  Time (mean ± σ):      4.090 s ±  0.081 s    [User: 7.027 s, System: 6.989 s]
  Range (min … max):    4.040 s …  4.183 s    3 runs

  Warning: Statistical outliers were detected. Consider re-running this benchmark on a quiet system without any interferences from other programs. It might help to use the '--warmup' or '--prepare' options.

Benchmark 6: dua /
  Time (mean ± σ):      6.269 s ±  0.133 s    [User: 4.541 s, System: 12.786 s]
  Range (min … max):    6.162 s …  6.418 s    3 runs

Summary
  gdu --non-interactive / ran
    1.05 ± 0.07 times faster than pdu /
    1.22 ± 0.09 times faster than dust -n 60 /
    1.41 ± 0.05 times faster than dut -Cn 60 /
    1.53 ± 0.04 times faster than dua /
    5.08 ± 0.10 times faster than du -sh /
```

### Benchmark 3: HDD Performance

For this benchmark, I did the same benchmark as the last except I did it on an HDD instead. Some of
the Rust programs perform quite badly in this scenario, but `dua` still beats `dut` narrowly.

The test location is my home directory on an old Linux installation. There are approximately 26k
subdirectories.

The drive being measured is a 2 terabyte 5400rpm Western Digital WD20EFRX connected to my laptop
with a USB enclosure.
```
$ sudo hyperfine 'dut -Cn 60' 'du -sh' 'pdu' 'dust -n 60' 'gdu --non-interactive' 'dua' -s 'sleep 10' -i -M 3 -p 'echo 1 > /proc/sys/vm/drop_caches'
Benchmark 1: dut -Cn 60
  Time (mean ± σ):     36.720 s ±  0.350 s    [User: 0.078 s, System: 0.740 s]
  Range (min … max):   36.411 s … 37.100 s    3 runs

Benchmark 2: du -sh
  Time (mean ± σ):     44.810 s ±  0.043 s    [User: 0.108 s, System: 0.657 s]
  Range (min … max):   44.767 s … 44.854 s    3 runs

Benchmark 3: pdu
  Time (mean ± σ):     81.361 s ±  0.954 s    [User: 0.320 s, System: 0.935 s]
  Range (min … max):   80.675 s … 82.451 s    3 runs

Benchmark 4: dust -n 60
  Time (mean ± σ):     86.991 s ±  2.449 s    [User: 0.337 s, System: 1.042 s]
  Range (min … max):   84.411 s … 89.286 s    3 runs

Benchmark 5: gdu --non-interactive
  Time (mean ± σ):     41.096 s ±  0.229 s    [User: 1.086 s, System: 1.165 s]
  Range (min … max):   40.837 s … 41.273 s    3 runs

Benchmark 6: dua
  Time (mean ± σ):     34.472 s ±  0.965 s    [User: 9.107 s, System: 29.192 s]
  Range (min … max):   33.733 s … 35.564 s    3 runs

Summary
  dua ran
    1.07 ± 0.03 times faster than dut -Cn 60
    1.19 ± 0.03 times faster than gdu --non-interactive
    1.30 ± 0.04 times faster than du -sh
    2.36 ± 0.07 times faster than pdu
    2.52 ± 0.10 times faster than dust -n 60
```

#### Why are `pdu` and `dust` so bad on HDD?
It's hard to say. My best guess is they have a really HDD-unfriendly access pattern, since they
both use Rayon for multithreading which uses FIFO ordering for tasks. This results in them doing
a breadth-first search of the filesystem, whereas `dut` and `du` both use depth-first search. I
don't know why one ordering is better than the other, but the difference is pretty drastic.

I also think that ordering is the reason `dut` doesn't do so well on SSD either, but I'm not so
sure of that.
