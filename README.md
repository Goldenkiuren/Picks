# picks — Servidor UDP (INF01151 Etapa 1)


Implementação em C de um serviço distribuído de transferências (estilo PIX) com:
- **Servidor UDP** (descoberta + processamento + logs no formato exigido)
- **Lógica pura** testável (ordem/duplicata/saldo)
- **Testes unitários** com cmocka


## Pré-requisitos (WSL/Ubuntu)
```bash
sudo apt update
sudo apt install -y build-essential cmocka-dev
```


## Compilar servidor
```bash
make servidor
./servidor 4000
```


## Rodar testes unitários
```bash
make tests
```


## Estrutura
- `include/`: headers públicos (state, logic, proto, servidor)
- `src/`: implementações
- `tests/`: testes unitários
- `Makefile`: build e execução de testes