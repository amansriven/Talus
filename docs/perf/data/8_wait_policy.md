Experiment 8 (supplement to 1 and 2): how group-commit waiters wait (fillrandom).

| policy | threads | ops/s | p99 µs | writes/group | cycles/op |
|---|---|---|---|---|---|
| no sync, spin 0, yield 0 µs | 1 | 646419 | 2.6 | 1.0 | 3081.45 |
| no sync, spin 0, yield 0 µs | 2 | 363741 | 63.4 | 1.1 | 5194.56 |
| no sync, spin 0, yield 0 µs | 4 | 81,711.4 | 106.6 | 1.8 | 11055.10 |
| no sync, spin 256, yield 0 µs | 1 | 645954 | 2.6 | 1.0 | 3065.56 |
| no sync, spin 256, yield 0 µs | 2 | 446287 | 10.3 | 1.4 | 11003.81 |
| no sync, spin 256, yield 0 µs | 4 | 191606 | 102.3 | 1.8 | 20380.19 |
| no sync, spin 4096, yield 0 µs | 1 | 646244 | 2.6 | 1.0 | 3050.87 |
| no sync, spin 4096, yield 0 µs | 2 | 455133 | 7.9 | 1.4 | 11617.11 |
| no sync, spin 4096, yield 0 µs | 4 | 357333 | 24.4 | 2.0 | 21592.27 |
| no sync, spin 256, yield 100 µs | 1 | 647422 | 2.6 | 1.0 | 3067.33 |
| no sync, spin 256, yield 100 µs | 2 | 450606 | 8.2 | 1.5 | 11483.91 |
| no sync, spin 256, yield 100 µs | 4 | 376959 | 23.6 | 2.1 | 19317.69 |
| sync, spin 0, yield 0 µs | 1 | 6,728.4 | 341.8 | 1.0 | 6148.03 |
| sync, spin 0, yield 0 µs | 2 | 8,591.0 | 419.6 | 1.5 | 11720.39 |
| sync, spin 0, yield 0 µs | 4 | 14,113.2 | 422.9 | 2.5 | 14346.53 |
| sync, spin 256, yield 0 µs | 1 | 6,853.7 | 245.0 | 1.0 | 6224.64 |
| sync, spin 256, yield 0 µs | 2 | 8,694.1 | 401.4 | 1.5 | 23764.63 |
| sync, spin 256, yield 0 µs | 4 | 14,249.6 | 419.8 | 2.5 | 31837.69 |
| sync, spin 4096, yield 0 µs | 1 | 6,870.5 | 200.3 | 1.0 | 6007.78 |
| sync, spin 4096, yield 0 µs | 2 | 9,734.0 | 412.9 | 1.7 | 155613.59 |
| sync, spin 4096, yield 0 µs | 4 | 16,438.3 | 391.7 | 2.8 | 199606.20 |
| sync, spin 256, yield 100 µs | 1 | 6,826.3 | 287.0 | 1.0 | 6081.22 |
| sync, spin 256, yield 100 µs | 2 | 9,424.5 | 396.0 | 1.6 | 83114.23 |
| sync, spin 256, yield 100 µs | 4 | 17,093.1 | 379.6 | 2.9 | 105320.14 |
