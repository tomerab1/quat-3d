Replace an existing material shader's source and recompile it; every entity using it switches to the new version on the next frame (no restart). Send the complete module, not a diff — get the current text with `shader_get`.

The call returns when the compile has finished. On failure the result lists the compiler messages as `file:line:column: error[code]: message` with the source line and a caret, and the previous version is restored on disk and keeps rendering — fix the reported lines and call `shader_update` again. Line numbers refer to the source you sent.

The same source rules as `shader_create` apply: no preprocessor or path imports, no inline SPIR-V or `vk::` intrinsics, no pointers or buffer device addresses. Every loop needs a compile-time bound; endless loops such as `for (;;)` and `while (true)` are refused. A refused source changes nothing.
