//--------------------------------------------------------------------------------------------------
// Network Lab                             Spring 2024                           System Programming
//
/// @file
/// @brief Client-side implementation of Network Lab
///
/// @author <your name>
/// @studid <your student id>
///
/// @section changelog Change Log
/// 2020/11/18 Hyunik Kim created
/// 2021/11/23 Jaume Mateu Cuadrat cleanup, add milestones
/// 2024/05/31 ARC lab add multiple orders per request
///
/// @section license_section License
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// Copyright (c) 2024, Architecture and Code Optimization Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "net.h"
#include "burger.h"

/// @brief client error function
/// @param socketfd file drescriptor of the socket
void error_client(int socketfd) {
	close(socketfd);
  	pthread_exit(NULL);
}

/// @brief client task for connection thread
void *thread_task(void *data)
{
  struct addrinfo *ai, *ai_it;
  size_t read, sent, buflen;
  int serverfd = -1;
  char *buffer;
  pthread_t tid;
  int *choices;
  unsigned int burger_count;

  tid = pthread_self();

  buffer = (char *)malloc(BUF_SIZE);
  buflen = BUF_SIZE;

  // Connect to McDonald's server
  //
  // Use getsocklist() to get the socket list
  // TODO
  int res;
  ai = getsocklist(IP, PORT, AF_UNSPEC, SOCK_STREAM, 0, &res);
  if (ai == NULL) {
    perror("get_socketlist");
    //printf("Cannot get address information\n");
    exit(EXIT_FAILURE);
  }

  // Iterate over addrinfos and try to connect
  // TODO
  for (ai_it = ai; ai_it != NULL; ai_it = ai_it->ai_next) {
    serverfd = socket(ai_it->ai_family, ai_it->ai_socktype, ai_it->ai_protocol);
    if (serverfd == -1) {
      perror("socket");
      //printf("Cannot create socket\n");
      continue; // Try next addrinfo
    }

    if (connect(serverfd, ai_it->ai_addr, ai_it->ai_addrlen) == -1) {
      perror("connect");
      //printf("Cannot connect to the server\n");
      close(serverfd);
      continue; // Try next addrinfo
    }

    break;  // Success
  }

  // Read welcome message from the server
  read = get_line(serverfd, &buffer, &buflen);
  if (read <= 0) {
    printf("Cannot read data from server\n");
    error_client(serverfd);
  }

  printf("[Thread %lu] From server: %s", tid, buffer);

  // Choose the number of orders for request
  if(BURGER_NUM_RAND)
    burger_count = rand() % MAX_BURGERS + 1;
  else
    burger_count = MAX_BURGERS;

  printf("[Thread %lu] Ordering %u burgers\n", tid, burger_count);

  // Randomly choose burger type for each order
  choices = (int *)malloc(sizeof(int) * burger_count);
  for (int i=0; i<burger_count; i++){
    int choice = rand() % BURGER_TYPE_MAX;

    // concat burger to buffer string
    char *temp_buffer;
    if (i == 0){
      int res = asprintf(&temp_buffer, "%s", burger_names[choice]);
      if (res<0) perror("asprintf");
    }
    else{
      int res = asprintf(&temp_buffer, "%s %s", buffer, burger_names[choice]);
      if (res<0) perror("asprintf");
    }
    
    strncpy(buffer, temp_buffer, BUF_SIZE);
    free(temp_buffer);

    choices[i] = choice;
  }

  printf("[Thread %lu] To server: Can I have %s burger(s)?\n", tid, buffer);
  
  int str_len = strnlen(buffer, BUF_SIZE);
  buffer[str_len] = '\n';

  // burger_count : number of burgers to order (1~MAX_BURGERS)
  // choices : array of burger choices (index of burger_names)
  // buffer : string to send to the server
  // str_len : length of the string to send

  // Send request to the server
  sent = put_line(serverfd, buffer, strlen(buffer));
  if (sent < 0) {
    printf("Error: cannot send data to server\n");
    error_client(serverfd);
  }

  // Get final message from the server
  memset(buffer, 0, BUF_SIZE);
  read = get_line(serverfd, &buffer, &buflen);
  if (read <= 0) {
    printf("Cannot read data from server\n");
    error_client(serverfd);
  }

  printf("[Thread %lu] From server: %s", tid, buffer);

  free(choices);

  close(serverfd);
  freeaddrinfo(ai);
  pthread_exit(NULL);
}

/// @brief program entry point
int main(int argc, char const *argv[])
{
  int i;
  int num_threads;

  if (argc != 2) {
    printf("usage ./client <num_threads>\n");
    return 0;
  }  
  //
  // TODO
  //
  // - create n threads where n is the numerical value of argv[1]
  // - have all threads join before exiting

  num_threads = atoi(argv[1]);

  if (num_threads <= 0) {
    fprintf(stderr, "Number of threads must be a positive integer\n");
    return EXIT_FAILURE;
  }

  pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
  if (threads == NULL) {
    perror("malloc");
    return EXIT_FAILURE;
  }

  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&threads[i], NULL, thread_task, NULL) != 0) {
      perror("pthread_create");
      free(threads);
      return EXIT_FAILURE;
    }
  }

  for (int i = 0; i < num_threads; i++) {
    if (pthread_join(threads[i], NULL) != 0) {
      perror("pthread_join");
      free(threads);
      return EXIT_FAILURE;
    }
  }

  free(threads);
  return 0;

}
