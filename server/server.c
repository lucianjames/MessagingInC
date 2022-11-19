#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <syscall.h>

#define perror_exit(msg) perror(msg); exit(EXIT_FAILURE);

#define PORT 52727

pthread_mutex_t mutex; /* used for client_socket_pointers */
/*  
client_socket_pointers is a dynamic array of pointers to socket descriptors. [*int, *int] points towards -> [realFdValue1, realFdValue2]
each thread will only interact with the array when trying to communicate with a different client, it wont use the array for its own socket
*/
int* client_socket_pointers; 
int client_count = 0; /* used for client_socket_pointers */


/*
verifies that a message is in fact a valid message.
ensures that the number of braces makes sense.
will do more in the future (NEEDS TO DO MORE IN THE FUTURE)
*/
int verify_msg_format(char* buffer, unsigned long size){
    unsigned long i = 0;
    unsigned long bracesCount = 0;
    while(i < size-1){
        if(buffer[i] == '{'){
            bracesCount++;
        }else if(buffer[i] == '}'){
            bracesCount--;
        }
        i++;
    }
    return (bracesCount==0?1:0);
}

/*
handle_client is spawned by main to talk to individual users in separate concurrent threads.
behaves as outlined in protocol.md
*/
void *handle_client(void* p_fd){
    int thread_id = syscall(SYS_gettid);
    char* client_id; /* clients chosen ID */
    char* buffer; /* used to store incoming data */
    char* messageBuffer;
    long bytes_received; /* used to store the number of bytes received from a single recv() call */
    long bytes_sent; /* used to */
    unsigned int i; /* used to iterate through the buffer */
    int fd = *(int*)p_fd; /* dereference the pointer to get the actual file descriptor */

    /* == RECEIVE CONNECT REQUEST == */
    /* this message identifies that a client is trying to connect */
    buffer = calloc(32+11, sizeof(char)); /* maximum ID size is 32 chars, +11 for the rest of the message */
    bytes_received = recv(fd, buffer, 42, 0); /* receive 42 bytes from the socket at fd and put them into the buffer */
    printf("Thread %d: Received %d bytes from client\n", thread_id, bytes_received);

    /* == VERIFY MESSAGE IS VALID == */
    /* verify the message format is valid (does not verify that it is an actual command/message) */
    if(!verify_msg_format(buffer, bytes_received)){
        printf("Thread %d: Received invalid format for REQCON message from client %d - Disconnecting (sending {CON_DENIED})\n", thread_id, fd);
        bytes_sent = send(fd, "{CON_DENIED}", 13, 0);
        free(buffer);
        close(fd);
        return NULL;
    }
    /* verify the command/message is of a valid type. at this moment, that means it must be REQCON */
    if(strncmp(buffer, "{REQCON{", 8) != 0){
        printf("Thread %d: Received invalid message for REQCON message from client %d - Disconnecting (sending {CON_DENIED})\n", thread_id, fd);
        bytes_sent = send(fd, "{CON_DENIED}", 13, 0);
        free(buffer);
        close(fd);
        return NULL;
    }

    /* == MESSAGE IS VALID, PROCESS IT == */
    /* finally, read the ID from the message */
    while(buffer[i] != '}'){ i++; } /* get the location of the first } in the buffer (this is where ID ends) */ /* THIS IS UNSAFE AS FUCK BRO */
    client_id = calloc(32, sizeof(char));
    strncpy(client_id, buffer+8, i-8); /* copy the ID from the buffer into client_id */
    printf("Thread %d: Successfull connection, client-specified ID: %s - Replying with {CON_ACCEPTED}\n", thread_id, client_id);
    bytes_sent = send(fd, "{CON_ACCEPTED}", 15, 0);
    printf("Thread %d: Sent %d bytes to client %d\n", thread_id, bytes_sent, fd);

    /* == PROCESS MESSAGES == */
    /* listen for a message from the client, if valid, pass it on to all the other clients */
    /* going to make this very basic for now, proper error handling and stuff will come later */
    while(1){
        buffer = calloc(1024, sizeof(char)); /* hard limit of 1024 on messages for now */
        bytes_received = recv(fd, buffer, 1024, 0);
        printf("Thread %d: Received %d bytes from client %d\n", thread_id, bytes_received, fd);
        if(bytes_received == 0){
            printf("Thread %d: Client %d disconnected\n", thread_id, fd);
            break;
        }
        if(!verify_msg_format(buffer, bytes_received)){
            continue;
        }
        /* without any proper format checking (TODO), send the received message to all the other clients */
        pthread_mutex_lock(&mutex);
        i=0;
        while(i<client_count){
            if(client_socket_pointers[i]!=fd){
                bytes_sent = send(client_socket_pointers[i], buffer, bytes_received, 0);
                printf("Thread %d: Sent %d bytes from client %d to client %d\n", thread_id, bytes_sent, fd, client_socket_pointers[i]);
            }
            i++;
        }
        pthread_mutex_unlock(&mutex);
        free(buffer);
    }

    /* == CLEANUP == */
    printf("Thread %d: Exiting (closing connection to client %d)\n", thread_id, fd);
    /* remove p_fd from client_socket_pointers */
    /* this may cause some fucky memory shit, ill have to find out */
    pthread_mutex_lock(&mutex);
    i=0;
    while(i<client_count){
        if(client_socket_pointers[i] == fd){
            /* found the socket descriptor, from this point on, move all the other pointers down one */
            while(i<client_count-1){
                client_socket_pointers[i] = client_socket_pointers[i+1];
                i++;
            }
        }
        i++;
    }
    client_count--;
    pthread_mutex_unlock(&mutex);
    free(client_id);
    free(buffer);
    close(fd);
    return NULL;
}

/*
main just sets everything up and waits for incoming connections
*/
int main(int argc, char const* argv[]){
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    signal(SIGPIPE, SIG_IGN); /* Ignore SIGPIPE, this prevents the server from crashing when a client disconnects */
    pthread_sigmask(SIGPIPE, NULL, NULL);
    pthread_mutex_init(&mutex, NULL);

    printf("Opening server....\n");
    /* create a socket to listen for incoming connections on */
    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror_exit("ERR: socket() failed");
    }
    /* set up the socket options */
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))){
        perror_exit("ERR: setsockopt() failed");
    }
    /* set up the address */
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    /* bind the socket to the port */
    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        perror_exit("ERR: bind() failed");
    }
    /* listen for incoming connections */
    if(listen(server_fd, 16) < 0){
        perror_exit("ERR: listen() failed");
    }
    /* start receiving connections and creating threads for them */
    printf("Waiting for connections...\n");
    while(1){
        int new_socket_fd;
        pthread_t new_thread;
        int addrlen = sizeof(address);
        int* p_new_socket_fd = malloc(sizeof(int));
        /* wait for a new connection */
        if((new_socket_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0){
            perror_exit("ERR: accept() failed");
        }
        p_new_socket_fd = &new_socket_fd; /* set the pointer to the new socket file descriptor */
        /* store the pointer in the global array, as well as passing it to the thread. */
        pthread_mutex_lock(&mutex);
        client_count++;
        client_socket_pointers = realloc(client_socket_pointers, client_count*sizeof(int));
        client_socket_pointers[client_count-1] = *p_new_socket_fd;
        pthread_mutex_unlock(&mutex);
        /* create a new thread to handle the connection */
        if(pthread_create(&new_thread, NULL, handle_client, p_new_socket_fd) != 0){
            perror_exit("ERR: pthread_create() failed");
        }
    }
    close(server_fd);
    return 0;
}
