Experiment 4: cache-line-blocked vs standard bloom filter (readmissing).

| filter | ops/s | p50 µs | p99 µs | L1d misses/op | L2 misses/op | LLC misses/op | false-positive rate |
|---|---|---|---|---|---|---|---|
| blocked | 2,248,070.0 | 0.4 | 1.3 | 20.63 | 0.93 | n/a | 0.958% |
| standard | 2,205,510.0 | 0.4 | 0.7 | 23.59 | 1.75 | n/a | 0.808% |
