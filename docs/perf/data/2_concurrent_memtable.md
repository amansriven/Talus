Experiment 2: concurrent vs single-writer memtable inserts (fillrandom, no sync).

| variant | threads | ops/s | p99 µs | IPC | L1d misses/op |
|---|---|---|---|---|---|
| concurrent (CAS) | 1 | 528308 | 4.1 | 0.74 | 82.53 |
| concurrent (CAS) | 2 | 481841 | 9.4 | 0.36 | 100.62 |
| concurrent (CAS) | 4 | 471118 | 24.6 | 0.27 | 112.75 |
| single writer | 1 | 533869 | 4.1 | 0.76 | 82.80 |
| single writer | 2 | 428440 | 10.3 | 0.36 | 96.49 |
| single writer | 4 | 441813 | 15.6 | 0.32 | 106.70 |
