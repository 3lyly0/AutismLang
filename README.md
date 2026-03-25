# AutismLang (v0.4.0)

AutismLang is a new low-level language project intended to build **AutismOS**.
This repository contains a bootstrap compiler written in C with Python-like syntax for function layout.
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

## Implemented Features (v0.4.0)

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
- `for var in range:` loops with native range syntax
- Function parameters and calls
- Command-line options: `--help`, `--version`, `--metadata`

## NEW: Native Range Syntax (v0.4.0)

AutismLang now supports a clean, expressive range syntax for loops:

### Exclusive Range (`..`)

```aut
fn main():
    # Iterates 0, 1, 2, ..., 9 (10 is excluded)
    for i in 0..10:
        print(i)
```

### Inclusive Range (`..=`)

```aut
fn main():
    # Iterates 0, 1, 2, 3, 4, 5 (5 is included)
    for i in 0..=5:
        print(i)
```

### Range with Step (`..` twice)

```aut
fn main():
    # Iterates 0, 2, 4, 6, 8 (step of 2)
    for i in 0..10..2:
        print(i)
```

### Reverse Range

```aut
fn main():
    # Iterates 10, 9, 8, 7, ..., 1, 0
    for i in 10..0..-1:
        print(i)
```

### Range Features

| Syntax | Description | Example |
|--------|-------------|---------|
| `a..b` | Exclusive end, auto step | `0..10` → 0,1,...,9 |
| `a..=b` | Inclusive end, auto step | `0..=5` → 0,1,...,5 |
| `a..b..s` | Explicit step | `0..10..2` → 0,2,4,6,8 |
| Auto step | Inferred +1 or -1 | `10..0` → step=-1 |

### Step Inference Rules

- If `start <= end`: step is `+1` (or explicit step)
- If `start > end`: step is `-1` (or explicit step)
- Explicit step always overrides inference

## Pointer & Memory Management (v0.3.0)

AutismLang supports low-level pointer operations for OS development:

### Pointer Type Annotation

```aut
fn main():
    ptr x = alloc(8)    # Allocate 8 bytes, returns pointer
    *x = 42             # Dereference and assign
    print(*x)           # Dereference and read (prints 42)
    free(x)             # Free allocated memory
```

### Address-Of Operator

```aut
fn main():
    int y = 100
    ptr p = &y          # Get address of variable
    print(*p)           # Dereference (prints 100)
```

### Pointer Casting

```aut
fn main():
    # Cast integer to pointer (for memory-mapped I/O)
    ptr vga = ptr(0xB8000)   # VGA text buffer address
    
    # Cast pointer to integer
    int addr = int(p)
    addr = addr + 8
    ptr p2 = ptr(addr)
```

### Available Pointer Operations

| Operation | Description |
|-----------|-------------|
| `ptr x = alloc(size)` | Allocate memory, returns pointer |
| `free(ptr)` | Free allocated memory |
| `*expr` | Dereference pointer (read/write) |
| `&var` | Get address of variable |
| `ptr(int_val)` | Cast integer to pointer |
| `int(ptr_val)` | Cast pointer to integer |
| `null` / `NULL` | Null pointer constant |

### Hexadecimal Literals

Integer literals support hexadecimal notation for memory addresses:

```aut
ptr vga = ptr(0xB8000)   # VGA text buffer
```

## Quick Start

1. Build compiler (Windows with gcc):

```bash
gcc autism.c -o autism.exe
```

2. Compile source:

```bash
./autism examples/hello.aut -o build/hello.c
```

3. Build executable from generated C:

```bash
gcc -O2 build/hello.c -o build/hello.exe
./build/hello.exe
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

## Changelog

### v0.4.0 (Current)
- **Native range syntax**: `for i in 0..10:` (exclusive)
- **Inclusive range**: `for i in 0..=10:` (inclusive end)
- **Step specification**: `for i in 0..10..2:` (with custom step)
- **Reverse ranges**: `for i in 10..0..-1:` (counting down)
- **Auto step inference**: Step is inferred from start/end comparison
- **Removed `range()` function**: Use native `..` syntax instead

### v0.3.0
- Pointer operations: `alloc()`, `free()`, `*` (dereference), `&` (address-of)
- Pointer type annotation: `ptr x = alloc(8)`
- Pointer casting: `ptr(int)`, `int(ptr)`
- Hexadecimal integer literals: `0xB8000`
- Null pointer: `null`, `NULL`

### v0.2.0
- Static typing with type inference
- Type annotations: `int x = 5`, `bool flag = true`
- String type support

### v0.1.0
- Basic function definitions
- Variables and arithmetic
- Control flow: if/else, while, for
- Print statements