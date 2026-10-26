> Originally developed at [codeberg.org/201984/dut](https://codeberg.org/201984/dut) · GPL-3.0

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
cd dut/
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
- OS: Arch Linux, kernel 6.11.8

In order to make things fair, I forced `dut` and `dust` to output in color and show 60 rows. I also
added a 10 second sleep between each program's run to limit the effects of thermal throttling.

```
$ hyperfine 'dut -Cn 60 /' 'du -sh /' 'pdu /' 'dust -n 60 /' 'gdu --non-interactive /' 'dua /' -s 'sleep 10' -i
Benchmark 1: dut -Cn 60 /
  Time (mean ± σ):     779.7 ms ±  28.5 ms    [User: 941.6 ms, System: 7151.0 ms]
  Range (min … max):   745.2 ms … 841.6 ms    10 runs

Benchmark 2: du -sh /
  Time (mean ± σ):      5.356 s ±  0.058 s    [User: 0.962 s, System: 4.331 s]
  Range (min … max):    5.286 s …  5.498 s    10 runs

  Warning: Ignoring non-zero exit code.

Benchmark 3: pdu /
  Time (mean ± σ):      1.127 s ±  0.021 s    [User: 2.912 s, System: 9.551 s]
  Range (min … max):    1.107 s …  1.179 s    10 runs

Benchmark 4: dust -n 60 /
  Time (mean ± σ):      2.206 s ±  0.032 s    [User: 4.680 s, System: 10.943 s]
  Range (min … max):    2.169 s …  2.283 s    10 runs

Benchmark 5: gdu --non-interactive /
  Time (mean ± σ):      2.927 s ±  0.107 s    [User: 19.045 s, System: 12.174 s]
  Range (min … max):    2.813 s …  3.193 s    10 runs

Benchmark 6: dua /
  Time (mean ± σ):      2.313 s ±  0.180 s    [User: 6.432 s, System: 15.732 s]
  Range (min … max):    2.088 s …  2.493 s    10 runs

Summary
  dut -Cn 60 / ran
    1.45 ± 0.06 times faster than pdu /
    2.83 ± 0.11 times faster than dust -n 60 /
    2.97 ± 0.26 times faster than dua /
    3.75 ± 0.19 times faster than gdu --non-interactive /
    6.87 ± 0.26 times faster than du -sh /
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
  Time (mean ± σ):      9.976 s ±  0.453 s    [User: 0.811 s, System: 7.427 s]
  Range (min … max):    9.475 s … 10.357 s    3 runs

Benchmark 2: du -sh /
  Time (mean ± σ):     37.995 s ±  0.050 s    [User: 1.247 s, System: 5.969 s]
  Range (min … max):   37.947 s … 38.047 s    3 runs

  Warning: Ignoring non-zero exit code.

Benchmark 3: pdu /
  Time (mean ± σ):      7.864 s ±  0.125 s    [User: 2.577 s, System: 8.941 s]
  Range (min … max):    7.737 s …  7.987 s    3 runs

Benchmark 4: dust -n 60 /
  Time (mean ± σ):      8.821 s ±  0.059 s    [User: 3.992 s, System: 10.052 s]
  Range (min … max):    8.760 s …  8.878 s    3 runs

Benchmark 5: gdu --non-interactive /
  Time (mean ± σ):      7.867 s ±  0.071 s    [User: 11.550 s, System: 11.608 s]
  Range (min … max):    7.802 s …  7.943 s    3 runs

Benchmark 6: dua /
  Time (mean ± σ):     10.551 s ±  0.006 s    [User: 7.465 s, System: 19.931 s]
  Range (min … max):   10.544 s … 10.557 s    3 runs

Summary
  pdu / ran
    1.00 ± 0.02 times faster than gdu --non-interactive /
    1.12 ± 0.02 times faster than dust -n 60 /
    1.27 ± 0.06 times faster than dut -Cn 60 /
    1.34 ± 0.02 times faster than dua /
    4.83 ± 0.08 times faster than du -sh /
```

### Benchmark 3: HDD Performance

For this benchmark, I did the same benchmark as the last except I did it on an HDD instead. Some of
the Rust programs perform quite badly in this scenario, but `dua` still beats `dut` narrowly.

The test location is my home directory on an old Linux installation. There are approximately 26k
subdirectories.

The drive being measured is a 2 terabyte 5400rpm Western Digital WD20EFRX connected to my laptop
with a USB enclosure.
```
$ sudo hyperfine 'dut -Cn 60' 'du -sh' 'pdu .' 'dust -n 60' 'gdu --non-interactive' 'dua' -s 'sleep 10' -i -M 3 -p 'echo 1 > /proc/sys/vm/drop_caches'
Benchmark 1: dut -Cn 60
  Time (mean ± σ):     36.418 s ±  0.376 s    [User: 0.080 s, System: 0.865 s]
  Range (min … max):   36.147 s … 36.847 s    3 runs

Benchmark 2: du -sh
  Time (mean ± σ):     44.925 s ±  0.255 s    [User: 0.120 s, System: 0.795 s]
  Range (min … max):   44.739 s … 45.216 s    3 runs

Benchmark 3: pdu .
  Time (mean ± σ):     45.019 s ±  0.151 s    [User: 0.176 s, System: 0.901 s]
  Range (min … max):   44.881 s … 45.181 s    3 runs

Benchmark 4: dust -n 60
  Time (mean ± σ):     86.746 s ±  1.076 s    [User: 0.340 s, System: 1.228 s]
  Range (min … max):   85.514 s … 87.504 s    3 runs

Benchmark 5: gdu --non-interactive
  Time (mean ± σ):     41.390 s ±  0.584 s    [User: 0.941 s, System: 1.204 s]
  Range (min … max):   40.936 s … 42.048 s    3 runs

Benchmark 6: dua
  Time (mean ± σ):     34.058 s ±  0.490 s    [User: 10.258 s, System: 27.648 s]
  Range (min … max):   33.660 s … 34.605 s    3 runs

Summary
  dua ran
    1.07 ± 0.02 times faster than dut -Cn 60
    1.22 ± 0.02 times faster than gdu --non-interactive
    1.32 ± 0.02 times faster than du -sh
    1.32 ± 0.02 times faster than pdu .
    2.55 ± 0.05 times faster than dust -n 60
```

#### Why is `dust` so bad on HDD?
It's hard to say. My best guess is it has a really HDD-unfriendly access pattern, since it
uses Rayon for multithreading which uses FIFO ordering for tasks. This results in `dust` doing
a breadth-first search of the filesystem, whereas `dut` and `du` both use depth-first search. I
don't know why one ordering is better than the other, but the difference is pretty drastic.

I also think that ordering is the reason `dut` doesn't do so well on SSD either, but I'm not so
sure of that.
