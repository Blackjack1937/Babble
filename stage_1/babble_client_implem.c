#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "babble_client.h"
#include "babble_types.h"
#include "babble_communication.h"
#include "babble_utils.h"

void *recv_one_msg(int sock)
{
    unsigned int *nb_items = NULL;
    int recv_bytes = 0;

    if ((recv_bytes = network_recv(sock, (void **)&nb_items)) != sizeof(unsigned int))
    {
        fprintf(stderr, "ERROR in msg reception -- expected msg size -- received %d bytes\n", recv_bytes);
        free(nb_items); // Free memory in case of error
        return NULL;
    }

    if (*nb_items != 1)
    {
        fprintf(stderr, "ERROR in msg reception -- a single msg expected -- %d announced\n", *nb_items);
        free(nb_items);
        return NULL;
    }
    free(nb_items);

    char *msg = NULL;

    if (network_recv(sock, (void **)&msg) == -1)
    {
        perror("ERROR reading from socket");
        return NULL;
    }

    return msg;
}

int recv_timeline_msg_and_print(int sock, int silent)
{
    unsigned int *buf1 = NULL;
    unsigned int nb_items = 0;
    unsigned int timeline_size = 0;
    int recv_bytes = 0;

    if ((recv_bytes = network_recv(sock, (void **)&buf1)) != sizeof(unsigned int))
    {
        fprintf(stderr, "ERROR in msg reception -- expected msg size -- received %d bytes\n", recv_bytes);
        free(buf1);
        return -1;
    }
    nb_items = *buf1;
    free(buf1);

    /* first data is the number of msgs in the most recent timeline */
    if ((recv_bytes = network_recv(sock, (void **)&buf1)) != sizeof(unsigned int))
    {
        fprintf(stderr, "ERROR in msg reception -- expected timeline size -- received %d bytes\n", recv_bytes);
        free(buf1);
        return -1;
    }
    timeline_size = *buf1;
    free(buf1);

    nb_items--;

    /* receive each publication in the timeline */
    while (nb_items)
    {
        char *publi = NULL;

        if (network_recv(sock, (void **)&publi) == -1)
        {
            fprintf(stderr, "ERROR in receiving timeline publication\n");
            free(publi); // Ensure memory is freed on error
            return -1;
        }

        if (!silent)
        {
            printf("%s", publi);
        }

        free(publi);
        nb_items--;
    }

    return timeline_size;
}

int connect_to_server(char *host, int port)
{
    /* creating the socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("ERROR opening socket");
        return -1;
    }

    struct addrinfo hints, *results, *raddr;
    memset(&hints, 0, sizeof(struct addrinfo));
    char s_port[64];
    snprintf(s_port, sizeof(s_port), "%d", port);

    hints.ai_family = AF_INET;

    if (getaddrinfo(host, s_port, &hints, &results))
    {
        perror("getaddrinfo failed");
        close(sockfd);
        return -1;
    }

    for (raddr = results; raddr != NULL; raddr = raddr->ai_next)
    {
        if (connect(sockfd, raddr->ai_addr, raddr->ai_addrlen) != -1)
        {
            break;
        }
    }

    freeaddrinfo(results);

    if (raddr == NULL)
    { /* No address succeeded */
        fprintf(stderr, "Could not connect\n");
        close(sockfd);
        return -1;
    }

    /* set socket option to manage client disconnection */
    struct linger sl = {1, 0};
    if (setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl)) < 0)
    {
        perror("setsockopt failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

unsigned long client_login(int sock, char *id)
{
    char buffer[BABBLE_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    if (strlen(id) > BABBLE_ID_SIZE)
    {
        fprintf(stderr, "Error -- invalid client id (too long): %s\n", id);
        return 0;
    }

    snprintf(buffer, sizeof(buffer), "%d %s\n", LOGIN, id);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        close(sock);
        return 0;
    }

    char *login_ack = recv_one_msg(sock);
    if (!login_ack)
    {
        close(sock);
        return 0;
    }

    unsigned long key = parse_login_ack(login_ack);
    free(login_ack);

    return key;
}

int client_follow(int sock, char *id, int with_streaming)
{
    char buffer[BABBLE_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    if (strlen(id) > BABBLE_ID_SIZE)
    {
        fprintf(stderr, "Error -- invalid client id (too long): %s\n", id);
        return -1;
    }

    snprintf(buffer, sizeof(buffer), with_streaming ? "S %d %s\n" : "%d %s\n", FOLLOW, id);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        fprintf(stderr, "Error -- sending FOLLOW message\n");
        return -1;
    }

    if (!with_streaming)
    {
        char *ack = recv_one_msg(sock);
        if (!ack)
        {
            fprintf(stderr, "ERROR in FOLLOW ack\n");
            close(sock);
            return -1;
        }

        if (strstr(ack, "follow") == NULL)
        {
            free(ack);
            return -1;
        }
        free(ack);
    }
    else
    {
        usleep(100);
    }

    return 0;
}

int client_follow_count(int sock)
{
    char buffer[BABBLE_BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%d\n", FOLLOW_COUNT);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        fprintf(stderr, "Error -- sending FOLLOW_COUNT message\n");
        return -1;
    }

    char *count_ack = recv_one_msg(sock);
    if (!count_ack)
    {
        fprintf(stderr, "ERROR on FOLLOW_COUNT ack\n");
        close(sock);
        return -1;
    }

    int count = parse_fcount_ack(count_ack);
    free(count_ack);

    return count;
}

int client_publish(int sock, char *msg, int with_streaming)
{
    char buffer[BABBLE_BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    if (strlen(msg) > BABBLE_PUBLICATION_SIZE)
    {
        fprintf(stderr, "Error -- invalid msg (too long): %s\n", msg);
        return -1;
    }

    snprintf(buffer, sizeof(buffer), with_streaming ? "S %d %s\n" : "%d %s\n", PUBLISH, msg);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        fprintf(stderr, "Error -- sending PUBLISH message\n");
        return -1;
    }

    if (!with_streaming)
    {
        char *ack = recv_one_msg(sock);
        if (!ack)
        {
            fprintf(stderr, "ERROR in PUBLISH ack\n");
            close(sock);
            return -1;
        }

        if (strstr(ack, "{") == NULL)
        {
            free(ack);
            return -1;
        }
        free(ack);
    }
    else
    {
        usleep(1);
    }

    return 0;
}

int client_timeline(int sock, int silent)
{
    char buffer[BABBLE_BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%d\n", TIMELINE);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        fprintf(stderr, "Error -- sending TIMELINE message\n");
        return -1;
    }

    return recv_timeline_msg_and_print(sock, silent);
}

int client_rdv(int sock)
{
    char buffer[BABBLE_BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%d\n", RDV);

    if (network_send(sock, strlen(buffer) + 1, buffer) != strlen(buffer) + 1)
    {
        fprintf(stderr, "Error -- sending RDV message\n");
        return -1;
    }

    char *ack = recv_one_msg(sock);
    if (!ack)
    {
        fprintf(stderr, "ERROR in RDV ack\n");
        close(sock);
        return -1;
    }

    if (strstr(ack, "rdv_ack") == NULL)
    {
        free(ack);
        return -1;
    }
    free(ack);

    return 0;
}
