# Converting between lossless formats with `call` and `uncall`: analysis

## The idea

Two lossless representations of the same content, and a single program that
relates them. Format A is the raw byte sequence; format B is the same
information run-length encoded. Going from A to B is a bijection, so the
decoder is not a second program to write and keep in sync: it is the encoder
read backwards.

The program recognizes the type of the input file and chooses the direction:

```kairos
if tipo == 0 then
    call rle(a, b, na, np)
else
    uncall rle(a, b, na, np)
fi tipo == 0
```

Under that fork there is a single procedure, `rle`. No decoding line exists in
the source. That is the point: in a reversible language decoding is not
implemented, it is obtained.

## Why `rle` must be clean

`rle` consumes the whole input and leaves only the output:

```
pre   src = [sentinel, byte_n-1, ..., byte_0]   nsrc = n   dst empty    npair = 0
post  src = [sentinel]                          nsrc = 0   dst = pairs  npair = k
```

This is the notion of *clean* of Lyngby, Nylandsted, Glück and Yokoyama. If
`rle` left intermediate state behind, its inverse would not be a decoder: to
run it would also need that garbage, that is information a format-B file does
not contain. Cleanliness is not elegance, it is the condition that makes the
inverse usable on a real file.

## Two language details that changed the code

**Kairos guards were simple comparisons.** There was no disjunction. The
natural condition for scanning a run is "something is left in `src`, or I just
consumed an element", and it could not be written. The attempt to work around
it with `(nsrc + cont) > 0` got past the parser but not the VM, which split the
`EVAL` line on spaces and did not rebuild the parenthesis.

The solution was not to bend the language but to change the data: a `-1`
sentinel at the bottom of the stack. No byte is -1, so the comparison always
stops by itself and the emptiness test disappears from the code. The guard is
back to `cont == 1`, a simple comparison.

**Every `if` must state on exit which branch it took.** After consuming the top
of `src` it is no longer possible to check that it was equal to `v`. A witness
is needed: `cont` is 1 when the last element read was equal, 0 when the run is
over. It is the same pattern as the `pref` used for the lexicographic
comparison in the Burrows–Wheeler transform, and it seems to be the standard
way of writing non-trivial conditionals in Kairos.

## Results

Binary PGM images, a filled circle on a light background. Full round trip
A → B → A with a byte-by-byte comparison of the final file against the
original.

| image | A (bytes) | runs | B (bytes) | round trip |
|---|---|---|---|---|
| 16x16 | 269 | 33 | 103 | identical |
| 32x32 | 1,037 | 53 | 163 | identical |
| 64x64 | 4,109 | 93 | 283 | identical |
| 128x128 | 16,399 | 179 | 541 | identical |
| 256x256 | 65,551 | 343 | 1,033 | identical |

The format-B container spends three bytes per run (value on one byte, length
on two). With the length on one byte and runs split at 255 it would be two
bytes per run, but the count does not change in nature.

## Efficiency

Times for the full round trip, separating the frontend from the execution:

| image | lines of the generated program | parse | execution |
|---|---|---|---|
| 32x32 | 1,132 | 0.1 s | 0.2 s |
| 64x64 | 4,204 | 0.6 s | 0.2 s |
| 128x128 | 16,494 | 6.3 s | 0.8 s |
| 256x256 | 65,646 | 87.3 s | 2.0 s |

**Execution is linear.** A 65 KB image is compressed and rebuilt in about 2 s.
RLE on stacks only touches the top, never needs random access, and the
reversible VM runs it at the expected cost.

**Parsing is not.** It grows roughly with the square of the program length,
and at 65,646 lines it costs 87.3 s, forty times the execution.

The reason is that the VM has no filesystem: the input cannot be read at run
time, so the file's bytes go into the source as a sequence of `push`. The
program has the size of the data. The wall one hits is not the reversible
machine, it is an LALR frontend in Python digesting a 65-thousand-line source.

The same asymmetry shows between the two directions. The `call` starts from
65,551 bytes and pays 87 s of parsing; the `uncall` starts from 343 pairs, that
is 686 `push`, and finishes in 1.9 s. It is not that the inverse is faster than
itself: its input is smaller, and so is its program.

## How much the inverse really costs

The decoder is free to write. It remains to ask whether it is free to run,
because a slow inverse would be a useless decoder.

The times of the previous table do not answer: the two programs do not do the
same work, the forward one runs n data-loading `push` and the inverse only 2k.
To isolate the conversion alone, each program runs twice, once as it is and
once with the counter set to zero: with `nsrc = 0` (or `npair = 0`) the
conversion does not start, while parsing and loading stay identical. The
difference is the conversion and nothing else.

