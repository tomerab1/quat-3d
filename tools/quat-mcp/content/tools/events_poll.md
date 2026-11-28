Read what collided with what: the engine's gameplay event feed. Each event is `{seq, kind, time_s, entity_a, entity_b, name_a?, name_b?, point?, normal?, impulse?}`:

- `contact_added`: two bodies started touching. It carries the world `point`, the `normal` (from a towards b) and `impulse`, the estimated impact in N·s (mass × speed change); a hard hit is large, a gentle touch near 0.
- `contact_removed`: they stopped touching, or one was removed. A body falling asleep does not end its contacts.
- `trigger_enter` / `trigger_exit`: a body or character entered or left a trigger volume (`collider.sensor`). `entity_a` is the trigger.

`entity_a` / `entity_b` are null for bodies no entity owns (terrain). Events come only from a running simulation (`sim_play`).

Read it incrementally. Pass the `cursor` from the previous reply as `since` to get only new events. `more: true` means call again from `cursor`. `dropped` counts events you can no longer read because the ring (`capacity` events) overwrote them or the physics step had too many to record. Filter with `kinds` and with `entity` (id or name, matching either side).

A typical loop: `events_poll` (note the cursor), `debug_step` some frames, then `events_poll {since: cursor}` to see exactly which contacts that time produced.
