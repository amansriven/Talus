Experiment 4b: one filter probe for an absent key (BM_BloomProbe, 4M keys, 10 bits/key).

| filter | ns/probe | cycles/probe | L1d misses/probe | L2 misses/probe | LLC misses/probe |
|---|---|---|---|---|---|
| blocked | 20.0 | 78.86 | 1.48 | 0.25 | n/a |
| standard | 23.3 | 91.95 | 3.05 | 0.51 | n/a |
