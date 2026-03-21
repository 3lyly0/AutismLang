# AutismLang (Bootstrap v0)

AutismLang is a new low-level language project intended to build **AutismOS**.
This repository currently contains a bootstrap compiler written in C with Python-like syntax for function layout.

## Current Syntax

```aut
fn main():
    msg = "hi"
    year = 2026
    print(msg)
    print(year)
    print("Welcome to " + "AutismLang")
```

## Implemented Features (v0)

- Function declaration with `fn name():`
- Required entry point: `fn main():`
- Indentation-based function body (4 spaces)
- Variable assignment with Python-like style: `name = expression`
- Expression terms: string literals, integer literals, variable references
- `+` operator for `int + int` and `str + str`
- `print(expression)` inside functions
- Compiles to x86_64 Linux NASM assembly

## Quick Start

1. Build compiler (Linux/macOS with gcc or clang):

```bash
gcc autismc.c -O2 -Wall -Wextra -std=c11 -o autismc
```

2. Compile source:

```bash
./autismc examples/hello.aut -o build/hello.asm
```

3. (Optional) Assemble and link on Linux:

```bash
nasm -felf64 build/hello.asm -o build/hello.o
ld build/hello.o -o build/hello
./build/hello
```

### Windows Notes

- Build with MSVC Developer Command Prompt:

```powershell
cl /O2 /W4 /std:c11 autismc.c /Fe:autismc.exe
```

- Then run:

```powershell
.\autismc.exe examples\hello.aut -o build\hello.asm
```

## Next Milestones

- Runtime expression evaluation (instead of compile-time folding)
- `if`, `while`
- Function calls with parameters
- Backend for bare metal target (AutismOS)
