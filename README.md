# AutismLang (v0.6.0)

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

## Implemented Features (v0.6.0)

- Function declaration with `fn name():`
- Function parameters require explicit types: `fn add(int a, int b):`
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
- Controlled raw pointer access via `unsafe:` blocks
- Inline assembly: `asm("...")` (string literal only)
- Port I/O primitives: `out(port, value)` and `in(port)`
- Volatile typed pointers: `volatile ptr<T> name`
- Command-line options: `--help`, `--version`, `--metadata`

## Native Range Syntax

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
    ptr<void> x = alloc(8)    # Allocate bytes, generic pointer
    ptr<int> xi = ptr<int>(x) # Cast to typed pointer before dereference
    unsafe:
        *xi = 42        # Dereference and assign
        print(*xi)      # Dereference and read (prints 42)
    free(x)             # Free allocated memory
```

### Address-Of Operator

```aut
fn main():
    int y = 100
    ptr<int> p = &y     # Get typed address of variable
    unsafe:
        print(*p)       # Dereference (prints 100)
```

### Pointer Casting

```aut
fn main():
    # Cast integer to pointer (for memory-mapped I/O)
    ptr<void> vga = ptr<void>(0xB8000)   # VGA text buffer address
    
    # Cast pointer to integer
    int addr = int(p)
    addr = addr + 8
    ptr<void> p2 = ptr<void>(addr)
```

### Available Pointer Operations

| Operation | Description |
|-----------|-------------|
| `ptr<void> x = alloc(size)` | Allocate memory, returns generic pointer |
| `free(ptr)` | Free allocated memory |
| `unsafe: *expr` | Dereference pointer (read/write) only in unsafe block |
| `&var` | Get address of variable |
| `asm("...")` | Emit inline GCC assembly (unsafe block only) |
| `out(port, value)` | Write 8-bit value to I/O port (unsafe block only) |
| `in(port)` | Read 8-bit value from I/O port (unsafe block only) |
| `volatile ptr<T> p` | Declare volatile pointer for MMIO |
| `ptr<T>(value)` | Cast integer/pointer to typed pointer |
| `int(ptr_val)` | Cast pointer to integer |
| `null` / `NULL` | Null pointer constant |

### Unsafe Rules

- Dereference outside `unsafe:` fails with `UnsafeError`
- Dereferencing `ptr<void>` is always forbidden
- Cast `ptr<void>` to a concrete pointer type first (for example `ptr<int>(raw)`)

### Hexadecimal Literals

Integer literals support hexadecimal notation for memory addresses:

```aut
ptr<void> vga = ptr<void>(0xB8000)   # VGA text buffer
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

## Kernel/Freestanding Output

Use no-runtime mode to emit C without libc headers and without built-in runtime helpers:

```bash
./autism examples/hello.aut --no-runtime -o build/hello_kernel.c
```

In this mode, generated C:

- keeps string values as static `const char*` literals
- does not emit heap string helpers (no `strdup`, no `aut_copy`)
- does not include `<stdio.h>`, `<stdlib.h>`, or `<string.h>`
- calls external hooks you provide:

```c
void aut_print_i64(long long value);
void aut_print_str(const char* value);
void* aut_alloc(unsigned long long size);
void aut_free(void* ptr);
```

Entry symbol is `aut_entry()` instead of `main()` in no-runtime mode.

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

### v0.6.0 (Current)
- **Unsafe block model**: explicit `unsafe:` block required for pointer dereference
- **Compile-time UnsafeError**: dereference outside unsafe is rejected
- **Strict raw-pointer rule**: `ptr<void>` cannot be dereferenced directly
- **Typed cast workflow**: explicit cast from `ptr<void>` to `ptr<T>` before dereference

### v0.5.0
- **Typed pointer system**: `ptr<T>` declarations and `ptr<T>(value)` casts
- **Strict static typing**: compile-time checks across `int`, `bool`, `str`, and pointer types
- **Pointer safety checks**: typed dereference/assignment validation and pointer arithmetic rules
- **Kernel/freestanding mode**: `--no-runtime` output with `aut_entry()` and external hooks
- **Runtime minimization**: static string literals and no heap string helper runtime in no-runtime mode

### Previous Range Release
- **Native range syntax**: `for i in 0..10:` (exclusive)
- **Inclusive range**: `for i in 0..=10:` (inclusive end)
- **Step specification**: `for i in 0..10..2:` (with custom step)
- **Reverse ranges**: `for i in 10..0..-1:` (counting down)
- **Auto step inference**: Step is inferred from start/end comparison
- **Removed `range()` function**: Use native `..` syntax instead

### v0.3.0
- Pointer operations: `alloc()`, `free()`, `*` (dereference), `&` (address-of)
- Pointer type annotation: `ptr<T>` (for example `ptr<int>`, `ptr<void>`)
- Pointer casting: `ptr<T>(value)`, `int(ptr)`
- Hexadecimal integer literals: `0xB8000`
- Null pointer: `null`, `NULL`

### v0.2.0
- Static typing with type inference
- Type annotations: `int x = 5`, `bool flag = true`, `ptr p = null`, `str s = "hi"`
- String type support (`str`, static `const char*` mapping)

### v0.1.0
- Basic function definitions
- Variables and arithmetic
- Control flow: if/else, while, for
- Print statements