Fixtures for `layering_check_detects_violation` (tests/sdk/layering_negative_test.cmake).
Each file here breaks a layering rule on purpose; `cmake/CheckLayering.cmake`
skips this directory when it scans the real tree.
