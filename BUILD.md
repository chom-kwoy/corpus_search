# Building corpus_search

## Prerequisites

```bash
$ apt install libabsl-dev libicu-dev libboost-dev
$ curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

## Build

```bash
$ cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
$ cmake --build build --parallel
```
