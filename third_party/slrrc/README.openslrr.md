# Vendored SLRR script compiler

Source: `https://github.com/TrentonCh/slrr-reverse`,
`engine/Core/Jvm/Compiler`, revision
`fe398dfa59c85c85a241ce16e0ed6dcf78f6eca3`.

The original MIT license is included as `LICENSE`. Compiler sources are copied
without functional changes. The host adapter is `src/jvm/script_compiler.cpp`.
This is a TUFA/TREE compiler, not javac. Compile the library as C++17 with
precise floating-point arithmetic; run the literal self-test before emission.
