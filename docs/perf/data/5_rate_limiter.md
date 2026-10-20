Experiment 5: protecting reads from compaction: rate limits and background CPU priority (readwhilewriting, open loop, half the cores reading).

| variant | p99 µs | p99.9 µs | p99.99 µs | writer MB/s | write stall s | compactions | limiter wait s |
|---|---|---|---|---|---|---|---|
| limiter off | 19.5 | 1,701.9 | 3,391.5 | 51.4 | 5.5 | 756 | 0.0 |
| limit 8 MiB/s | 4.7 | 8.8 | 572.9 | 3.3 | 19.3 | 40 | 127.6 |
| limit 16 MiB/s | 5.1 | 11.3 | 557.1 | 5.3 | 18.9 | 65 | 110.0 |
| limit 32 MiB/s | 5.8 | 61.9 | 2,379.8 | 9.8 | 18.0 | 126 | 83.6 |
| limit 64 MiB/s | 6.3 | 77.4 | 1,845.2 | 13.8 | 17.2 | 194 | 65.2 |
| limiter off, bg nice 10 | 11.0 | 148.5 | 2,715.7 | 51.8 | 5.6 | 749 | 0.0 |
