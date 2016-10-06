/*
  afl.c - reads stdin to execute script on in-RAM stfs and dump the result into test.img
  commands are
  m <path> - mkdir
  x <path> - rmdir
  o 0|64 <string> - open 64=O_CREAT
  w <fd> <size> - write
  r <fd> <size> - read
  s <fd> <pos> <whence> - seek
  c <fd> - close
  t <size> <path> - truncate
  d <path> - unlink

  <path> is always length prefixed, space-separated, e.g.:
  m 5 /root
      ^ path
    ^ length of path

  ./afl is also used by stfsfuzz.py, if you want to use it with afl,
  you should compile without logging and dumping of the fs image.

 */
#include "stfs.h"
#include <stdio.h>
#include <string.h>

void dump(uint8_t *src, uint32_t len);

/*
  directory ops
  m path
int stfs_mkdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
  ctx=l path
int opendir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, ReaddirCTX *ctx);
  inode=n ctx
const Inode_t* readdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], ReaddirCTX *ctx);
  x path
int stfs_rmdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
  file ops
  fd=o path flags
int stfs_open(uint8_t *path, int oflag, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
  w fd buf size
ssize_t stfs_write(int fildes, const void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
  r fd buf size
ssize_t stfs_read(int fildes, void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
  s off whence
off_t stfs_lseek(int fildes, off_t offset, int whence);
  c fd
int stfs_close(int fildes, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
  d path
int stfs_unlink(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
  t path size
int stfs_truncate(uint8_t *path, int length, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);


      m path
  ctx=l path
inode=n ctx
      x path
   fd=o path flags
      w fd buf
      r fd size
      s fd off whence
      c fd
      d path
      t path size
*/

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

void handler(int signo) {
  exit(1);
}

Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK];
int _main(void) {
  printf("AFL test harness\n");
  memset(&blocks,0xff,sizeof(blocks));

  struct sigaction sa;

  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGALRM, &sa, NULL);

  if(stfs_init(blocks)==-1) {
    return 0;
  };

  uint8_t cmd;
  int cmd_size;
  while (1) {
    //alarm(1);
    if(fscanf(stdin,"%c ", &cmd)!=1) return 0;
    //alarm(0);
    switch(cmd) {
    case('m'): { /*mkdir path*/
      //alarm(1);
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t path[cmd_size+1];
      int n;
      if((n=fread(path, 1, cmd_size, stdin))!=cmd_size) return 0;
      //alarm(0);
      path[n]=0;
      fprintf(stderr,"mkdir %s returns: %d\n", path, stfs_mkdir(blocks, path));
      break;
    };
    case('l'): { /*ctx=opendir path*/ };
    case('n'): { /*readdir ctx*/ };
    case('x'): { /*rmdir path*/
      //alarm(1);
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t path[cmd_size+1];
      int n;
      if((n=fread(path, 1, cmd_size, stdin))!=cmd_size) return 0;
      //alarm(0);
      path[n]=0;
      fprintf(stderr,"rmdir '%s' returns: %d\n", path, stfs_rmdir(blocks, path));
      break;
    };
    case('o'): { /*open flags path */
      int oflags;
      //alarm(1);
      if(fscanf(stdin, "%d ", &oflags)!=1) return 0;
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t path[cmd_size+1];
      int n;
      if((n=fread(path, 1, cmd_size, stdin))!=cmd_size) return 0;
      //alarm(0);
      path[n]=0;
      fprintf(stderr,"open '%s' %d returns: %d\n", path, oflags, stfs_open(path, oflags, blocks));
      break;
    };
    case('w'): { /*write fd buf*/
      int fd;
      //alarm(1);
      if(fscanf(stdin, "%d ", &fd)!=1) return 0;
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      //alarm(0);
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t buf[cmd_size+1];
      int n;
      for(n=0;n<cmd_size;n++) buf[n]=n%256;
      fprintf(stderr,"write %dB -> %d returns: %d\n", cmd_size, fd, stfs_write(fd, buf, cmd_size, blocks));
      break;
    };
    case('r'): { /*read fd size*/
      int fd;
      //alarm(1);
      if(fscanf(stdin, "%d ", &fd)!=1) return 0;
      if(fscanf(stdin, "%d", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      //alarm(0);
      uint8_t buf[cmd_size+1];
      int ret;
      fprintf(stderr,"read %dB from %d returns: %d\n", cmd_size, fd, (ret=stfs_read(fd, buf, cmd_size, blocks)));
      //if(ret>0) dump(buf,ret);
      break;
    }
    case('s'): { /*seek fd off whence*/
      int fd, off, whence;
      //alarm(1);
      if(fscanf(stdin, "%d %d %d", &fd, &off, &whence)!=3) return 0;
      //alarm(0);
      fprintf(stderr,"seek %d %d %d returns: %d\n", fd, off, whence, stfs_lseek(fd, off, whence));
      break;
    };
    case('c'): { /*close fd */
      int fd;
      //alarm(1);
      if(fscanf(stdin, "%d", &fd)!=1) return 0;
      //alarm(0);
      fprintf(stderr,"close %d returns: %d\n", fd, stfs_close(fd, blocks));
      break;
    };
    case('d'): { /*unlink path */
      //alarm(1);
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t path[cmd_size+1];
      int n;
      if((n=fread(path, 1, cmd_size, stdin))!=cmd_size) return 0;
      //alarm(0);
      path[n]=0;
      fprintf(stderr,"unlink '%s' returns: %d\n", path, stfs_unlink(blocks, path));
      break;
    };
    case('t'): { /*truncate size path */
      int size;
      //alarm(1);
      if(fscanf(stdin, "%d ", &size)!=1) return 0;
      if(fscanf(stdin,"%d ", &cmd_size)!=1) return 0;
      if(cmd_size>1024*1024 || cmd_size<0) return 0;
      uint8_t path[cmd_size+1];
      int n;
      if((n=fread(path, 1, cmd_size, stdin))!=cmd_size) return 0;
      //alarm(0);
      path[n]=0;
      fprintf(stderr,"truncate %d '%s' returns: %d\n", size, path, stfs_truncate(path, size, blocks));
      break;
    };
    case('\n'): break;
    default: return 0;
    }
  }

  return 0;
}

void dump_info(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);

int main(void) {
  _main();
  dump_info(blocks);
  int fd;
  fd=open("test.img", O_RDWR | O_CREAT | O_TRUNC, 0666 );
  fprintf(stderr, "[i] dumping fs to fd %d\n", fd);
  write(fd,blocks, sizeof(blocks));
  close(fd);
  return 0;
}
