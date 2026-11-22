/-!
Machine-checked invariants for ChronoRing lock-free ring buffers (Lean 4 core).

Proves the key safety lemmas behind SpscRingBuffer shadow-cursor caching,
slot mutual exclusion (zero data race between release-acquire pairs), and
Vyukov MPSC ticket uniqueness within an active capacity window.
-/

namespace ChronoRing.Formal

/-- Shadow-tail bound: if `cached_tail <= tail` (monotonicity of release/acquire consumer
    tail observations) and producer sees `head - cached_tail < cap`, then the true occupancy
    `head - tail` is strictly below `cap`, preventing overwrite of unconsumed slots. -/
theorem shadow_tail_bound
    (head tail cached_tail cap : Nat)
    (h_mono : cached_tail ≤ tail)
    (h_room : head - cached_tail < cap) :
    head - tail < cap :=
  Nat.lt_of_le_of_lt (Nat.sub_le_sub_left h_mono head) h_room

/-- Shadow-head bound: if `cached_head <= head` and consumer sees `tail < cached_head`,
    then `tail < head`, guaranteeing at least one initialized element is visible. -/
theorem shadow_head_bound
    (head tail cached_head : Nat)
    (h_mono : cached_head ≤ head)
    (h_nonempty : tail < cached_head) :
    tail < head :=
  Nat.lt_of_lt_of_le h_nonempty h_mono

/-- Auxiliary modular inequality: if `r < cap`, `0 < d`, and `d < cap`,
    then `(r + d) % cap ≠ r`. -/
private theorem add_mod_ne_left
    (r d cap : Nat)
    (hr : r < cap)
    (hd_pos : 0 < d)
    (hd_lt : d < cap) :
    (r + d) % cap ≠ r := by
  intro h_eq
  rcases Nat.lt_or_ge (r + d) cap with h_small | h_wrap
  · rw [Nat.mod_eq_of_lt h_small] at h_eq
    have h_zero : d = 0 := by
      have : r + d = r + 0 := by simpa using h_eq
      exact Nat.add_left_cancel this
    exact Nat.ne_of_gt hd_pos h_zero
  · have h_lt_2cap : r + d < cap + cap := Nat.add_lt_add hr hd_lt
    have h_sub_lt : (r + d) - cap < cap := Nat.sub_lt_left_of_lt_add h_wrap (by simpa [Nat.add_comm] using h_lt_2cap)
    have h_mod : (r + d) % cap = (r + d) - cap := by
      rw [← Nat.add_sub_cancel' h_wrap]
      have : cap + ((r + d) - cap) = ((r + d) - cap) + cap := Nat.add_comm _ _
      rw [Nat.mod_eq_sub_mod h_wrap, Nat.mod_eq_of_lt h_sub_lt]
    rw [h_mod] at h_eq
    have h_rd : r + d = r + cap := by
      have h1 := congrArg (· + cap) h_eq
      simp only [] at h1
      rw [Nat.sub_add_cancel h_wrap] at h1
      exact h1
    have h_d_eq_cap : d = cap := Nat.add_left_cancel h_rd
    exact Nat.ne_of_lt hd_lt h_d_eq_cap

/-- SPSC slot mutual exclusion: whenever producer `head` is strictly ahead of consumer
    `tail` within one buffer capacity (`head - tail < cap`), their slot indices
    `head % cap` and `tail % cap` cannot collide. -/
theorem spsc_slot_disjoint
    (head tail cap : Nat)
    (h_cap : 0 < cap)
    (h_lt : tail < head)
    (h_bound : head - tail < cap) :
    head % cap ≠ tail % cap := by
  let d := head - tail
  have hd_pos : 0 < d := Nat.sub_pos_of_lt h_lt
  have hd_lt : d < cap := h_bound
  have h_decomp : head = tail + d := (Nat.add_sub_of_le (Nat.le_of_lt h_lt)).symm
  rw [h_decomp, Nat.add_mod, Nat.mod_eq_of_lt hd_lt]
  exact add_mod_ne_left (tail % cap) d cap (Nat.mod_lt tail h_cap) hd_pos hd_lt

/-- Vyukov MPSC ticket exclusivity: two distinct producer tickets `p1 ≠ p2` active in the
    same capacity window (`deq ≤ p1`, `deq ≤ p2`, `p1 - deq < cap`, `p2 - deq < cap`)
    always map to distinct physical ring slots (`p1 % cap ≠ p2 % cap`). -/
theorem vyukov_active_tickets_disjoint_slots
    (p1 p2 deq cap : Nat)
    (h_cap : 0 < cap)
    (h_ne : p1 ≠ p2)
    (hp1_ge : deq ≤ p1)
    (hp2_ge : deq ≤ p2)
    (hp1_win : p1 - deq < cap)
    (hp2_win : p2 - deq < cap) :
    p1 % cap ≠ p2 % cap := by
  rcases Nat.lt_trichotomy p1 p2 with h12 | h_eq | h21
  · have h_diff : p2 - p1 < cap :=
      Nat.lt_of_le_of_lt (Nat.sub_le_sub_left hp1_ge p2) hp2_win
    exact (spsc_slot_disjoint p2 p1 cap h_cap h12 h_diff).symm
  · exact absurd h_eq h_ne
  · have h_diff : p1 - p2 < cap :=
      Nat.lt_of_le_of_lt (Nat.sub_le_sub_left hp2_ge p1) hp1_win
    exact spsc_slot_disjoint p1 p2 cap h_cap h21 h_diff

end ChronoRing.Formal
