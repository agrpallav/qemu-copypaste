#include <sys/socket.h>
#include <sys/un.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


//char *socket_path = "./socket";
char *socket_path = "/tmp/sock1";

int main(int argc, char *argv[]) {
  struct sockaddr_un addr;
  char buf[100];
  int fd,rc;

  if (argc > 1) socket_path=argv[1];

  if ( (fd = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket error");
    exit(-1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("connect error");
    exit(-1);
  }
GIOChannel *gc = g_io_channel_unix_new(fd);
g_io_channel_set_encoding(gc,NULL, NULL);
gsize wrote;
//const gchar buff[10];

GIOStatus st=g_io_channel_write_chars(gc, "my name is pallav", -1, &wrote, NULL);
g_io_channel_flush(gc,NULL);
printf("wrote: %d status: %d\n",wrote,st);

sleep(3);

st=g_io_channel_write_chars(gc, "my name is anshu", -1, &wrote, NULL);
g_io_channel_flush(gc,NULL);
sleep(2);

//write(fd, "my name is pallav",rc);
/*
  while( (rc=read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
    if (write(fd, buf, rc) != rc) {
      if (rc > 0) fprintf(stderr,"partial write");
      else {
        perror("write error");
        exit(-1);
      }
    }
  }*/
  
  g_io_channel_shutdown(gc,TRUE, NULL);
  g_io_channel_unref(gc);
  
//close(fd);
  return 0;
}
