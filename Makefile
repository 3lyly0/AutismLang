# AutismLang Makefile v0.4.0 - C backend
CC     = gcc
AUTISM = ./autism
FILE   ?= examples/hello
NAME   = $(notdir $(FILE))
SRC    = $(FILE).aut
_C     = build/$(NAME).c
_EXE   = build/$(NAME)
TEST_SUCCESS = else_if_bool arithmetic_precedence while_break_continue function_return range_native
TEST_FAIL    = type_error_add comparison_type_error

.PHONY: all run compiler test test-suite version clean

all: $(_EXE)
	@echo Done: $(_EXE)

run: $(_EXE)
	@echo Running $(_EXE)...
	$(_EXE)

$(_C): $(SRC)
	mkdir -p build
	$(AUTISM) $(SRC) -o $(_C)

$(_EXE): $(_C)
	$(CC) -O2 $(_C) -o $(_EXE)

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

build/tests/%.c: tests/cases/%.aut | build/tests
	$(AUTISM) $< -o $@

build/tests/%: build/tests/%.c
	$(CC) -O2 $< -o $@

test-success-%: build/tests/% tests/expected/%.out
	@./build/tests/$* > build/tests/$*.actual 2>&1
	@diff -u tests/expected/$*.out build/tests/$*.actual
	@echo PASS: $*

test-fail-%: tests/cases/%.aut | build/tests
	@set +e; ./autism $< -o build/tests/$*.c > build/tests/$*.actual 2>&1; code=$$?; set -e; \
	if [ $$code -eq 0 ]; then echo "Test failed: expected non-zero exit"; cat build/tests/$*.actual; exit 1; fi; \
	if ! grep -F "TypeError" build/tests/$*.actual > /dev/null; then echo "Test failed: expected TypeError"; cat build/tests/$*.actual; exit 1; fi
	@echo PASS: $*

version: compiler
	./autism --version

clean:
	rm -rf build
	@echo Cleaned.