Read the renderer's tunable settings — the same values the editor's Renderer panel shows. Returns `{group: {field: value}}` for every group, or only `group` when given: `exposure` (auto, compensation_ev, key, min_exposure, max_exposure, adaptation_s), `bloom` (enabled, threshold, knee, intensity, radius), `taa` (enabled, feedback), `shadows` (enabled, extent_m, depth_bias, slope_bias, pcf_radius), `ibl` (intensity), `clouds` (as `environment_get`).

Read this before changing anything, so you know the starting point; `render_settings_schema` gives each field's range, default and unit.
