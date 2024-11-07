# Answering the TP Questions

## Digging into the code

1. Line 174 of babble_server :

   ````if((sockfd = server_connection_init(portno)) == -1){
       return -1;
   }```

   ````

2. Line 184 to 217 of babble_server :

   ````while(1){

       if((newsockfd= server_connection_accept(sockfd))==-1){
           return -1;
       }

       memset(client_name, 0, BABBLE_ID_SIZE+1);
       if((recv_size = network_recv(newsockfd, (void**)&recv_buff)) < 0){
           fprintf(stderr, "Error -- recv from client\n");
           close(newsockfd);
           continue;
       }
       cmd = new_command(0);

       if(parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN){
           fprintf(stderr, "Error -- in LOGIN message\n");
           close(newsockfd);
           free(cmd);
           continue;
       }```

   ````

3. The first step after accepting a client's connexion, is to register the socket associated with that client. We then process the LOGIN command "only" with process_command, notify him of the registration if successful. We then generate the ID Hash key, that we store locally, we then start looping on the client's commands, including messages. After receiving the command, the server parses it to see which one should be executed, then processes the commmand with process_command, then frees the cmd, answer and recv_buffer.

4. The processing of LOGIN is different than the others, because its the message that initialize the client's actual connexion to the Babble server, rather than just the initial socket connexion. It also identifies the client by assigning him a hash key and save it locally.

5. I suppose the role of registration table is simply saving the id keys (hash) of each registered client.

6. Upon registering for the first time, the string id given to the server on the client's stdin is hashed into a unique key, which purpose is to identify the client for later babble interactions. The key is added to the registration table if there is enough place, then is looked up in that same table if needed.

7. The command is parsed, processed, eventually generates an answer that is forwarded to the client.
