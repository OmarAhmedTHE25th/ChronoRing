# ChronoRing Formal Verification (TLA+ & Lean 4)

This directory contains formal models and machine-checked proofs covering the lock-free ring-buffer algorithms used across ChronoRing.

## Contents

| Artifact | Purpose |
|---|---|
| `tla/SpscRing.tla` / `SpscRing.cfg` | Exhaustive state-space TLA+ model of `SpscRingBuffer` including private shadow-cursor caching (`cachedTail`, `cachedHead`), `reserve`/`commit`, and `peek`/`consume`. Verifies `TypeInvariant`, `BoundsInvariant`, `ShadowMonotonicity`, `NoDataRace`, and strict `FifoOrder`. |
| `tla/MpscRing.tla` / `MpscRing.cfg` | Multi-producer TLA+ model of Vyukov's bounded MPSC sequence protocol (`enqueuePos`, `dequeuePos`, per-slot `seq`). Verifies `UniqueSlotOwnership`, `SequenceSafety`, and `NoSpuriousOrDuplicateItems`. |
| `lean/ChronoRingProofs.lean` | Core Lean 4 theorems (zero external dependencies, zero `sorry`) proving shadow-cursor bounds, modular slot mutual exclusion (`spsc_slot_disjoint`), and active Vyukov ticket disjointness (`vyukov_active_tickets_disjoint_slots`). |

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
