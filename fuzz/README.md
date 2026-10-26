# Fuzzing the readers

reac-repacer relays REAC frames without decoding them, but it reads a few bytes of each frame to decide what to do with
it, it writes a few bytes into each frame it emits, and it reads two texts besides its command line: the warm-start
lock it wrote on an earlier run, and the reply of `uci get` on SIGHUP. Each of those readers and writers is a pure
function of `src/` (`src/repace_wire.c`, `src/repace_conf.c`, their rules written down in `include/`), which the daemon
calls, and each has a libFuzzer harness here. CI builds them with clang under AddressSanitizer and
UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`) and runs them on every push and pull request
(`.github/workflows/fuzz.yml`).

A harness is one C file defining `LLVMFuzzerTestOneInput`. It links the two reader files and libreac's `src/reac.c`
(the readers call `reac_frame_clean_len`, `reac_frame_is_reac` and the braid map of `<reac/reac_braid.h>`), and
nothing else: no socket, no thread, no clock, no environment, no file. Each property is checked with
`if (cond) abort();` in a `noinline` function `check_<property>`, never `assert()`, and every outcome an input can
choose returns 0. An input of 64 MiB or more is refused, as OSS Verify's driver refuses it.

The expected answer of every check is the harness's own: `fuzz/fuzz.h` writes the REAC frame down as the protocol lays
it out (the EtherType, the counter, the type, the frame law `52 + nch * 36` with its FCS residue, the braid as the pair
packed big-endian and word-swapped), and each harness writes down the grammar of the text it reads and its limits
(`LOCK_MAX`, the hot-parameter ranges). No check takes its expected value from the reader under test or from a
constant of `include/`; each names the line of `include/` that states the rule (`contract:`), so a reader whose rule
or limit moves turns the check red.

## The harnesses

| Harness | Reader | Input |
| --- | --- | --- |
| `fuzz_wire` | every reader of a frame off the wire: the forward reader (`rp_fwd_class`), the return relay (`rp_ret_accept`), the downstream listener (`rp_ds_accept`), the clock meter (`rp_meter_counter`), the pacer's counter seed (`rp_counter`), the width rule (`rp_frame_channels`), and the inject-copy reader (`rp_copy_untag`, then `rp_slot_read`) | one byte (bit 0 `--bcast-only`, the rest the slot plus 2), then the frame, cut to the 1600 bytes the daemon's receive buffer holds |
| `fuzz_egress` | every write into a frame the pacer emits: the counter restamp (`rp_restamp`), the rate-change mute (`rp_mute`), the injectors' gate (`rp_slot_width`) and writer (`rp_slot_write`) | the slot plus 2, the counter, 12 samples, then the frame |
| `fuzz_lock` | `rp_lock_lookup`, the warm-start lock (`/etc/reac-repacer.lock`) | the rate, the port name, then the file |
| `fuzz_uci` | `rp_uci_int` on the five hot parameters' `uci get` replies, then `rp_hot_clamp`, as the SIGHUP path stages them | the ring, then each parameter's running value and reply |

### The properties

- **fuzz_wire**: each reader takes exactly the frames its rule names and reads the field it names; a reader that
  declines a frame leaves what it would have written (the meter's counter, the copy reader's samples); the samples of
  a slot are the braid's, sign-extended from 24 bits, and are read only when the frame's width holds the slot.
- **fuzz_egress**: the restamp writes the counter's low 16 bits at bytes 14-15 and no other byte; the mute zeroes an
  audio frame of more than 18 bytes from byte 18 to its end and no other frame; the gate lets the injectors write only
  into an audio frame whose width holds the slot; the writer writes each sample's low 24 bits at the braid's three
  bytes and no other byte, and nothing into a frame the gate declines.
- **fuzz_lock**: no_overread (the file with `0` after it and with a space after it gives one answer) and lookup (the
  period restored is the one the record grammar of `include/repace_conf.h` gives, read by the harness: the whole first
  field is the port, the period and the label are 1 to 18 digits, the period is within 5% of the detected rate's own
  standard period, bounds included, the last record wins, a line with a NUL is no record, and a file past 64 KiB
  restores nothing).
- **fuzz_uci**: no_overread (likewise), value (the value is the reply grammar's, or the running value for a reply that
  is no int, past an int included) and clamp (each parameter held to its range, the adaptive ceiling to the floor and
  above, the margin to the ring of the build).

## What has no harness, and why

- **RTP.** reac-repacer reads no RTP: the relay moves REAC frames only (EtherType 0x8819), and every frame it reads is
  read by one of the readers above.
- **The command line.** `main()` reads its flags with `strcmp` and `atoi`, in among the opening of the sockets and
  the start of the threads. The command line is the operator's or the init script's, which builds it from the typed
  options of `/etc/config/reac-repacer`; no byte from the wire or from a file another party writes reaches it.
- **ubus set.** libubus parses the message and hands the daemon typed 32-bit values (`blobmsg_get_u32`); the daemon
  then holds them to their ranges with `rp_hot_clamp`, which `fuzz_uci` drives. The blob parser is libubox's.
- **The lock's writer.** `lock_save` rewrites the lock and copies the lines of the ports it does not own as they are.
  It writes what the reader reads; it decides which lines are its own by the first 15 bytes of the first field
  (`sscanf("%15s")`), so a line whose first field is longer than a 15-byte port name it owns and starts with it is
  dropped on the next save. The reader no longer reads such a line as that port's.
- **The relay loops.** The ring, the pacing clock, the drain servo, the PLL and gap concealment are sockets, threads
  and a clock; the decisions they make on bytes are the readers above, and gap concealment sends the last frame again
  through the same restamp and mute.

## The seed corpus

`fuzz/corpus/<harness>/` is written by `fuzz/gen-corpus.c` (`make fuzz-corpus`) from the frame law and the two
grammars: the frames of a rig (S-0808, S-1608 and S-4000 returns, the 40-channel downstream broadcast, clean and with
the FCS residue, control frames, a handshake, VLAN-tagged frames), the lock the daemon writes, and the uci replies,
beside one input for each case a check's record names. Nothing is fetched or captured, and the corpus is committed, so
CI and a tarball fuzz from the same seeds. `make fuzz-corpus-check` is red when the committed corpus is not what the
generator writes.

## The coverage witness

`make fuzz-coverage` builds each harness for `llvm-cov` without sanitizers, runs it once over its corpus (`-runs=0`),
and prints the line coverage of the reader functions it drives and every line of them no seed reached
(`fuzz/coverage.sh` lists the functions). `fuzz/coverage-floor.txt` holds the lines each harness covers today, and the
target is red when one falls below it. Every `abort()` of a harness, and the statement that guards it, must be reached
by a seed too (`fuzz/checks.awk`): a check no input reaches is a property nobody tests.

| Harness | Reader lines | Covered | Lines no input reaches |
| --- | ---: | ---: | --- |
| wire | 82 | 82 | |
| egress | 45 | 44 | `rp_frame_channels`'s refusal of a negative length: the pacer's frames have none (`fuzz_wire` reaches it) |
| lock | 64 | 64 | |
| uci | 55 | 55 | |

## The records

Every `abort()` of `fuzz/` is a check, and its comment names a bug it catches: a one-line change of `src/` or
`include/` and the seed that shows it, in the form OSS Verify reads (docs/build-contract.md of oss-verify, "The checks
of a fuzz target"):

```
/* The return relay passes every REAC frame of 14 bytes or more, handshakes
 * included, and nothing else.
 * contract: include/repace_wire.h:33-34
 * mutant: src/repace_wire.c:47 `n >= 14 && is_reac(f)` -> `n >= 14`
 * input: not-reac-ipv4 */
