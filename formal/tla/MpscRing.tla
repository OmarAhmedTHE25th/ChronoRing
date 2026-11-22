------------------------------ MODULE MpscRing ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Capacity,   \* Power-of-two capacity (e.g. 4)
    Producers,  \* Set of producer IDs (e.g. {p1, p2})
    MaxOps      \* Upper bound on enqueued tickets for finite TLC exploration

ASSUME Capacity \in Nat /\ Capacity >= 2
ASSUME MaxOps \in Nat /\ MaxOps >= 1

VARIABLES
    enqueuePos, \* Shared CAS enqueue ticket counter
    dequeuePos, \* Single-consumer dequeue counter
    seq,        \* Per-slot sequence number [0..Capacity-1] -> Nat
    slotVal,    \* Per-slot payload [0..Capacity-1] -> Nat
    pState,     \* Producer state: [Producers -> "Idle" | "Claimed"]
    pTicket,    \* Claimed ticket position per producer
    nextVal,    \* Monotonic sequence counter assigned on commit
    consumed    \* Set of consumed token IDs

vars == <<enqueuePos, dequeuePos, seq, slotVal, pState, pTicket, nextVal, consumed>>

Idx(pos) == pos % Capacity

Init ==
    /\ enqueuePos = 0
    /\ dequeuePos = 0
    /\ seq = [i \in 0..(Capacity - 1) |-> i]
    /\ slotVal = [i \in 0..(Capacity - 1) |-> 0]
    /\ pState = [p \in Producers |-> "Idle"]
    /\ pTicket = [p \in Producers |-> 0]
    /\ nextVal = 1
    /\ consumed = {}

\* Producer p atomically checks seq[Idx(enqueuePos)] == enqueuePos and claims ticket via CAS.
ProdClaim(p) ==
    /\ pState[p] = "Idle"
    /\ enqueuePos < MaxOps
    /\ LET pos == enqueuePos
           idx == Idx(pos)
       IN
       /\ seq[idx] = pos
       /\ enqueuePos' = pos + 1
       /\ pState' = [pState EXCEPT ![p] = "Claimed"]
       /\ pTicket' = [pTicket EXCEPT ![p] = pos]
       /\ UNCHANGED <<dequeuePos, seq, slotVal, nextVal, consumed>>

\* Producer p writes slotVal and publishes `seq[idx] := ticket + 1` with release semantics.
ProdPublish(p) ==
    /\ pState[p] = "Claimed"
    /\ LET pos == pTicket[p]
           idx == Idx(pos)
       IN
       /\ slotVal' = [slotVal EXCEPT ![idx] = pos + 1]
       /\ seq' = [seq EXCEPT ![idx] = pos + 1]
       /\ pState' = [pState EXCEPT ![p] = "Idle"]
       /\ nextVal' = nextVal + 1
       /\ UNCHANGED <<enqueuePos, dequeuePos, pTicket, consumed>>

\* Single consumer observes `seq[Idx(dequeuePos)] == dequeuePos + 1` (acquire),
\* reads `slotVal`, advances `seq[idx] := dequeuePos + Capacity` (release), and bumps dequeuePos.
ConsPop ==
    LET pos == dequeuePos
        idx == Idx(pos)
    IN
    /\ seq[idx] = pos + 1
    /\ consumed' = consumed \cup {slotVal[idx]}
    /\ seq' = [seq EXCEPT ![idx] = pos + Capacity]
    /\ dequeuePos' = pos + 1
    /\ UNCHANGED <<enqueuePos, slotVal, pState, pTicket, nextVal>>

Next ==
    \/ (\E p \in Producers : ProdClaim(p) \/ ProdPublish(p))
    \/ ConsPop
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

TypeInvariant ==
    /\ enqueuePos \in Nat
    /\ dequeuePos \in Nat
    /\ \A i \in 0..(Capacity - 1) : seq[i] \in Nat

\* Two producers in the "Claimed" state can never hold the same ring slot simultaneously.
UniqueSlotOwnership ==
    \A p1, p2 \in Producers :
        (p1 # p2 /\ pState[p1] = "Claimed" /\ pState[p2] = "Claimed")
            => Idx(pTicket[p1]) # Idx(pTicket[p2])

\* Consumer never overtakes producer enqueue tickets, and active writers stay strictly within Capacity.
SequenceSafety ==
    /\ dequeuePos <= enqueuePos
    /\ \A p \in Producers :
         pState[p] = "Claimed" =>
            (pTicket[p] >= dequeuePos /\ (pTicket[p] - dequeuePos) < Capacity)

\* Every consumed value corresponds to a unique previously claimed ticket (1..enqueuePos).
NoSpuriousOrDuplicateItems ==
    /\ Cardinality(consumed) = dequeuePos
    /\ \A v \in consumed : v >= 1 /\ v <= enqueuePos

=============================================================================
