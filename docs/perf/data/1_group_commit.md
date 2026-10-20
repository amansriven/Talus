Experiment 1: group commit on vs off (fillrandom, sync=true).

| variant | threads | ops/s | p99 µs | writes/fsync | cycles/op |
|---|---|---|---|---|---|
| group commit | 1 | 6,736.5 | 352.0 | 1.0 | 6329.31 |
| group commit | 2 | 9,659.5 | 422.7 | 1.7 | 81373.37 |
| group commit | 4 | 17,891.3 | 385.3 | 3.0 | 103804.05 |
| no grouping | 1 | 6,789.4 | 292.6 | 1.0 | 6234.48 |
| no grouping | 2 | 5,920.8 | 709.1 | 1.0 | 113505.47 |
| no grouping | 4 | 5,903.1 | 814.1 | 1.0 | 119150.11 |
