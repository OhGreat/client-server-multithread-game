// #include <GL/glut.h> // not needed here

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <semaphore.h>
#include <arpa/inet.h>

#include "server_common.h"
#include "image.h"
#include "surface.h"
#include "world.h"
#include "vehicle.h"
#include "world_viewer.h"
#include "error_helper.h"
#include "server_connection_handler.c"
#include "so_game_protocol.h"



sem_t* client_list_sem;
sem_t* wup_sem;

int main(int argc, char **argv) {
  if (argc<3) {
    printf("usage: %s <elevation_image> <texture_image>\n", argv[1]);
    exit(-1);
  }
  char* elevation_filename=argv[1];
  char* texture_filename=argv[2];
  char* vehicle_texture_filename="./images/arrow-right.ppm";
  printf("loading elevation image from %s ... ", elevation_filename);

  // load the images
  Image* surface_elevation = Image_load(elevation_filename);
  if (surface_elevation) {
    printf("Done! \n");
  } else {
    printf("Fail! \n");
  }


  printf("loading texture image from %s ... ", texture_filename);
  Image* surface_texture = Image_load(texture_filename);
  if (surface_texture) {
    printf("Done! \n");
  } else {
    printf("Fail! \n");
  }

  printf("loading vehicle (default) texture from %s ... ", vehicle_texture_filename);
  Image* vehicle_texture = Image_load(vehicle_texture_filename);
  if (vehicle_texture) {
    printf("Done! \n");
  } else {
    printf("Fail! \n");
  }

  //creating world
  World* world = malloc(sizeof(World));
  World_init(world, surface_elevation, surface_texture, 0.5, 0.5, 0.5);


  int ret;
  sem_cleanup();
  //creating map struct to share with clients
  Map* map_info = malloc(sizeof(Map));
  map_info->map_texture = surface_texture;
  map_info->map_elevation = surface_elevation;


  //initializing semaphore for thread synchronization
  client_list_sem = sem_open(CLIENT_LIST_SEM, O_CREAT | O_EXCL, 0600, 1);
  if (client_list_sem == SEM_FAILED && errno == EEXIST) {
      sem_unlink(CLIENT_LIST_SEM);
      client_list_sem = sem_open(CLIENT_LIST_SEM, O_CREAT | O_EXCL, 0600, 1);
  }
  if (client_list_sem == SEM_FAILED) {
      printf("[FATAL ERROR] could not open client_list_sem, the reason is: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
  }

  //creating worldupdatepacket and client_list to share with clients
  ListHead* client_list = malloc(sizeof(ListHead));
  List_init(client_list);

  WorldUpdatePacket* wup = malloc(sizeof(WorldUpdatePacket));
  wup->header.type = 0x6;
  wup->header.size = sizeof(WorldUpdatePacket);
  wup->num_vehicles = 0;
  //creating semaphore to handle world update packet
  wup_sem = sem_open(WUP_SEM, O_CREAT | O_EXCL, 0600, 1);
  if (wup_sem == SEM_FAILED && errno == EEXIST) {
      sem_unlink(WUP_SEM);
      client_list_sem = sem_open(WUP_SEM, O_CREAT | O_EXCL, 0600, 1);
  }
  if (wup_sem == SEM_FAILED) {
      printf("[FATAL ERROR] could not open wup_sem, the reason is: %s\n", strerror(errno));
      exit(EXIT_FAILURE);
  }


  //creating thread to send wup to clients **********************************
  pthread_t wup_sender_thread;
  wup_sender_args* wup_thread_args = malloc(sizeof(wup_sender_args));
  wup_thread_args->client_list = client_list;
  wup_thread_args->wup = wup;
  ret = pthread_create(&wup_sender_thread, NULL, wup_sender, (void *) wup_thread_args);
  PTHREAD_ERROR_HELPER(ret, "Could not create wup sender thread\n");
  ret = pthread_detach(wup_sender_thread);
  PTHREAD_ERROR_HELPER(ret, "Could not detach wup sender thread");
  if (DEBUG) printf("Thread spawned to periodically send wup to clients\n" );
  //**************************************************************************

  //creating client update receiver socket************************************
  int cl_up_recv_sock;
  struct sockaddr_in server_cl_up_recv_addr;
  struct sockaddr_in* cl_up_client_addr = malloc(sizeof(struct sockaddr_in));

  server_cl_up_recv_addr.sin_family = AF_INET;
  server_cl_up_recv_addr.sin_port = htons(CL_UP_RECV_PORT);
  server_cl_up_recv_addr.sin_addr.s_addr = INADDR_ANY;

  cl_up_recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(cl_up_recv_sock, "Error creating vehicle update receiver socket\n");

  ret = bind(cl_up_recv_sock, (struct sockaddr*) &server_cl_up_recv_addr, sizeof(server_cl_up_recv_addr));
  ERROR_HELPER(ret, "Error binding address to socket in server vehicle update\n");
  //**************************************************************************

  //thread to handle client updates*******************************************
  cl_up_args* cl_args = malloc(sizeof(cl_up_args));
  cl_args->wup = wup;
  cl_args->client_list = client_list;
  cl_args->world = world;
  cl_args->cl_up_socket = cl_up_recv_sock;
  pthread_t cl_thread;
  ret = pthread_create(&cl_thread, NULL, cl_up_handler, (void*) cl_args);
  PTHREAD_ERROR_HELPER(ret, "Could not create cl_up handler thread\n");
  ret = pthread_detach(cl_thread);
  PTHREAD_ERROR_HELPER(ret, "Could not detach cl_up handler thread");
  if (DEBUG) printf("Spawned thread to handle cl_up from clients\n");

  //creating texture handler socket and thread********************************
  int texture_handler_socket;
  struct sockaddr_in texture_addr;
  texture_addr.sin_family = AF_INET;
  texture_addr.sin_port = htons(SERVER_TEXTURE_HANDLER_PORT);
  texture_addr.sin_addr.s_addr = INADDR_ANY;

  texture_handler_socket = socket(AF_INET, SOCK_DGRAM, 0);
  ERROR_HELPER(texture_handler_socket, "Error creating texture handler socket\n");

  ret = bind(texture_handler_socket, (struct sockaddr*) &texture_addr, sizeof(texture_addr));
  ERROR_HELPER(ret, "Error binding address to texture handler socket\n");
/*
  //creating thread to handle texture requests
  pthread_t texture_handler_thread;
  texture_handler_args* texture_args = malloc(sizeof(texture_handler_args));
  texture_args->socket_desc = texture_handler_socket;
  texture_args->world = world;
  ret = pthread_create(&texture_handler_thread, NULL, texture_request_handler, (void *) texture_args);
  PTHREAD_ERROR_HELPER(ret, "Could not create texture handler thread\n");
  ret = pthread_detach(texture_handler_thread);
  PTHREAD_ERROR_HELPER(ret, "Could not detach texture handler thread");
  if (DEBUG) printf("Spawned thread to handle texture requests from clients\n");
  ****************************************************************************
*/
  // creating tcp_socket and accepting new connections to be handled by a thread
  int tcp_socket_desc, tcp_client_desc;
  struct sockaddr_in tcp_server_addr = {0};
  int sockaddr_len = sizeof(struct sockaddr_in);
  struct sockaddr_in* tcp_client_addr = malloc(sizeof(struct sockaddr_in));

  tcp_socket_desc = socket(AF_INET, SOCK_STREAM, 0);
  ERROR_HELPER(tcp_socket_desc,"Cannot initialize socket");

  tcp_server_addr.sin_addr.s_addr   = INADDR_ANY;
  tcp_server_addr.sin_family        = AF_INET;
  tcp_server_addr.sin_port          = htons(SERVER_TCP_PORT);

  /* enabling SO_REUSEADDR to quickly restart server after crash */
  int reuseaddr_opt = 1;
  ret = setsockopt(tcp_socket_desc, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt, sizeof(reuseaddr_opt));
  ERROR_HELPER(ret, "Cannot set SO_REUSEADDR option");

  ret = bind(tcp_socket_desc, (struct sockaddr*) &tcp_server_addr, sockaddr_len);
  ERROR_HELPER(ret, "Cannot bind address to socket");

  ret = listen(tcp_socket_desc, MAX_CLIENTS);
  ERROR_HELPER(ret, "Cannot listen on socket");

  if (DEBUG) printf("Waiting for incoming connections..\n");

  //creating thread to handle incoming client connections
  while (1) {

      tcp_client_desc = accept(tcp_socket_desc, (struct sockaddr* ) tcp_client_addr, (socklen_t *) &sockaddr_len);
      ERROR_HELPER(tcp_client_desc, "Cannot open socket for incoming connection");

      if (DEBUG) fprintf(stderr, "Accepted incoming connection succesfully...\n");

      //creating udp client addr to pass to client thread
      cl_up_client_addr->sin_family = AF_INET;
      cl_up_client_addr->sin_addr.s_addr = tcp_server_addr.sin_addr.s_addr;

      pthread_t client_handler_thread;
      connection_handler_args *thread_args = malloc(sizeof(connection_handler_args));
      thread_args->tcp_socket_desc = tcp_client_desc;
      thread_args->client_addr = tcp_client_addr;
      thread_args->cl_up_recv_sock = cl_up_recv_sock;
      thread_args->server_cl_up_recv_addr = &server_cl_up_recv_addr;
      thread_args->texture_handler_socket = texture_handler_socket;
      thread_args->cl_up_client_addr = cl_up_client_addr;
      thread_args->world = world;
      thread_args->map_info = map_info;
      thread_args->wup = wup;
      thread_args->client_list = client_list;

      ret = pthread_create(&client_handler_thread, NULL, server_connection_handler, (void *)thread_args);
      PTHREAD_ERROR_HELPER(ret, "Could not create a new thread\n");

      ret = pthread_detach(client_handler_thread);
      PTHREAD_ERROR_HELPER(ret, "Could not detach thread");

      if (DEBUG) printf("New thread created to handle client connection\n");

      tcp_client_addr = calloc(1, sizeof(struct sockaddr_in));
      cl_up_client_addr = calloc(1, sizeof(struct sockaddr_in));


  }

  sem_cleanup();

  return 0;
}

//function to send wup to clients***********************************************
void* wup_sender(void* arg) {
    int ret = 0, i = 0;
    int bytes_to_send, socket_desc;
    struct sockaddr_in client_addr = {0};
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(CLIENT_WUP_RECEIVER_PORT);

    char buffer[1024*1024*5];
    wup_sender_args* args = (wup_sender_args*) arg;

    ListHead* client_list = args->client_list;
    int n_clients;
    Client_info* client;

    char addr_str[INET6_ADDRSTRLEN];

    socket_desc = socket(AF_INET, SOCK_DGRAM, 0);
    ERROR_HELPER(socket_desc, "Could not create socket to send wup \n");

    while (1) {

        sem_t* client_list_sem = sem_open(CLIENT_LIST_SEM, 0);
        ret = sem_wait(client_list_sem);
        ERROR_HELPER(ret, "Cannot open client list semaphore\n");
        sem_t* wup_sem = sem_open(WUP_SEM, 0);
        ERROR_HELPER(ret, "Cannot open wup semaphore to send wup\n");
        ret = sem_wait(wup_sem);
        ERROR_HELPER(ret, "Cannot wait wup sem\n");

        n_clients = client_list->size;
        printf("n connected clients:%d\n",n_clients);
        client = (Client_info*) client_list->first;
        bytes_to_send = Packet_serialize(buffer, (PacketHeader*) args->wup);
        while (i < n_clients) {

            client_addr.sin_addr.s_addr = client->client_addr->sin_addr.s_addr;

            ret = sendto(socket_desc, &bytes_to_send, HEADER_SIZE, 0, (struct sockaddr*) &client_addr, sizeof(client_addr));
            ret = sendto(socket_desc, buffer, bytes_to_send, 0, (struct sockaddr*) &client_addr, sizeof(client_addr));


            inet_ntop(AF_INET, &(client_addr.sin_addr.s_addr), addr_str, sizeof(addr_str));
            if (DEBUG) printf("wup sent to: %d addr: %s\n", client->id, addr_str);
            client = (Client_info*) client->list.next;
            i++;

          }
          i = 0;
          ret = sem_post(wup_sem);
          ERROR_HELPER(ret, "Cannot post wup sem\n");
          ret = sem_close(wup_sem);
          ERROR_HELPER(ret, "Cannot close wup sem\n");
          ret = sem_post(client_list_sem);
          ERROR_HELPER(ret, "Cannot post client list semaphore\n");
          ret = sem_close(client_list_sem);
          ERROR_HELPER(ret, "Cannot close client list semaphore\n");
          ret = usleep(50000);
          if (ret == -1 && errno == EINTR) printf("error sleeping thread with usleep\n");
          if (DEBUG) printf("successfully sent wup to all clients\n");
    }
}
/*

//function to handle texture requests**********************************************
void* texture_request_handler(void* arg) {
  int ret = 0;
  int read_bytes, bytes_to_send, bytes_to_read, bytes_sent, socket_desc;
  struct sockaddr_in client_addr ={0};
  int sockaddr_len = sizeof(struct sockaddr_in);
  char buffer[1024*1024*5];
  texture_handler_args* args = (texture_handler_args*) arg;
  socket_desc = args->socket_desc;






  while(1) {

    read_bytes = recvfrom(socket_desc, buffer, bytes_to_read, MSG_WAITALL, (struct sockaddr*) &client_addr, (socklen_t*) &sockaddr_len );
    ERROR_HELPER(read_bytes, "Cannot receive vehicle update packet from client\n");

    printf("Texture request received!!!\n");

    ImagePacket* text_req = (ImagePacket*) Packet_deserialize(buffer, read_bytes);
    if (text_req->header.type != 0x2 && text_req->id == 0)
        ERROR_HELPER(-1, "Received wrong packet, waiting for texture request\n");

    vehicle = World_getVehicle(args->world, text_req->id);
    text_req->header.type = 0x4;
    text_req->image = vehicle->texture;
    text_req->header.size = sizeof(text_req);
    bytes_to_send = Packet_serialize(buffer, (PacketHeader*) text_req);
    printf("TEXTURE BYTES TO SEND: %d\n", bytes_to_send);
    ret = sendto(socket_desc, &bytes_to_send, sizeof(int), 0, (struct sockaddr*) &client_addr, sizeof(client_addr));
    bytes_sent = 0;
    while (bytes_sent < bytes_to_send) {
        ret = sendto(socket_desc, buffer+bytes_sent, 1, 0, (struct sockaddr*) &client_addr, sizeof(client_addr));
        if (errno == EINTR) continue;
        ERROR_HELPER(ret, "Error sending texture to client that requested it\n");
        bytes_sent += ret;
    }

    printf("texture: %d sent to client!\n", text_req->id);
    Packet_free((PacketHeader*) text_req);

  }
}
*/
void* cl_up_handler (void* arg) {
  int ret;
  char buffer[1024*1024*5];
  cl_up_args* args = (cl_up_args*) arg;
  struct sockaddr_in client_addr;
  socklen_t addrlen = sizeof(client_addr);
  VehicleUpdatePacket* vup = malloc( sizeof(VehicleUpdatePacket));
  vup->header.size = sizeof(VehicleUpdatePacket);
  vup->header.type = 0x7;
  int bytes_to_read = Packet_serialize(buffer, (PacketHeader*) vup);
  printf("CL TO READ: %d\n", bytes_to_read);
  Packet_free((PacketHeader*) vup);
  Vehicle* veh;
  ClientUpdate* cl_up;
  int sem_val = 0;
  //sem_t* sem;
  while(1) {
      ret = recvfrom(args->cl_up_socket, buffer, bytes_to_read, MSG_WAITALL, (struct sockaddr*) &client_addr, &addrlen);
      ERROR_HELPER(ret, "Error reciving cl_up\n");
      if (DEBUG) printf("received cl_up!!\n");
      printf("CL TO READ: %d\n", bytes_to_read);
/*

      if (buffer[0] == '0') {
        sem = sem_open(WUP_SEM, 0);
        ERROR_HELPER(ret, "Could not open wup sem to log out client\n");
        ret = sem_wait(sem);
        ERROR_HELPER(ret, "Could not wait wup sem to log out client\n");
        List_detach(args->client_list, (ListItem*) client);
        wup_cl_remove(args->wup, buffer[1]);
        ret = sem_post(sem);
        ERROR_HELPER(ret, "Could not post wup sem to log out client\n");
        ret = sem_close(sem);
        ERROR_HELPER(ret, "Could not close wup sem to log out client\n");
        ret = sem_close(sem);
        ERROR_HELPER(ret, "Could not close wup_sem\n");
      }
*/
      vup = (VehicleUpdatePacket*) Packet_deserialize( buffer, bytes_to_read);

      //find client and update its vehicles
      Client_info* client = (Client_info*) args->client_list->first;
      while (client != NULL && client->client_addr->sin_addr.s_addr != client_addr.sin_addr.s_addr) {
        client = (Client_info*) client->list.next;
      }
      if (client != NULL) {
          cl_up = client->cl_up;
          veh = client->veh;
          veh->translational_force_update = vup->translational_force;
          veh->rotational_force_update = vup->rotational_force;
          Vehicle_update(veh, args->world->dt);
          while (sem_val == 0) {
            sem_getvalue(wup_sem, &sem_val);
          }
          cl_up->x = veh->x;
          cl_up->y = veh->y;
          cl_up->theta = veh->theta;
          if (DEBUG) printf("Updated vehicle: %d on x:%f, y: %f\n", cl_up->id, cl_up->x, cl_up->y);
      }
    }
  }



void wup_cl_remove(WorldUpdatePacket* wup, int client_id)
{
    int i, j=0, n_veh= wup->num_vehicles;
    WorldUpdatePacket* new = malloc(sizeof(WorldUpdatePacket));
    new->num_vehicles = n_veh-1;
    new->updates = malloc(sizeof(ClientUpdate)*new->num_vehicles);
    for (i=0;i<n_veh;i++)
    {
        if (wup->updates[i].id != client_id)
        {
            new->updates[j] = wup->updates[i];
            j++;
        }
    }
    free(wup->updates);
    wup->updates = new->updates;
    wup->num_vehicles = n_veh-1;
}


//sem cleanup func
void sem_cleanup(void) {
    sem_close(client_list_sem);
    sem_unlink(CLIENT_LIST_SEM);

    sem_close(wup_sem);
    sem_unlink(WUP_SEM);
}
