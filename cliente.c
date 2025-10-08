#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1" //localhost

int main(int argc, char *argv[]) {
    
    if (argc != 2) {
        fprintf(stderr, "Use: ./cliente <porta>\n");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in servidor_addr;
    const char *hello_message = "Olá, servidor! Sou o cliente.";

    //criando socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha na criação do socket.");
        exit(EXIT_FAILURE);
    }

    memset(&servidor_addr, 0, sizeof(servidor_addr));

    servidor_addr.sin_family = AF_INET;
    servidor_addr.sin_port = htons(port);

    // configurando o endereço do servidor
    if (inet_aton(SERVER_IP, &servidor_addr.sin_addr) == 0) {
        fprintf(stderr, "Endereco IP invalido\n");
        exit(EXIT_FAILURE);
    }

    // enviando mensagem
    
    sendto(sockfd, hello_message, strlen(hello_message), 0, (const struct sockaddr *)&servidor_addr, sizeof(servidor_addr));
    
    printf("Mensagem enviada para o servidor.\n");

    close(sockfd);
    return 0;
}