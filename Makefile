# AutismLang Makefile - C backend
CC     = gcc
AUTISM = .\autism.exe
FILE   ?= examples/hello
NAME   = $(notdir $(FILE))
SRC    = $(FILE).aut
_C     = build/$(NAME).c
_EXE   = build/$(NAME).exe

.PHONY: all run compiler clean

all: $(_EXE)
	@echo Done: $(_EXE)

run: $(_EXE)
	@echo Running $(_EXE)...
	$(_EXE)

$(_C): $(SRC)
	-mkdir build 2>nul
	$(AUTISM) $(SRC) -o $(_C)

$(_EXE): $(_C)
	$(CC) -O2 $(_C) -o $(_EXE)

compiler:
	$(CC) -O2 -o autism.exe autism.c
	@echo Compiler rebuilt: autism.exe

clean:
	-rmdir /s /q build 2>nul
	@echo Cleaned.