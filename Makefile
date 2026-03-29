# AutismLang Makefile v0.9.0 - x86_64 ASM backend
CC     = gcc
AS     = as
LD     = ld
AUTISM = ./autism
FILE   ?= examples/hello
NAME   = $(notdir $(FILE))
SRC    = $(FILE).aut
_ASM   = build/$(NAME).s
_OBJ   = build/$(NAME).o
_EXE   = build/$(NAME)

# ASM backend v1 tests (int, bool, str, arithmetic, control flow, functions)
TEST_SUCCESS = arithmetic_precedence typing_valid function_return else_if_bool while_break_continue range_native asm_nop_valid
TEST_FAIL    = type_error_add comparison_type_error typing_invalid_reassign typing_invalid_add asm_non_string struct_invalid_decl

.PHONY: all run compiler test test-suite version clean

all: $(_EXE)
	@echo Done: $(_EXE)

run: $(_EXE)
	@echo Running $(_EXE)...
	$(_EXE)

$(_ASM): $(SRC)
	mkdir -p build
	$(AUTISM) $(SRC) -o $(_ASM)

$(_OBJ): $(_ASM)
	$(AS) --64 $(_ASM) -o $(_OBJ)

$(_EXE): $(_OBJ)
	$(LD) $(_OBJ) -o $(_EXE)

compiler:
	$(CC) -O2 -o autism autism.c
	@echo Compiler rebuilt: autism

test: compiler
	@echo Running tests...
	@$(MAKE) --no-print-directory test-suite

test-suite: $(addprefix test-success-,$(TEST_SUCCESS)) $(addprefix test-fail-,$(TEST_FAIL))
	@echo All tests passed.

build/tests:
	mkdir -p build/tests

build/tests/%.s: tests/cases/%.aut | build/tests
	$(AUTISM) $< -o $@

build/tests/%.o: build/tests/%.s
	$(AS) --64 $< -o $@

build/tests/%: build/tests/%.o
	$(LD) $< -o $@

test-success-%: build/tests/% tests/expected/%.out
	@./build/tests/$* > build/tests/$*.actual 2>&1
	@tr -d '\r' < build/tests/$*.actual > build/tests/$*.actual.tmp
	@tr -d '\r' < tests/expected/$*.out > build/tests/$*.expected.tmp
	@diff -u build/tests/$*.expected.tmp build/tests/$*.actual.tmp
	@echo PASS: $*

test-fail-%: tests/cases/%.aut | build/tests
	@set +e; ./autism $< -o build/tests/$*.s > build/tests/$*.actual 2>&1; code=$$?; set -e; \
	if [ $$code -eq 0 ]; then echo "Test failed: expected non-zero exit"; cat build/tests/$*.actual; exit 1; fi; \
	if ! grep -E "TypeError|UnsafeError" build/tests/$*.actual > /dev/null; then echo "Test failed: expected TypeError or UnsafeError"; cat build/tests/$*.actual; exit 1; fi
	@echo PASS: $*

version: compiler
	./autism --version

clean:
	rm -rf build
	@echo Cleaned.