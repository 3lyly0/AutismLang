# AutismLang (Bootstrap v0.0.1)

AutismLang is a new low-level language project intended to build **AutismOS**.
This repository currently contains a bootstrap compiler written in C with Python-like syntax for function layout.
Current backend emits C code, then compiles it with GCC.

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
- `else if condition:` chained branching
- `while condition:` loops
- Function parameters and calls, for example: `fn greet(name): ...` then `greet("Neo")`
- `return expression` from functions (default return is `0` when omitted)
- Function calls inside expressions, for example: `x = add(2, 3) * 4`
- `break` and `continue` inside `while` loops
- Boolean literals: `True`, `False` (also `true`, `false`)
- Builtin `input()` / `input("prompt")` (always returns string)
- Inline comments using `#` at the end of code lines
- Condition operators: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Strict runtime type checks for arithmetic/comparisons
- CLI flags: `--help`, `--version`, `--metadata`

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
gcc autism.c -O2 -Wall -Wextra -std=c11 -o autism
```

2. Compile source:

```bash
./autism examples/hello.aut -o build/hello.c
```

3. Build executable from generated C:

```bash
gcc -O2 build/hello.c -o build/hello
./build/hello
```

4. Run tests:

```bash
make test
```

## Version Metadata

- Semantic version is stored in `VERSION.json`
- Compiler prints version with:

```bash
./autism --version
```

- Compiler prints machine-readable metadata with:

```bash
./autism --metadata
```

## About ASM Output

The active backend in `autism.c` currently emits C, not NASM.
If you need assembly today, generate C first then ask GCC to emit assembly:

```bash
./autism examples/hello.aut -o build/hello.c
gcc -S -masm=intel build/hello.c -o build/hello.s
```

## Automation (GitHub Actions)

- CI runs automatically on every push and pull request across:
  - Linux (GCC)
  - macOS (Clang)
  - Windows (MSYS2 + GCC)
- Release workflow runs on tags like `v0.1.0` and publishes:
  - Linux archive
  - macOS archive
  - Windows archive
- GitHub Release notes are generated automatically from merged PRs.

### Release Flow

1. Update `VERSION.json`
2. Commit and push
3. Create and push tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

4. GitHub Actions will build all release binaries and create the release automatically.

## Next Milestones

- `for ... in range(...)`
- Builtin conversion helpers (`int()`, `str()`)
- Data structures (planned later)
- Native ASM backend (future milestone)
