------------------------------ MODULE SpscRing ------------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS
    Capacity,   \* Power-of-two ring capacity (e.g. 4)
    MaxItems    \* Upper bound on sequence numbers for finite TLC model checking

ASSUME Capacity \in Nat /\ Capacity >= 2
ASSUME MaxItems \in Nat /\ MaxItems >= 1

VARIABLES
    buf,         \* Array [0..Capacity-1] -> Int
    head,        \* Atomic release-published producer index
    tail,        \* Atomic release-published consumer index
    cachedTail,  \* Producer-private shadow copy of tail
    cachedHead,  \* Consumer-private shadow copy of head
    prodState,   \* "Idle" | "Reserved"
    consState,   \* "Idle" | "Peeking"
    prodSeq,     \* Next sequence number to produce
    poppedSeq    \* Sequence of consumed values (verifies strict FIFO order)

vars == <<buf, head, tail, cachedTail, cachedHead, prodState, consState, prodSeq, poppedSeq>>

Idx(pos) == pos % Capacity

Init ==
    /\ buf = [i \in 0..(Capacity - 1) |-> 0]
    /\ head = 0
    /\ tail = 0
    /\ cachedTail = 0
    /\ cachedHead = 0
    /\ prodState = "Idle"
    /\ consState = "Idle"
    /\ prodSeq = 1
    /\ poppedSeq = <<>>

\* Producer refreshes its shadow bound when ring appears full.
ProdRefreshAndReserve ==
    /\ prodState = "Idle"
    /\ prodSeq <= MaxItems
    /\ LET refreshedTail ==
           IF (head - cachedTail) < Capacity THEN cachedTail ELSE tail
       IN
       /\ (head - refreshedTail) < Capacity
       /\ cachedTail' = refreshedTail
       /\ buf' = [buf EXCEPT ![Idx(head)] = prodSeq]
       /\ prodState' = "Reserved"
       /\ UNCHANGED <<head, tail, cachedHead, consState, prodSeq, poppedSeq>>

\* Producer publishes slot write via release store on `head`.
ProdCommit ==
    /\ prodState = "Reserved"
    /\ head' = head + 1
    /\ prodSeq' = prodSeq + 1
    /\ prodState' = "Idle"
    /\ UNCHANGED <<buf, tail, cachedTail, cachedHead, consState, poppedSeq>>

\* Consumer refreshes its shadow bound if empty and peeks slot before release-updating `tail`.
ConsPeek ==
    /\ consState = "Idle"
    /\ LET refreshedHead ==
           IF tail < cachedHead THEN cachedHead ELSE head
       IN
       /\ tail < refreshedHead
       /\ cachedHead' = refreshedHead
       /\ consState' = "Peeking"
       /\ poppedSeq' = Append(poppedSeq, buf[Idx(tail)])
       /\ UNCHANGED <<buf, head, tail, cachedTail, prodState, prodSeq>>

\* Consumer finishes reading and updates `tail` via release store.
ConsConsume ==
    /\ consState = "Peeking"
    /\ tail' = tail + 1
    /\ consState' = "Idle"
    /\ UNCHANGED <<buf, head, cachedTail, cachedHead, prodState, prodSeq, poppedSeq>>

Next ==
    \/ ProdRefreshAndReserve
    \/ ProdCommit
    \/ ConsPeek
    \/ ConsConsume
    \/ UNCHANGED vars

Spec == Init /\ [][Next]_vars

TypeInvariant ==
    /\ head \in Nat
    /\ tail \in Nat
    /\ cachedTail \in Nat
    /\ cachedHead \in Nat
    /\ prodState \in {"Idle", "Reserved"}
    /\ consState \in {"Idle", "Peeking"}

BoundsInvariant ==
    /\ tail <= head
    /\ (head - tail) <= Capacity

ShadowMonotonicity ==
    /\ cachedTail <= tail
    /\ tail <= cachedHead
    /\ cachedHead <= head

\* Mutual exclusion on the active slot: if producer is writing `Idx(head)` and consumer is
\* reading `Idx(tail)`, their slot indices cannot alias.
NoDataRace ==
    (prodState = "Reserved" /\ consState = "Peeking") => (Idx(head) # Idx(tail))

\* Every consumed value matches strict 1..Len(poppedSeq) FIFO order with zero loss or duplication.
FifoOrder ==
    \A i \in 1..Len(poppedSeq) : poppedSeq[i] = i

=============================================================================