if (rp_ret_accept(f, n) != want)
	abort();
```

A stop after an allocation of the harness's own fails has one line `not a property: <reason>` instead. `make
fuzz-records` (`fuzz/records.sh`) proves each record alone: the harness, plain and native with `fuzz/standalone.c` for
its driver, exits 0 on the seed as the tree is; with the change made and the harness built again, it exits 134 on the
seed, at that check's own `abort()`. A check with neither a record nor a `not a property` line, or with lines not of
that form, makes `make fuzz-records` and `make fuzz-coverage` red (`fuzz/checks.awk`). A record's line is the line of
`src/` or `include/` at the commit it is read at, so a change to a reader moves the records it shifts.

## The patches

`make fuzz-sabotage` proves each property finds a bug over the whole corpus. Each
`fuzz/sabotage/<harness>-<property>.patch` is a bug in a reader, which OSS Verify reads as the breakage patches of
`mutants: fuzz/sabotage`; above its `diff --git` line it names the check it breaks and the seed that catches it. The
target applies each, rebuilds, and runs the harness over every seed of its corpus, one at a time, without fuzzing. A
patch is found when the seed its header names aborts in that property's own `check_` function, and every seed that
fails does so by an `abort()` in a `check_` function. `make fuzz-records` proves the header's line too.

| Patch | The bug | Named seed |
| --- | --- | --- |
| wire-forward | the forward reader takes a frame whose REAC EtherType sits behind a VLAN tag | `s0808-audio-tagged` |
| egress-inject | an odd slot is written at the bytes of the even channel of its pair | `s0808-audio-slot-3` |
| lock-lookup | a period is held to the standard of its own nearest rate, not of the rate detected | `rate-96000-other-rate-period` |
| uci-value | a reply past an int is cast to one | `prefill-4294967297` |
| uci-clamp | the adaptive floor is held at the bottom only, so it ends above the ceiling | `adapt-min-2000` |

## What the harnesses found

Writing the lock and uci readers down as grammars, for their harnesses, showed four defects of the readers they
replace, each fixed in this tree with a test in `tests/test_conf.c`:

- A warm-start record's period was held to the standard of its own nearest rate, not of the rate detected:
  `lan1 250000 96000 0` restored a 48 kHz period into a 96 kHz start. (`lock-lookup` puts it back.)
- A record's port was the first 15 bytes of its first field: for the port `abcdefghijklmno`, the line
  `abcdefghijklmno125000 96000 0` restored 125000 ns.
- The adaptive floor was held at the bottom only: a floor of 2000 ms left the window at min 2000, max 1000.
  (`uci-clamp` puts it back.)
- A uci reply past an int was cast to one: 4294967297 staged 1. (`uci-value` puts it back.)

## Running

```
make fuzz-libreac            # clone libreac at the pinned tag, when subprojects/libreac is absent
make fuzz                    # build every harness into build-fuzz/
make fuzz-run FUZZ_TIME=60   # fuzz each for 60 s; red on any crash
make fuzz-sabotage           # every patch found, each in the property its header names
make fuzz-records            # every check's own record, alone, makes that check abort on its seed
make fuzz-coverage           # the coverage witness and its floors
make fuzz-static             # each harness linked fully static with a plain driver, over its corpus
make fuzz-corpus             # write fuzz/corpus/ again
make fuzz-corpus-check       # the committed corpus is what the generator writes
```

They need clang with libFuzzer (`clang compiler-rt` on Fedora), `llvm-cov`, `llvm-profdata` and `llvm-readelf`
(`llvm`), git for the patches, and the static libc (`glibc-static`) for `fuzz-static`. The meson build is never
touched: everything lands in `build-fuzz/`.

AddressSanitizer does not support a fully static executable, and libFuzzer's own runtime faults at startup under
`-static`. `make fuzz-static` links the same harness sources and readers with `fuzz/standalone.c`, a driver that runs
each file it is given through `LLVMFuzzerTestOneInput` once, into an executable with no library beyond libc: the build
a verifier that brings its own driver takes.

## Reproducing a finding

`make fuzz-run` writes the input that crashed a harness to `fuzz/findings/<harness>-crash-<sha1>` (or `leak-`,
`timeout-`, `oom-`) and the harness's log beside it; CI uploads the directory as the `fuzz-findings` artifact. To
replay it:

```
make fuzz
build-fuzz/fuzz_<harness> fuzz/findings/<harness>-crash-<sha1>
```

Once the bug is fixed, have `fuzz/gen-corpus.c` write the input under a name that says what it is, so every later run
replays it first.
