Experiment 3: thread-local SuperVersion vs mutex-protected acquisition (readrandom).

| variant | threads | ops/s | p99.9 µs | slow-path acquisitions | cycles/op |
|---|---|---|---|---|---|
| thread-local (lock-free) | 1 | 822155 | 17.9 | 0 | 4789.79 |
| thread-local (lock-free) | 2 | 1,606,140.0 | 16.7 | 0 | 4792.11 |
| thread-local (lock-free) | 4 | 3,167,260.0 | 16.8 | 0 | 4815.04 |
| mutex | 1 | 827332 | 17.6 | 1000000 | 4757.09 |
| mutex | 2 | 1,403,900.0 | 16.4 | 2000000 | 5410.97 |
| mutex | 4 | 2,179,970.0 | 21.0 | 4000000 | 5946.38 |
