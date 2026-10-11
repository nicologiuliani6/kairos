# Reversible lossless compression

A case study where reversibility and concurrency are both useful: compressing
and decompressing are by construction each other's inverse, so a single
reversible program implements both, and the blocks the input is split into are
independent, so they are transformed in parallel.

Reference: Lyngby, Nylandsted, Glück, Yokoyama, *Towards Clean Reversible
Lossless Compression: A Reversible Programming Experiment with Zip*, RC 2024,
LNCS 14680, pp. 94–102. The structure and the test vector come from there; that
work, however, is entirely sequential.

## Files

| file | content |
|---|---|
| `indice_su_stack.kairos` | indexed access on top of stacks (the bridge needed before arrays; kept as an example) |
| `bwt.kairos` | Burrows–Wheeler transform of one block, and its inverse |
| `bwt_parallelo.kairos` | blocks as branches of a `par`, columns and indices delivered over channels |
| `compress_par.kairos` | block-wise delta transform: the concurrent skeleton, easier to read |
| `genera_bwt.py` | generates the benchmark cases, sequential and concurrent variants |
| `converti.kairos` | conversion between two lossless formats: one procedure, `call` one way and `uncall` the other |
| `converti.py` | converter driver: input file, detected format, output file |
| `esperimenti.py` | cost of the inverse, validation, worst case |
| `analisi_converti.md` | converter results and measurements (Italian) |
| `casi/guardia_inversa.kairos` | minimal case: the entry guard checked when inverting an `if` |
| `cerchio.pgm` | test image, 16x16 greyscale |

Every `.kairos` file is part of `make test`.

## Converting between formats

A raw file and its run-length encoding are two lossless representations of the
same content. The decoder is not written: it is the encoder run backwards, and
the program picks the direction from the file type.

```bash
python3 lossless/converti.py lossless/cerchio.pgm --giro   # A -> B -> A, byte-by-byte comparison
python3 lossless/converti.py lossless/cerchio.pgm          # writes cerchio.pgm.rle1
python3 lossless/converti.py cerchio.pgm.rle1              # back to raw, with uncall
```

The full round trip is verified up to 256x256, i.e. 65,551 bytes, rebuilt
identical. Execution is linear, about 2 s for 65 KB; what grows is the parsing
of the generated source, because the VM does not read files and the bytes end
up in the program. The inverse costs between 1.7 and 2.2 times the forward run,
and the ratio does not grow with size.

```bash
python3 lossless/esperimenti.py tutti   # cost, validation, worst case
```

When an `if` is inverted, the VM re-checks its entry guard against the branch
chosen by the exit assertion, so the inverse rejects states that no forward run
produces and assertions written in the Janus idiom also work backwards. The
minimal case is [`casi/guardia_inversa.kairos`](casi/guardia_inversa.kairos).
Details and measurements in [`analisi_converti.md`](analisi_converti.md).

## Reproducing the measurements

```
python3 lossless/genera_bwt.py <n> <blocks> <seq|par> > /tmp/case.kairos
./venv/bin/python -m src.kairos /tmp/case.kairos
```

The two variants have an identical body: comparing their times measures only
the effect of concurrency.

## Results

Correctness, on the reference work's test vector (`banana`, with the alphabet
reduced to a=1, b=2, n=3):

```
block                    [2, 1, 3, 1, 3, 1]     banana
sorted rotations         [5, 3, 1, 0, 4, 2]
last column              [3, 3, 2, 1, 1, 1]     nnbaaa
index                    3
rebuilt from (column, index)  [2, 1, 3, 1, 3, 1]   banana
```

Cost of one block, `genera_bwt.py <n> 1 seq` (minimum of 3 runs):

```
 n      6     8    12    16    20    24    28    32
 s   0.13  0.17  0.38  0.76  1.42  2.79  3.97  5.63
```

The log-log regression slope between n=12 and n=32 is 2.82: the cost is n^3,
as in the reference work (the fixed start-up time weighs on small blocks).

Concurrency, n=20 (median of 3 runs, 10 for 6 blocks; 6 cores / 12 threads):

```
blocks   sequential   concurrent   speed-up
   2        2.29 s       1.27 s      1.80x
   4        4.49 s       1.41 s      3.19x
   6        6.77 s       1.58 s      4.29x
  12       13.62 s       2.95 s      4.62x
```

Hardware reference: the same block launched as independent processes gives a
throughput of 3.50x (4), 4.59x (6), 5.10x (12). The `par` reaches 91% of it.

The speed-up is almost linear up to 4 blocks and then flattens: beyond the six
physical cores the threads share execution units, and the sequential part
remains (the collector receives from the channels one at a time).
