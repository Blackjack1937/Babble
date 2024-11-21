#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "babble_registration.h"

client_bundle_t *registration_table[MAX_CLIENT];
int nb_registered_clients;

/* Forward synchronization implementation*/
/* Variables */
pthread_mutex_t registration_table_mutex;
pthread_cond_t readers_turn;
pthread_cond_t writers_turn;
/* Counters */
int active_readers = 0;
int active_writers = 0;
int wait_writers = 0;


void registration_init(void)
{
    nb_registered_clients=0;

    memset(registration_table, 0, MAX_CLIENT * sizeof(client_bundle_t*));
    
    pthread_mutex_init(&registration_table_mutex, NULL);
    pthread_cond_init(&readers_turn, NULL);
    pthread_cond_init(&writers_turn, NULL);
}

/* Forward lock and unlock functions for synchronization */
void reader_mutex_lock(void){
    pthread_mutex_lock(&registration_table_mutex);

    //wait if there are active or waiting writers
    while(wait_writers > 0 || active_writers > 0){
        pthread_cond_wait(&readers_turn, & registration_table_mutex);
    }
    active_readers++;
    pthread_mutex_unlock(&registration_table_mutex);
}

void reader_mutex_unlock(void){
    pthread_mutex_lock(&registration_table_mutex);
    active_readers--;
    if(active_readers == 0){
        pthread_cond_signal(&writers_turn);
    }
    pthread_mutex_unlock(&registration_table_mutex);
}

void writer_mutext_lock(void){

    pthread_mutex_lock(&registration_table_mutex);

    wait_writers++;

    //wait if no active readers or writers
    while(active_readers>0 || active_writers>0){
        pthread_cond_wait(&writers_turn, &registration_table_mutex);
    }

    wait_writers--;
    active_writers++;

    pthread_mutex_unlock(&registration_table_mutex);
}

void writer_mutext_unlock(void){
    
    pthread_mutex_lock(&registration_table_mutex);

    active_writers--;

    // let waiting writers write before readers read
    if(wait_writers > 0){
        pthread_cond_signal(&writers_turn);
    } else {
        pthread_cond_broadcast(&readers_turn);
    }
    pthread_mutex_unlock(&registration_table_mutex);

}

client_bundle_t* registration_lookup(unsigned long key)
{
    //locking the reader lock
    reader_mutex_lock();

    int i=0;
    client_bundle_t *c = NULL;

    for(i=0; i< nb_registered_clients; i++){
        if(registration_table[i]->key == key){
            c = registration_table[i];
            break;
        }
    }

    reader_mutex_unlock();
    return c;
}

int registration_insert(client_bundle_t* cl)
{    
    
    //locking the writer lock
    writer_mutext_lock();

    if(nb_registered_clients == MAX_CLIENT){
        fprintf(stderr, "ERROR: MAX NUMBER OF CLIENTS REACHED\n");
        writer_mutext_unlock();
        return -1;
    }
    
    /* lookup to find if key already exists */
    int i=0;

    
    for(i=0; i< nb_registered_clients; i++){
        if(registration_table[i]->key == cl->key){
            break;
        }
    }
    
    
    if(i != nb_registered_clients){
        writer_mutext_unlock();
        fprintf(stderr, "Error -- id % ld already in use\n", cl->key);
        return -1;
    }

    /* insert cl */
    registration_table[nb_registered_clients]=cl;
    nb_registered_clients++;

    writer_mutext_unlock();
    return 0;
}


client_bundle_t* registration_remove(unsigned long key)
{
    //locking another writer lock
    writer_mutext_lock();
    
    int i=0;
    
    for(i=0; i<nb_registered_clients; i++){
        if(registration_table[i]->key == key){
            break;
        }
    }

    if(i == nb_registered_clients){
        fprintf(stderr, "Error -- no client found\n");
        writer_mutext_unlock();
        return NULL;
    }
    
    client_bundle_t* cl= registration_table[i];

    nb_registered_clients--;
    registration_table[i] = registration_table[nb_registered_clients];

    writer_mutext_unlock();
    return cl;
}

//maybe don't need? on shutdown or termination  but the whole prograùm will be over so ?
void registration_lock_destroy(void){
    pthread_mutex_destroy(&registration_table_mutex);
    pthread_cond_destroy(&readers_turn);
    pthread_cond_destroy(&writers_turn);
}
