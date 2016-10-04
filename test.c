#include "stfs.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

void dump(uint8_t *src, uint32_t len);
void dump_inode(const Inode_t *inode);
void dump_chunk(Chunk *chunk);

int main(void) {
  Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK];
  memset(blocks,0xff,sizeof(blocks));

  uint8_t testdir[]="/test";
  uint8_t testdir2[]="/test/test";
  uint8_t testdir3[]="/test/asdf";
  uint8_t testdir4[]="/test/zxcv";
  uint8_t testdir5[]="/test/qwer";
  uint8_t testdir6[]="/test/hjkl";
  uint8_t testfile[]="/test.txt";
  uint8_t testfile2[]="/test2.txt";
  uint8_t testfilebig[]="/huge.bin";
  uint8_t rootpath[]="/test";

  // geometry
  printf("[i] storage is: %.2fKB\n", sizeof(blocks)/1024.0);
  printf("[i] chunk is: %dB\n", sizeof(Chunk));
  printf("[i] inode is: %dB\n", sizeof(Inode_t));
  printf("[i] data is: %dB\n", sizeof(Data_t));
  printf("[i] block is: %.2fKB\n", sizeof(blocks[0])/1024.0);
  printf("[x] initializing\n");
  if(stfs_init(blocks)==-1) {
    return 1;
  };
  // testing mkdir
  printf("[?] mkdir %s, returns %d\n", testdir, stfs_mkdir(blocks, testdir));
  dump_chunk(&blocks[0][0]);
  dump((uint8_t*) blocks,128);

  dump_chunk(&blocks[0][1]);
  //dump((uint8_t*) &blocks[0][1],128);
  printf("[?] mkdir %s returns %d\n", testdir2, stfs_mkdir(blocks, testdir2));
  dump_chunk(&blocks[0][1]);
  //dump((uint8_t*) &blocks[0][1],128);

  dump_chunk(&blocks[0][2]);
  //dump((uint8_t*) &blocks[0][2],128);
  printf("[?] mkdir %s returns %d\n", testdir3, stfs_mkdir(blocks, testdir3));
  dump_chunk(&blocks[0][2]);
  //dump((uint8_t*) &blocks[0][2],128);

  printf("[?] mkdir %s returns %d\n", testdir4, stfs_mkdir(blocks, testdir4));
  printf("[?] mkdir %s returns %d\n", testdir5, stfs_mkdir(blocks, testdir5));
  printf("[?] mkdir %s returns %d\n", testdir6, stfs_mkdir(blocks, testdir6));
  printf("[?] mkdir %s returns %d\n", testdir3, stfs_mkdir(blocks, testdir3));

  // basic getdents aka ls /test
  ReaddirCTX ctx;
  opendir(blocks, rootpath, &ctx);
  const Inode_t *inode;
  while((inode=readdir(blocks, &ctx))!=0) {
    dump_inode(inode);
  }

  // testing rmdir
  dump_chunk(&blocks[0][0]);
  printf("[?] rmdir %s returns %d\n", testdir, stfs_rmdir(blocks, testdir));
  dump_chunk(&blocks[0][0]);
  dump_chunk(&blocks[0][1]);
  printf("[?] rmdir %s returns %d\n", testdir2, stfs_rmdir(blocks, testdir2));
  dump_chunk(&blocks[0][1]);

  // ls /test
  opendir(blocks, rootpath, &ctx);
  while((inode=readdir(blocks, &ctx))!=0) {
    dump_inode(inode);
  }

  // file op tests
  // open create?
  int fd=stfs_open(testfile, O_CREAT, blocks);
  printf("[?] open %s o_creat returns %d\n", testfile, fd);

  // write data
  uint8_t data0[256];
  int i, ret;
  for(i=0;i<sizeof(data0);i++) data0[i]=i;
  if((ret=stfs_write(fd, data0, 256, blocks))!=256) {
    // fail to write
    printf("[x] write 256 returns %d\n", ret);
  }
  // lseek tests
  printf("[i] lseek start: %ld\n", stfs_lseek(fd, 0, SEEK_SET));
  printf("[i] lseek end: %ld\n", stfs_lseek(fd, 0, SEEK_END));
  printf("[i] lseek mid: %ld\n", stfs_lseek(fd, -128, SEEK_CUR));
  printf("[i] lseek err: %ld\n", stfs_lseek(fd, 256, SEEK_CUR));
  printf("[i] lseek err: %ld\n", stfs_lseek(fd, -256, SEEK_CUR));
  stfs_lseek(fd, 0, SEEK_SET);

  // re-read data from yet unclosed file and verify it
  uint8_t data0r[256];
  memset(data0r,0,256);
  ret = stfs_read(fd, data0r, 256, blocks);
  if(ret!=256) {
    printf("[x] short write: %d\n", ret);
  }
  if(memcmp(data0, data0r, 256)!=0) {
    printf("[x] fail to compare saved file with original\n");
  } else {
    printf("[!] verified correctly saved file with original\n");
  }

  // also try short read only 64B
  memset(data0r,0,sizeof(data0r));
  stfs_lseek(fd, 0, SEEK_SET);
  ret = stfs_read(fd, data0r, 64, blocks);
  if(ret!=64) {
    printf("[x] short read: %d\n", ret);
  } else {
    dump(data0r, 256);
  }

  // also try short read only 64B but spanning eof
  memset(data0r,0,sizeof(data0r));
  stfs_lseek(fd, -1, SEEK_END);
  ret = stfs_read(fd, data0r, 64, blocks);
  if(ret!=1) {
    printf("[x] short read: %d\n", ret);
  } else {
    dump(data0r, 256);
  }

  // close fd
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  // reopen file
  fd=stfs_open(testfile, 0, blocks);
  printf("[i] fd after reopen %d\n", fd);

  // re-read file and verify with original data
  memset(data0r,0,256);
  ret = stfs_read(fd, data0r, 256, blocks);
  if(ret!=256) {
    printf("[x] short write: %d\n", ret);
  }
  if(memcmp(data0, data0r, 256)!=0) {
    printf("[x] fail to compare saved file with original\n");
  } else {
    printf("[!] verified correctly saved file with original\n");
  }
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  // test truncate
  stfs_truncate(testfile, 16, blocks);

  // try to read 256 of truncated to 16 bytes file
  fd=stfs_open(testfile, 0, blocks);
  printf("[i] fd after reopen %d\n", fd);

  memset(data0r,0,256);
  ret = stfs_read(fd, data0r, 256, blocks);
  if(ret!=16) {
    printf("[x] short write: %d\n", ret);
  } else {
    dump(data0r,sizeof(data0r));
  }
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  // unlink file
  stfs_unlink(blocks, testfile);

  // re-create file
  fd=stfs_open(testfile2, O_CREAT, blocks);
  printf("[?] open %s o_creat returns %d\n", testfile2, fd);
  // try to write to file 1 byte a time, not creating new chunks, but
  // updating the latest
  for(i=0;i<sizeof(data0);i++) {
    if((ret=stfs_write(fd, &data0[i], 1, blocks))!=1) {
      // fail to write
      printf("[x] write 1 returns %d\n", ret);
    }
  }
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  fd=stfs_open(testfile2, 0, blocks);
  printf("[i] fd after reopen %d\n", fd);

  // verify that the short writes also produce a valid file
  memset(data0r,0,256);
  ret = stfs_read(fd, data0r, 256, blocks);
  if(ret!=256) {
    printf("[x] short write: %d\n", ret);
  }
  if(memcmp(data0, data0r, 256)!=0) {
    printf("[x] fail to compare saved file with original\n");
  } else {
    printf("[!] verified correctly saved file with original\n");
  }
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  uint8_t howdy[]="hello world";
  fd=stfs_open(testfile2, 0, blocks);
  stfs_lseek(fd,16,SEEK_SET);
  stfs_write(fd, howdy, sizeof(howdy), blocks);
  stfs_lseek(fd,0,SEEK_SET);
  memset(data0r,0,256);
  ret = stfs_read(fd, data0r, 256, blocks);
  if(ret!=256) {
    printf("[x] short read: %d\n", ret);
  }
  dump(data0r,sizeof(data0r));
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  printf("[i] writing 64KB file\n");
  fd=stfs_open(testfilebig, O_CREAT, blocks);
  printf("[?] open %s o_creat returns %d\n", testfilebig, fd);

  // write data
  for(i=0;i<256;i++) {
    //printf("[x] %dth write", i);
    ret=stfs_write(fd, data0, i<255?256:255, blocks);
    if((i<255 && ret!=256) || (i==255 && ret!=255)) {
      // fail to write
      printf("256 returns %d\n", ret);
      break;
    }
  }
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  printf("[i] reading 64KB file\n");
  fd=stfs_open(testfilebig, 0, blocks);
  printf("[?] open %s returns %d\n", testfilebig, fd);

  // read data
  int cnt=0;
  for(i=0;i<256;i++) {
    //printf("[x] %dth read\n", i);
    ret = stfs_read(fd, data0r, i<255?256:255, blocks);
    if(i<255 && ret!=256) {
      printf("[x] short read: %d\n", ret);
    } else if(i==255) {
      printf("[x] last read: %d\n", ret);
    }
    if(ret>0) cnt+=ret;
    if(memcmp(data0, data0r, i<255?256:255)!=0) {
      printf("[x] fail to compare saved file with original\n");
    }
  }
  printf("[i] total read: %d\n", cnt);
  printf("[?] close returns %d\n",stfs_close(fd, blocks));

  fd=open("test.img", O_RDWR | O_CREAT | O_TRUNC, 0666 );
  printf("[i] dumping fs to fd %d\n", fd);
  write(fd,blocks, sizeof(blocks));
  close(fd);

  return 0;
}
