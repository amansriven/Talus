Experiment 6: pread vs mmap table reads, huge pages off vs on (readrandom).

| variant | ops/s | p50 µs | p99 µs | L2 misses/op | LLC misses/op | cycles/op |
|---|---|---|---|---|---|---|
| pread | 965224 | 3.1 | 20.7 | 28.65 | n/a | 8681.77 |
| pread + huge pages | 983480 | 3.1 | 20.7 | 25.95 | n/a | 8367.32 |
| mmap | 2,496,350.0 | 1.5 | 2.5 | 19.86 | n/a | 6413.50 |
| mmap + huge pages | 2,461,700.0 | 1.5 | 2.5 | 19.82 | n/a | 6376.07 |
