Replace an existing material shader's source and recompile it; every entity using it switches to the new version on the next frame (no restart). Send the complete module, not a diff — get the current text with `shader_get`.

The call returns when the compile has finished. On failure the result lists the compiler messages as `file:line:column: error[code]: message` with the source line and a caret, and the previous version is restored on disk and keeps rendering — fix the reported lines and call `shader_update` again. Line numbers refer to the source you sent.
