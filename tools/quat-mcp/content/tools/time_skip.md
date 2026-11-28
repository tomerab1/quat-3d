Move the world clock forward or back by `hours` (negative goes back; up to ten years either way) and return the new state (as `time_get`). With `fixed_day` the time of day wraps within the same date.

Use it to jump between times of day while looking at the result with `screenshot`, e.g. `{"hours": 3}` from noon to mid-afternoon, or `{"hours": 24}` to see the next day's Moon phase. Fails if the scene has no world clock yet: call `time_set` first.
