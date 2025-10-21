#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"

#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Use: ./servidor <porta>");
        return 1;
    }

    int port = atoi(argv[1]);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    packet pkt;

    //cria socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("falha em criar o socket");
        exit(EXIT_FAILURE);
    }

    //zera estruturas de endereço
    memset(&server_addr, 0, sizeof(server_addr));
    memset(&client_addr, 0, sizeof(client_addr));

    //configura endereço do servidor
    server_addr.sin_family = AF_INET; //IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; //escuta em todas as interfaces de rede disponíveis
    server_addr.sin_port = htons(port); //converte a porta para o formato de rede

    // vincula o socket ao endereço e porta
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("falha no bind");
        exit(EXIT_FAILURE);
    }

    printf("Servidor UDP escutando na porta %d\n", port);

    // recebendo mensagens
    while (1) {
        socklen_t len = sizeof(client_addr);
        int n = recvfrom(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&client_addr, &len);

        if (n > 0) {
            if (pkt.type == TYPE_DESCOBERTA) {
                printf("Pedido de descoberta recebido de %s\n", inet_ntoa(client_addr.sin_addr));
            
                packet ack_pkt;
                ack_pkt.type = TYPE_ACK_DESCOBERTA;
                
                sendto(sockfd, &ack_pkt, sizeof(packet), 0, (const struct sockaddr *)&client_addr, len);
            }
        }
    }

    close(sockfd);
    return 0;
}