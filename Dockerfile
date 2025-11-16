# 1. Comece com uma imagem base que tenha o compilador GCC e o 'make'
FROM gcc:latest

# 2. Defina o diretório de trabalho dentro do contêiner
WORKDIR /app

# 3. Copie TODOS os seus arquivos de código para dentro do contêiner
COPY . .

# 4. Execute o Makefile para compilar o projeto
# (Isso irá criar os binários 'servidor' e 'cliente')
RUN make

# 5. A seção 'command:' no docker-compose.yml cuidará de executar o binário