| bytes | runs | `call` | `uncall` | ratio |
|---|---|---|---|---|
| 1,037 | 53 | 0.02 s | 0.03 s | 2.06x |
| 4,109 | 93 | 0.06 s | 0.11 s | 1.73x |
| 16,399 | 179 | 0.20 s | 0.43 s | 2.18x |
| 65,551 | 343 | 0.97 s | 1.68 s | 1.73x |

**The inverse costs between 1.7 and 2.2 times the forward direction, and the
ratio does not grow with size.** It is a constant factor, not a factor n:
inversion is a local operation on the instruction, not a reworking of the
program. A decoder obtained by inversion is therefore usable, not just
provable.

## Is the inverse also a validating decoder?

This is the question that decides whether the idea holds outside the lab. A
format-B file comes from disk and there is no guarantee that it is in the image
of the encoder. What happens when it is given to the inverse?

The experiment (`esperimenti.py validazione`) builds seven format-B inputs, one
legitimate and six that no forward execution would produce.

**The starting result is negative.** The inverse silently accepts inputs
outside the image and answers with wrong data, or diverges.

| input | outcome |
|---|---|
| reference, image of the forward program | accepts, correct |
| run of length 0 | **does not terminate** |
| two adjacent runs with the same value | **accepts**, and re-encoding the result does not give back the input |
| `npair` smaller than the content | **accepts**, truncated result |
| `npair` larger than the content | rejects, but through `POP` on an empty stack |
| negative length | **does not terminate** |
| value colliding with the sentinel | **accepts** |

### The cause: only one of the two guards was checked

The reversible conditional has two guards and the semantics requires two
checks, one per direction. Going forwards the exit assertion is checked. Going
backwards the exit assertion chooses the branch, and it remains to check that
the entry guard agrees with that choice.

The minimal case showing it:

```kairos
procedure p(int x, int y)
    if x > 0 then
        y += 1
    fi y == 1

procedure main()
    int x
    int y
    x -= 5          // the entry guard is false
    y += 1          // the exit assertion is true
    uncall p(x, y)
    show(y)
```

The starting state is not reachable by any forward execution of `p`. The VM
inverted the `then` branch, brought `y` to 0 and ended successfully. The same
program run forwards with a false exit assertion produced instead the error
`IF/FI not reversible`: the check was there in one direction and missing in
the other. Now both directions do it.

### The real consequence: assertions did not work backwards

In Janus an assertion is written with a two-guard conditional:

```kairos
local int positiva = 0
    if run > 0 then
        positiva += 1
    fi positiva == 1
delocal int positiva = 1
```

Forwards it works: if `run <= 0` the variable stays 0 and the `delocal` fails.
Backwards it should work the same way, and instead **it did absolutely
nothing**, because the branch was chosen from the exit assertion and the guard
`run > 0` was never evaluated again.

This is what makes the gap serious rather than cosmetic: it is not about a
corner of the language, it is about the only way to write a precondition. In a
decoder obtained by inversion, every precondition on the input is exactly an
assertion of this form.

### The missing check, and what changes

The check was added in `src/vm/vm_invert.h` (`check_if_entry_inverse`): after
inverting the branch, when the store is back to the state before the if and
the guard can be evaluated again, it checks that the guard holds exactly when
the branch taken was `then`. The two exceptions are those the forward check
already has: no check where the exit assertion is missing (the branch was
deduced from the guard itself, checking it again would be a tautology) and no
check in `par` branches, where other threads may change shared ints between
the evaluation and the check.

With the check and the assertion `run > 0` written in the source:

| input | before the fix | after |
|---|---|---|
| reference | accepts, correct | accepts, correct |
| run of length 0 | does not terminate | **rejects** |
| two equal adjacent runs | accepts, wrong | **rejects** |
| `npair` too small | accepts, truncated | accepts, truncated |
| `npair` too large | rejects (`POP` on an empty stack) | rejects |
| negative length | does not terminate | **rejects** |
| value = sentinel | accepts | accepts |

Three cases go from silent error or divergence to explicit rejection. Note that
the assertion `run > 0` is in the source in both columns: the left column is
not "without the assertion", it is "with an assertion that is not checked".

