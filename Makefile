# Makefile — build do servidor e testes (cmocka)
# Uso:
#   make servidor     # compila o servidor
#   make tests        # compila e executa os testes
#   make clean

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -pthread -Iinclude -I/usr/local/include
LDLIBS  ?= -L/usr/local/lib -lcmocka

SRC = src/state.c src/logic.c src/proto.c
OBJ = $(SRC:.c=.o)

# ===============================
# Servidor
# ===============================
servidor: $(OBJ) src/servidor.c
	$(CC) $(CFLAGS) -o $@ $^

# ===============================
# Testes unitários (cmocka)
# ===============================
TEST_EXE = tests/test_logic

$(TEST_EXE): tests/test_logic.c $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

.PHONY: tests
tests: $(TEST_EXE)
	./$(TEST_EXE)

.PHONY: clean
clean:
	rm -f $(OBJ) servidor $(TEST_EXE)
