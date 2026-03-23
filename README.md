# AutismLang (Bootstrap v0.1.0)

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

## Implemented Features (v0.1.0)

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
- **NEW: `for var in range(...):` loops**
- Function parameters and calls, for example: `fn greet(name): ...` then `greet("Neo")`
- `return expression` from functions (default return is `0` when omitted)
- Function calls inside expressions, for example: `x = add(2, 3) * 4`
- `break` and `continue` inside `while` and `for` loops
- Boolean literals: `True`, `False` (also `true`, `false`)
- Builtin `input()` / `input("prompt")` (always returns string)
- **NEW: `int(expression)` - converts string to integer**
- **NEW: `str(expression)` - converts integer to string**
- Inline comments using `#` at the end of code lines
- Condition operators: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Strict runtime type checks for arithmetic/comparisons
- CLI flags: `--help`, `--version`, `--metadata`

## NEW: For-In-Range Loop

```aut
fn main():
    # Range with stop only (0 to 4)
    for i in range(5):
        print(i)
    
    # Range with start and stop (2 to 6)
    for i in range(2, 7):
        print(i)
    
    # Range with step (0, 2, 4, 6, 8)
    for i in range(0, 10, 2):
        print(i)
    
    # Negative step (counting backwards)
    for i in range(10, 0, -1):
        print(i)
```

## NEW: Type Conversion

```aut
fn main():
    # String to integer
    s = "42"
    n = int(s)
    print(n + 10)  # prints 52
    
    # Integer to string
    x = 100
    msg = "Value: " + str(x)
    print(msg)  # prints "Value: 100"
    
    # With user input
    user_input = input("Enter number: ")
    num = int(user_input)
    print(num * 2)
```

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

- Data structures (arrays/lists)
- Logical operators (`and`, `or`, `not`)
- Native ASM backend (future milestone)