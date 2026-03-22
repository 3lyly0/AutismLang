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
    if year == 2026:
        print("ready for AutismOS")
    else:
        print("not ready")
```

## Implemented Features (v0)

- Function declaration with `fn name():`
- Required entry point: `fn main():`
- Indentation-based function body (4 spaces)
- Variable assignment with Python-like style: `name = expression`
- Expression terms: string literals, integer literals, variable references
- Arithmetic for integers: `+`, `-`, `*`, `/`
- Parentheses in expressions: `( ... )`
- `+` also supports string concatenation (`str + str`)
- `print(expression)` inside functions
- `if condition:` blocks (single-level indentation inside functions)
- `else:` blocks after `if`
- `while condition:` loops
- Function parameters and calls, for example: `fn greet(name): ...` then `greet("Neo")`
- `return expression` from functions (default return is `0` when omitted)
- Function calls inside expressions, for example: `x = add(2, 3) * 4`
- `break` and `continue` inside `while` loops
- Builtin `input()` / `input("prompt")` (compile-time input)
- Inline comments using `#` at the end of code lines
- Condition operators: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Compiles to x86_64 Linux NASM assembly

Example arithmetic behavior:

```aut
fn main():
    a = 10 + 5 * 2      # 20
    b = (10 + 5) * 2    # 30
    print(a)
    print(b)
```

Example functions + while + else:

```aut
fn greet(name, times):
    i = 0
    while i < times:
        print("hello " + name)
        i = i + 1

fn main():
    year = 2026
    if year == 2027:
        print("wrong")
    else:
        print("ok")
    greet("AutismLang", 2)
```

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

- One-command native Windows compile + run (no GCC in link/run):

```powershell
$ldBin = Split-Path (Get-Command ld).Source -Parent; $ldLib = Join-Path (Split-Path $ldBin -Parent) "lib"; .\autismc.exe examples\hello.aut -o build\hello.asm; if ($LASTEXITCODE -eq 0) { nasm -f win64 build\hello.asm -o build\hello.obj; if ($LASTEXITCODE -eq 0) { ld build\hello.obj -o build\hello.exe -e _start --subsystem console -L $ldLib -lkernel32; if ($LASTEXITCODE -eq 0) { .\build\hello.exe } } }
```

## Next Milestones

- Runtime expression evaluation (instead of compile-time folding)
- `if`, `while`
- Function calls with parameters
- Backend for bare metal target (AutismOS)
