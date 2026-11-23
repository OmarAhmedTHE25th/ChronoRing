# ChronoRing Formal Models & Arithmetic Lemmas (TLA+ & Lean 4)

This directory contains small bounded TLA+ state-space models and Lean 4 arithmetic lemmas covering index invariants of ChronoRing's ring-buffer algorithms.

## Scope & Limitations

- The TLA+ models run TLC at **bounded capacity 4** (~350 reachable states) under sequential interleaving semantics.
- The Lean 4 file proves **5 arithmetic/modular-index lemmas** (cursor bounds and modular slot index disjointness) with zero `sorry`.
- Neither artifact proves full **linearizability** nor models the **C++20 weak memory model** (`acquire`/`release`/`seq_cst` hardware reorderings), and neither runs automatically in CI.

## Contents

| Artifact | Purpose |
|---|---|
| `tla/SpscRing.tla` / `SpscRing.cfg` | Small bounded (capacity 4) TLA+ model of `SpscRingBuffer` including private shadow-cursor caching (`cachedTail`, `cachedHead`), `reserve`/`commit`, and `peek`/`consume`. Checks `TypeInvariant`, `BoundsInvariant`, `ShadowMonotonicity`, `NoDataRace`, and `FifoOrder`. |
| `tla/MpscRing.tla` / `MpscRing.cfg` | Small bounded (capacity 4) multi-producer TLA+ model of Vyukov's MPSC sequence protocol (`enqueuePos`, `dequeuePos`, per-slot `seq`). Checks `UniqueSlotOwnership`, `SequenceSafety`, and `NoSpuriousOrDuplicateItems`. |
| `lean/ChronoRingProofs.lean` | 5 Lean 4 arithmetic lemmas (zero external dependencies, zero `sorry`) proving shadow-cursor bounds, modular slot disjointness (`spsc_slot_disjoint`), and active Vyukov ticket disjointness (`vyukov_active_tickets_disjoint_slots`). |

## Running TLC (TLA+)

With TLA+ Toolbox or `tla2tools.jar` installed:

```bash
java -cp tla2tools.jar tlc2.TLC -config formal/tla/SpscRing.cfg formal/tla/SpscRing.tla
java -cp tla2tools.jar tlc2.TLC -config formal/tla/MpscRing.cfg formal/tla/MpscRing.tla
```

## Type-Checking Lean 4 Proofs

With Lean 4 (`lean`) installed:

```bash
lean formal/lean/ChronoRingProofs.lean
```
