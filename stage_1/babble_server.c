#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>

#include "babble_server.h"
#include "babble_config.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"
#include "babble_server_answer.h"
#include "fastrand.h"

int random_delay_activated;
volatile sig_atomic_t server_running = 1; // server shutdown

static void display_help(char *exec)
{
    printf("Usage: %s -p port_number -r [activate_random_delays]\n", exec);
}

static int parse_command(char *str, command_t *cmd)
{
    char *name = NULL;

    /* start by cleaning the input */
    str_clean(str);

    /* get command id */
    printf("Received command string: %s\n", str); // Parsing test

    cmd->cid = str_to_command(str, &cmd->answer_expected);

    switch (cmd->cid)
    {
    case LOGIN:
        if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Error from [%s]-- invalid LOGIN -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case PUBLISH:
        if (str_to_payload(str, cmd->msg, BABBLE_PUBLICATION_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Warning from [%s]-- invalid PUBLISH -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case FOLLOW:
        if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE))
        {
            name = get_name_from_key(cmd->key);
            fprintf(stderr, "Warning from [%s]-- invalid FOLLOW -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case TIMELINE:
    case FOLLOW_COUNT:
    case RDV:
        cmd->msg[0] = '\0';
        break;
    default:
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Error from [%s]-- invalid client command -> %s\n", name, str);
        free(name);
        return -1;
    }

    return 0;
}

/* processes the command and eventually generates an answer */
int process_command(command_t *cmd, answer_t **answer)
{
    int res = 0;

    switch (cmd->cid)
    {
    case LOGIN:
        res = run_login_command(cmd, answer);
        break;
    case PUBLISH:
    case FOLLOW:
    case TIMELINE:
        random_delay(random_delay_activated);
        res = (cmd->cid == PUBLISH)  ? run_publish_command(cmd, answer)
              : (cmd->cid == FOLLOW) ? run_follow_command(cmd, answer)
                                     : run_timeline_command(cmd, answer);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd, answer);
        break;
    case RDV:
        res = run_rdv_command(cmd, answer);
        break;
    case UNREGISTER:
        res = unregisted_client(cmd);
        *answer = NULL; // No answer needed
        break;
    default:
        fprintf(stderr, "Error -- Unknown command id\n");
        return -1;
    }

    if (res)
    {
        fprintf(stderr, "Error -- Failed to run command ");
        display_command(cmd, stderr);
    }

    return res;
}

// Init thread tables
pthread_t comm_threads[MAX_CLIENT];
pthread_t executor_thread;

// Buffer counters
int buffer_in = 0;
int buffer_out = 0;
int buffer_count = 0;

command_t command_buffer[MAX_COMMANDS]; // Command buffer (Producer-Consumer)

// Mutual exclusion mechanisms
pthread_mutex_t buffer_mutex;
pthread_cond_t buffer_not_empty;
pthread_cond_t buffer_not_full;

int is_streaming_command(int cmd_id)
{
    return (cmd_id == PUBLISH || cmd_id == FOLLOW);
}

void *communication_thread_routine(void *arg)
{
    int newsockfd = *(int *)arg;
    char *recv_buff = NULL;
    command_t *cmd;
    int recv_size;
    unsigned long client_key;

    // Handle login
    recv_size = network_recv(newsockfd, (void **)&recv_buff);

    if (recv_size <= 0)
    {
        fprintf(stderr, "Client disconnected or recv error\n");
        close(newsockfd);
        free(arg);
        pthread_exit(NULL);
    }
    if (recv_size < 0)
    {
        fprintf(stderr, "Error -- recv from client\n");
        close(newsockfd);
        free(arg);
        pthread_exit(NULL);
    }
    cmd = new_command(0);
    if (parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN)
    {
        close(newsockfd);
        free(cmd);
        free(arg);
        pthread_exit(NULL);
    }
    cmd->sock = newsockfd;
    answer_t *answer;
    if (process_command(cmd, &answer) == -1)
    {
        close(newsockfd);
        free(cmd);
        free(arg);
        pthread_exit(NULL);
    }
    send_answer_to_client(answer);
    free_answer(answer);

    client_key = cmd->key;
    free(cmd);
    free(recv_buff);

    // main loop to handle commands
    while ((recv_size = network_recv(newsockfd, (void **)&recv_buff)) > 0)
    {
        cmd = new_command(client_key);
        if (parse_command(recv_buff, cmd) == -1)
        {
            notify_parse_error(cmd, recv_buff, &answer);
            send_answer_to_client(answer);
            free_answer(answer);
            free(cmd);
        }
        else
        {
            pthread_mutex_lock(&buffer_mutex);
            while (buffer_count == MAX_COMMANDS && server_running)
            {
                pthread_cond_wait(&buffer_not_full, &buffer_mutex);
            }
            if (!server_running)
            {
                pthread_mutex_unlock(&buffer_mutex);
                pthread_exit(NULL);
            }
            command_buffer[buffer_in] = *cmd;
            buffer_in = (buffer_in + 1) % MAX_COMMANDS;
            buffer_count++;
            pthread_cond_signal(&buffer_not_empty);
            pthread_mutex_unlock(&buffer_mutex);
            free(cmd);
        }
        free(recv_buff);
    }

    // Client unregistration
    cmd = new_command(client_key);
    cmd->cid = UNREGISTER;
    process_command(cmd, &answer);
    free(cmd);
    close(newsockfd);
    free(arg);
    pthread_exit(NULL);
}

void *executor_thread_routine(void *arg)
{
    fastRandomSetSeed(time(NULL) + pthread_self() * 100);

    while (server_running)
    {
        pthread_mutex_lock(&buffer_mutex);
        while (buffer_count == 0 && server_running)
        {
            pthread_cond_wait(&buffer_not_empty, &buffer_mutex);
        }
        if (!server_running)
        {
            pthread_mutex_unlock(&buffer_mutex);
            pthread_exit(NULL);
        }
        command_t *cmd = &command_buffer[buffer_out];
        buffer_out = (buffer_out + 1) % MAX_COMMANDS;
        buffer_count--;

        pthread_cond_signal(&buffer_not_full);
        pthread_mutex_unlock(&buffer_mutex);

        // check for UNREGISTER command and handle it directly
        if (cmd->cid == UNREGISTER)
        {
            process_command(cmd, NULL); // process the unregistration
            continue;                   // skip further processing for this command
        }

        answer_t *answer = NULL;
        process_command(cmd, &answer);
        if (answer != NULL)
        {
            send_answer_to_client(answer);
            free_answer(answer);
        }
    }
    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    int sockfd;
    int portno = BABBLE_PORT;

    int opt;
    int nb_args = 1;

    while ((opt = getopt(argc, argv, "+hp:r")) != -1)
    {
        switch (opt)
        {
        case 'p':
            portno = atoi(optarg);
            nb_args += 2;
            break;
        case 'r':
            random_delay_activated = 1;
            nb_args += 1;
            break;
        case 'h':
        case '?':
        default:
            display_help(argv[0]);
            return -1;
        }
    }

    if (nb_args != argc)
    {
        display_help(argv[0]);
        return -1;
    }
    server_data_init();
    pthread_mutex_init(&buffer_mutex, NULL);
    pthread_cond_init(&buffer_not_empty, NULL);
    pthread_cond_init(&buffer_not_full, NULL);

    if (pthread_create(&executor_thread, NULL, executor_thread_routine, NULL) != 0)
    {
        fprintf(stderr, "Error -- unable to create executor thread\n");
        return -1;
    }

    if ((sockfd = server_connection_init(portno)) == -1)
    {
        return -1;
    }

    printf("Babble server bound to port %d\n", portno);

    int client_index = 0;

    while (server_running)
    {
        int *newsockfd = malloc(sizeof(int));
        *newsockfd = server_connection_accept(sockfd);
        if (*newsockfd < 0)
        {
            fprintf(stderr, "Error -- server accept\n");
            free(newsockfd);
            continue;
        }

        if (client_index >= MAX_CLIENT)
        {
            fprintf(stderr, "Error -- max client limit reached\n");
            close(*newsockfd);
            free(newsockfd);
            continue;
        }

        if (pthread_create(&comm_threads[client_index], NULL, communication_thread_routine, newsockfd) != 0)
        {
            fprintf(stderr, "Error -- unable to create communication thread\n");
            close(*newsockfd);
            free(newsockfd);
            continue;
        }
        pthread_detach(comm_threads[client_index]);
        client_index = (client_index + 1) % MAX_CLIENT;
    }

    close(sockfd);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_cond_destroy(&buffer_not_empty);
    pthread_cond_destroy(&buffer_not_full);

    return 0;
}