The cost of the fix, measured before making it final: **no regressions**. The
Kairos suite gave 58 PASS / 4 FAIL, identical, with the same four failures as
before. All 87 C programs of Mnemo, the real generator of `uncall`, gave
identical output with and without the check. The only two differences
observed turned out to be artifacts: `PC.c` is nondeterministic by itself,
because `par` scheduling changes the order of the output from run to run;
`maze_backtrack.c` takes about five minutes and the sweep timeout cut it at
different points, while run to the end it gives the same result in both modes.
The two programs `try_rollback.c` and `try_backtracking.c` failed under
`--check-invertibility`, but they failed the same way on the VM before the
change.

### What remains out of reach, and why

**The count field.** `npair` is an input of the inverse, and nothing in the
program ties it to the content of `dst`. A count that is too small leaves pairs
unused and nobody notices. One would conclude that self-delimiting formats are
preferable, but the conclusion is the opposite: a Kairos loop needs an entry
assertion that is false on every re-entry, and for a data-driven loop that
assertion is naturally a counter. **The language pushes towards counted
formats, not terminated ones**, and the price is that the counter must be
validated outside the program. The driver does it, checking that `dst` ends
up empty.

**The in-band sentinel.** The value -1 that simplified the guards can be
represented in format B, so a crafted file can contain it and be confused with
the bottom of the stack. The real pipeline never produces it, because bytes
lie between 0 and 255, but it is an attack surface created by the encoding
choice, not by the language.

**Termination.** No guard check makes it decidable whether an inverse
terminates. Here the two divergent cases were closed because the assertion
catches them before the loop, but that is luck of the specific case, not a
theorem.

## The worst case

Incompressible data, every run of length 1, so format B is twice as large as
format A.

| n | runs | B/A | `call` | `uncall` | round trip |
|---|---|---|---|---|---|
| 200 | 199 | 1.99 | 0.1 s | 0.2 s | identical |
| 800 | 797 | 1.99 | 0.2 s | 0.3 s | identical |

The round trip still closes. It is worth saying explicitly, because it is what
distinguishes this work from a compressor: **B is not "the compressed file", it
is "the file in the other format"**, and the bijection does not depend on
whether it compresses. A compressor that doubles the data is useless; a
converter that doubles the data is still a correct converter.

## Comparison with the C path

The very same algorithm, on the very same 269-byte file, written in C and
compiled by Mnemo, cost 290.5 s. Written in Kairos on stacks it costs 0.1 s:
three thousand times less.

The difference is not in the VM, which is the same. It is in the data
structure. In C, RLE is written on arrays with a runtime index, and Mnemo
lowered each indexed access to a chain of comparisons as long as the array: a
linear scan became O(n²). On stacks the same scan only touches the top and
stays O(n).

The comparison must be read honestly: it does not say that Kairos is better
than C, it says that RLE is an algorithm that scans and never jumps, and that
written against the right data structure it pays nothing. An algorithm that
really jumps, like the Burrows–Wheeler transform, pays the factor n on both
paths.

## Next steps

**Reading and writing files from the VM.** It is the change with the best
cost/benefit ratio, and it helps both studies. It decouples the program size
from the data size, removes the 87 s of parsing, and brings the 65 KB case from
90 s to 2 s. It does not touch the reversible semantics: reading and writing
are effects at the boundary, like `show`.

**Recognizing the format inside the program.** Today the type arrives as a
variable written by the generator. With file reading, the program would read
the magic bytes itself and choose the direction, which is the complete form of
the idea.

**More than two formats.** With three representations and the conversions A→B
and B→C, composing `call` and `uncall` gives all six directed paths. The gain
must be stated for what it is: a conventional converter organized as a star
also writes O(k) conversions, not O(k²). What reversibility adds is two
things, both measurable: **half the code**, because each conversion is written
in one direction only, and **the impossibility of the two directions
diverging**, because they are not two programs someone must keep in sync. It
is a narrower argument than the quadratic one, and it is the true one.

**Validating the count inside the language.** Today the fact that `dst` ends up
empty is checked by the Python driver, outside the program. A primitive
asserting an empty stack would move the check inside, and close the last class
of malformed inputs the inverse silently accepts.

**Runs in parallel.** The image splits into independent horizontal bands; the
boundaries never touch because a run does not cross a band if the bands are
cut on the rows. Each band becomes a `par` branch, and it is where
reversibility and concurrency are seen together on a real file.

**Stronger encodings.** LZW and the Burrows–Wheeler transform compress much
better than RLE on non-synthetic data, but they need random access, which on
stacks costs a factor n. They depend on a reversible indexed memory with
constant cost.
