# AutismLang (v0.3.0)

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

## Implemented Features (v0.3.0)

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
- `for var in range(...):` loops
- Function parameters and calls, for example: `fn greet(name): ...` then `greet("Neo")`
- `return expression` from functions (default return is `0` when omitted)
- Function calls inside expressions, for example: `x = add(2, 3) * 4`
- `break` and `continue` inside `while` and `for` loops
- Boolean literals: `True`, `False` (also `true`, `false`)
- Builtin `input()` / `input("prompt")` (always returns string)
- `int(expression)` - converts string to integer
- `str(expression)` - converts integer to string
- Inline comments using `#` at the end of code lines
- Condition operators: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Strict runtime type checks for arithmetic/comparisons
- CLI flags: `--help`, `--version`, `--metadata`

## NEW: Pointer & Memory Management (v0.3.0)

AutismLang now supports low-level pointer operations for OS development:

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

Integer literals now support hexadecimal notation for memory addresses:

```aut
fn main():
    ptr vga = ptr(0xB8000)    # VGA text mode buffer
    ptr bios = ptr(0xFFFF0)   # BIOS entry point
```

## For-In-Range Loop

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

## Type Conversion

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
