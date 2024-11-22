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

#include "babble_server.h"
#include "babble_config.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"
#include "babble_server_answer.h"
#include "fastrand.h"

int select_buffer_index(unsigned long key);
/* to activate random delays in the processing of messages */
int random_delay_activated;

/* helper function to display help */
static void display_help(char *exec)
{
    printf("Usage: %s -p port_number -r [activate_random_delays]\n", exec);
}

/* function to parse commands */
static int parse_command(char *str, command_t *cmd)
{
    char *name = NULL;
    str_clean(str);                               // clean the input string
    printf("Received command string: %s\n", str); // parsing test

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

/* process a command */
static int process_command(command_t *cmd, answer_t **answer)
{
    int res = 0;
    switch (cmd->cid)
    {
    case LOGIN:
        res = run_login_command(cmd, answer);
        break;
    case PUBLISH:
        random_delay(random_delay_activated);
        res = run_publish_command(cmd, answer);
        break;
    case FOLLOW:
        random_delay(random_delay_activated);
        res = run_follow_command(cmd, answer);
        break;
    case TIMELINE:
        random_delay(random_delay_activated);
        res = run_timeline_command(cmd, answer);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd, answer);
        break;
    case RDV:
        res = run_rdv_command(cmd, answer);
        break;
    case UNREGISTER:
        res = unregisted_client(cmd);
        *answer = NULL;
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

/* data structures for multiple buffers */
typedef struct
{
    command_t buffer[MAX_COMMANDS];
    int buffer_in;
    int buffer_out;
    int buffer_count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} command_buffer_t;

command_buffer_t buffers[BABBLE_PRODCONS_NB];
pthread_t comm_threads[MAX_CLIENT];
pthread_t executor_threads[BABBLE_PRODCONS_NB];

/* initialize the buffers */
void buffers_init(void)
{
    for (int i = 0; i < BABBLE_PRODCONS_NB; i++)
    {
        buffers[i].buffer_in = 0;
        buffers[i].buffer_out = 0;
        buffers[i].buffer_count = 0;
        pthread_mutex_init(&buffers[i].mutex, NULL);
        pthread_cond_init(&buffers[i].not_empty, NULL);
        pthread_cond_init(&buffers[i].not_full, NULL);
    }
}

void *communication_thread_routine(void *arg)
{
    int newsockfd = *(int *)arg;
    char *recv_buff = NULL;
    command_t *cmd;
    int recv_size;

    while ((recv_size = network_recv(newsockfd, (void **)&recv_buff)) > 0)
    {
        cmd = new_command(hash(recv_buff));
        if (parse_command(recv_buff, cmd) == -1)
        {

            answer_t *answer = NULL;
            notify_parse_error(cmd, recv_buff, &answer);
            if (answer)
            {
                send_answer_to_client(answer);
                free_answer(answer);
            }
            free(recv_buff);
            free(cmd);
            continue;
        }

        int buffer_index = select_buffer_index(cmd->key);
        command_buffer_t *buffer = &buffers[buffer_index];

        pthread_mutex_lock(&buffer->mutex);
        while (buffer->buffer_count == MAX_COMMANDS)
        {
            pthread_cond_wait(&buffer->not_full, &buffer->mutex);
        }

        buffer->buffer[buffer->buffer_in] = *cmd;
        buffer->buffer_in = (buffer->buffer_in + 1) % MAX_COMMANDS;
        buffer->buffer_count++;

        pthread_cond_signal(&buffer->not_empty);
        pthread_mutex_unlock(&buffer->mutex);

        free(recv_buff);
        free(cmd);
    }

    close(newsockfd);
    free(arg);
    pthread_exit(NULL);
}

void *executor_thread_routine(void *arg)
{
    int thread_id = *(int *)arg;
    command_buffer_t *buffer = &buffers[thread_id];
    fastRandomSetSeed(time(NULL) + thread_id * 100);
    command_t *cmd;
    answer_t *answer;

    while (1)
    {
        pthread_mutex_lock(&buffer->mutex);
        while (buffer->buffer_count == 0)
        {
            pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
        }

        cmd = &buffer->buffer[buffer->buffer_out];
        buffer->buffer_out = (buffer->buffer_out + 1) % MAX_COMMANDS;
        buffer->buffer_count--;

        pthread_cond_signal(&buffer->not_full);
        pthread_mutex_unlock(&buffer->mutex);

        if (process_command(cmd, &answer) == -1)
        {
            fprintf(stderr, "Error processing command\n");
        }

        if (answer != NULL)
        {
            send_answer_to_client(answer);
            free_answer(answer);
        }
    }

    free(arg);
    pthread_exit(NULL);
}

/* initialize executor threads */
void executor_threads_init(void)
{
    for (int i = 0; i < BABBLE_PRODCONS_NB; i++)
    {
        int *arg = malloc(sizeof(int));
        *arg = i;
        if (pthread_create(&executor_threads[i], NULL, executor_thread_routine, arg) != 0)
        {
            fprintf(stderr, "Error -- unable to create executor thread\n");
            exit(EXIT_FAILURE);
        }
    }
}

/* main function */
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
    buffers_init();
    executor_threads_init();

    if ((sockfd = server_connection_init(portno)) == -1)
    {
        return -1;
    }

    printf("Babble server bound to port %d\n", portno);

    int client_index = 0;
    while (1)
    {
        int *newsockfd = malloc(sizeof(int));
        *newsockfd = server_connection_accept(sockfd);
        if (*newsockfd < 0)
        {
            fprintf(stderr, "Error -- server accept\n");
            continue;
        }

        if (pthread_create(&comm_threads[client_index], NULL, communication_thread_routine, newsockfd) != 0)
        {
            fprintf(stderr, "Error -- unable to create communication thread\n");
            close(*newsockfd);
            continue;
        }
        client_index = (client_index + 1) % MAX_CLIENT;
    }

    close(sockfd);
    return 0;
}
