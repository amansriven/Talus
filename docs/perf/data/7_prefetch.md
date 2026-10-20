Experiment 7: prefetching on vs off in skiplist search (BM_SkipListLookup).

| variant | ns/lookup | lookups/s | cycles/lookup | IPC | L1d misses/lookup | L2 misses/lookup |
|---|---|---|---|---|---|---|
| prefetch, 10,000 keys | 227.2 | 4,400,665.9 | 927.41 | 1.42 | 18.62 | 0.00 |
| prefetch, 1,000,000 keys | 1,118.4 | 894,197.3 | 3857.93 | 0.52 | 56.75 | 11.78 |
| no prefetch, 10,000 keys | 207.4 | 4,822,996.2 | 846.49 | 1.30 | 23.50 | 0.00 |
| no prefetch, 1,000,000 keys | 1,022.0 | 978,605.1 | 3772.87 | 0.44 | 63.76 | 18.96 |
