/* simple embedded flash filesystem */
/* test with `gcc -DSTFS_INLINE_TESTS -o stfs stfs.c 2>&1 && ./stfs | less` */
/* for how to use see main() */

/*
    stfs is a log-structured appending file system for embedded flash
    devices, that expose the flash mapped as memory.

  - for embedded flash in cortex-m3, where you have only a few
    4KB-128KB ereasable blocks. (this implementation only supports
    blocks all with the same size)

  - STFS provides only directories and files, but no other types like
    pipes, links, etc.

  - no file metadata like timestamps or access permissions.

  - filenames are max 32 bytes long, files max 64KB.

  - always reserves one empty block for vacuuming.

  - default blocksize is 128B with fs metadata included. unlike other
    flash filesystems where the blocksize is usually limited by 512B.

  - single threaded

   data structure

   chunk_size = 128
   chunks_per_block = 1024

   4 different chunk types are used:

   empty = 0xff (1B) irrelevant(all 0xff) (127B)
   inode (128B)- contain file meta information
     - chunktype (0xAA) (1B)
     - directory | file (1b)
     - size (2B)
     - parent_directory_obj_id (4B)
     - obj_id (4B)
     - name_len (6b)
     - name (32B)
     - data (84B)
   data (7B) - contain data
    - chunktype (0xCC) (1B)
    - seq_id (2B)
    - obj_id (4B)
    - data blob (chunksize-metasize)
   deleted = 0x00 (1B) irrelevant(all 0x00) (127B)

   inode with oid 1 is the root directory and virtual

 */

#include <stdint.h>
#include <string.h> // mem*
#include <stdio.h>  // printf
#include <stdlib.h> // random
#include <fcntl.h>
#include <unistd.h>

#define CHUNK_SIZE 128
#define CHUNKS_PER_BLOCK 1024
#define NBLOCKS 5
#define DATA_PER_CHUNK (CHUNK_SIZE-7)
#define MAX_FILE_SIZE 65535
#define MAX_OPEN_FILES 4

#define E_NOFDS     0
#define E_EXISTS    1
#define E_NOTOPEN   2
#define E_INVFD     3
#define E_INVFP     4
#define E_TOOBIG    5
#define E_SHORTWRT  6
#define E_NOSEEKEOF 7
#define E_NOSEEKSOF 8
#define E_NOTFOUND  9
#define E_WRONGOBJ  10
#define E_NOCHUNK   11
#define E_NOEXT     12
#define E_RELPATH   13
#define E_NAMESIZE  14
#define E_FULL      15
#define E_BADCHUNK  16
#define E_VAC       17
#define E_INVNAME   18
#define E_OPEN      19
#define E_DELROOT   20
#define E_FDREOPEN  21
#define E_DANGLE    22

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define VALIDFD(fd) if(validfd(fd)!=0) return -1;

#ifdef DEBUG_LEVEL
#define LOG(level, ...) if(DEBUG_LEVEL>=level) fprintf(stderr, ##__VA_ARGS__)
#else
#define LOG(level, ...)
#endif // DEBUG_LEVEL

typedef enum {
  Deleted          = 0x00,
  Inode            = 0xAA,
  Data             = 0xCC,
  Empty            = 0xff
} ChunkType;

typedef enum {
  Directory          = 0x00,
  File               = 0x01,
} InodeType;

typedef struct Inode_Struct {
  InodeType type :1;
  unsigned int name_len :6;
  int padding: 1;
  uint16_t size;
  uint32_t parent;
  uint32_t oid;
  uint8_t name[32];
  uint8_t data[CHUNK_SIZE - 44];
} __attribute((packed)) Inode_t;

typedef struct Data_Struct {
  uint16_t seq;
  uint32_t oid;
  uint8_t data[CHUNK_SIZE-7];
} __attribute((packed)) Data_t;

typedef struct Chunk_Struct {
  ChunkType type :8;
  union {
    Inode_t inode;
    Data_t data;
  };
} __attribute((packed)) Chunk;

typedef struct {
  uint32_t oid;
  uint32_t block;
  uint32_t chunk;
} ReaddirCTX;

typedef struct {
  char free :1;
  char idirty :1;
  char padding :6;
  Chunk ichunk;
  uint32_t fptr;
} STFS_File;

static STFS_File fdesc[MAX_OPEN_FILES];
static uint32_t errno;
static uint32_t reserved_block;

void dump(uint8_t *src, uint32_t len) {
  uint32_t i,j;
  for(i=0;i<len;i+=32) {
    for(j=0;j<32 && i+j<len;j++) {
      printf("%02x ", src[i+j]);
    }
    printf("\n");
  }
}

void dump_inode(const Inode_t *inode) {
    uint8_t tmpname[33];
    if(inode->name_len>32 || inode->name_len<1) {
      printf("[x] inode has invalide name size: %d\n", inode->name_len);
      printf("[i] chunk: %s inode(%d) %dB parent: %x\n",
             (inode->type==File)?"File":"Directory",
             inode->oid,
             inode->size,
             inode->parent
             );
      return;
    }
    memcpy(tmpname,inode->name, inode->name_len);
    tmpname[inode->name_len]=0;
    printf("[i] chunk: %s %s inode(%d) %dB parent: %x\n",
           (inode->type==File)?"File":"Directory",
           tmpname,
           inode->oid,
           inode->size,
           inode->parent
           );
}

void dump_chunk(Chunk *chunk) {
  switch(chunk->type) {
  case(Empty): { printf("[i] chunk: empty\n"); break; }
  case(Deleted): { printf("[i] chunk: deleted\n"); break; }
  case(Data): {
    printf("[i] chunk: data %d %d\n", chunk->data.oid, chunk->data.seq);
    dump(chunk->data.data,DATA_PER_CHUNK);
    break;
  }
  case(Inode): {
    dump_inode(&chunk->inode);
    break;
  }
  }
}

static int validfd(uint32_t fildes) {
  if(fildes<0 || fildes>=MAX_OPEN_FILES) {
    // fail invalid fildes
    LOG(1, "[x] invalid fd, %d\n", fildes);
    errno = E_INVFD;
    return -1;
  }
  if(fdesc[fildes].free!=0) {
    // fail not open
    LOG(1, "[x] unused fd, %d\n", fildes);
    errno = E_NOTOPEN;
    return -1;
  }
  return 0;
}

static const Chunk* find_inode_by_parent_fname(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK],
                         const uint32_t parent,
                         const uint8_t* fname,
                         uint32_t *block, uint32_t *chunk) {
  LOG(3, "[i] find_inode_by_parent_fname %x %s %d %d\n", parent, fname, *block, *chunk);
  uint32_t b;
  const uint32_t fsize=strlen((const char*) fname);
  for(b=0;b<NBLOCKS;b++) {
    if(b==reserved_block) continue;
    uint32_t c;
    for(c=0;c<CHUNKS_PER_BLOCK && blocks[b][c].type!=Empty;c++) {
      //fprintf(stderr, "[O] %d == %d '%s', '%s'\n", fsize, blocks[b][c].inode.name_len, fname, blocks[b][c].inode.name);
      if(blocks[b][c].type==Inode &&
         (blocks[b][c].inode.parent==parent) &&
         (fsize == blocks[b][c].inode.name_len) &&
         memcmp(fname, &blocks[b][c].inode.name, blocks[b][c].inode.name_len)==0) {
         *block=b;
         *chunk=c;
         //printf("asdf %x %s --- %s\n", &blocks[b][c], fname, blocks[b][c].inode.name);
         //dump_chunk(&blocks[b][c]);
         return &blocks[b][c];
         }
    }
  }
  return NULL;
}

static const Chunk* find_chunk(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK],
                         const ChunkType type,
                         const uint32_t oid,
                         const uint32_t parent,
                         const uint16_t seq,
                         uint32_t *block, uint32_t *chunk) {
  //printf("[i] find_chunk %x %x %x %x %d %d\n", type, oid, parent, seq, *block, *chunk);
  uint32_t b,c=*chunk;
  for(b=*block;b<NBLOCKS;b++) {
    if(b==reserved_block) continue;
    for(;c<CHUNKS_PER_BLOCK;c++) {
      if(blocks[b][c].type==type && (
              // for inodes we match oids
              (type==Inode && oid!=0 && blocks[b][c].inode.oid==oid) ||
              // for inodes we match or parents
              (type==Inode && parent!=0 && blocks[b][c].inode.parent==parent) ||
              // for data we match oid and seq
              (type==Data && seq!=0xffff && blocks[b][c].data.oid==oid && blocks[b][c].data.seq==seq) ||
              // for data we match only oid
              (type==Data && seq==0xffff && blocks[b][c].data.oid==oid) ||
              // empty and deleted we match easily
              (type==Empty || type==Deleted) )) {
        *block=b;
        *chunk=c;
        return &blocks[b][c];
      }
      if(type!=Empty && blocks[b][c].type==Empty) break;
    }
    c=0;
  }
  return NULL;
}

static uint32_t oid_by_path(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, uint32_t *b, uint32_t *c) {
  LOG(3, "[i] oid_by_path %s\n", path);
  if(path[0]==0) { // root directory virtual path is 0 size
    return 1; // oid = 1
  }
  if(path[0]!='/') {
    // fail is not absolute path
    //printf("[x] fail is not absolute path\n");
    errno = E_RELPATH;
    return 0;
  }

  uint32_t i;
  uint8_t* ptr=path+1;
  uint32_t parent=1; // start with root dir

  for(i=1;path[i]!=0;i++) {
    if(path[i]=='/') {
      path[i]=0;
      const uint32_t psize = strlen((char*) ptr);
      if(psize==0 || psize>32) {
        // directory name size is <0 or >32
        LOG(1, "[x] directory name size is <0 or >32\n");
        path[i]='/'; // restore path
        errno = E_NAMESIZE;
        return 0;
      }
      LOG(3, "[i] looking for child named: %s\n", ptr);
      if(find_inode_by_parent_fname(blocks, parent, ptr,b,c)==NULL) {
        // fail no such directory
        //printf("[x] find inode by parent/fname");
        path[i]='/'; // restore path
        errno = E_NOTFOUND;
        return 0;
      }
      parent=blocks[*b][*c].inode.oid;
      path[i]='/'; // restore path
      ptr=&path[i+1]; // advance start ptr
    }
  }

  const uint32_t psize = strlen((char*) ptr);
  if(psize==0 || psize>32) {
    // directory name size is <0 or >32
    errno = E_NAMESIZE;
    return 0;
  }
  if(find_inode_by_parent_fname(blocks, parent, ptr,b,c)==NULL) {
    // fail no such directory
    errno = E_NOTFOUND;
    return 0;
  }
  return blocks[*b][*c].inode.oid;
}

static int write_chunk(void *dst, void *src, uint32_t size) {
  if(size!=sizeof(Chunk)) {
    LOG(1, "[x] Bad chunk size: %d\n", size);
    errno = E_BADCHUNK;
    return -1;
  }
  memcpy(dst, src, size);
  return 0;
}

int vacuum(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  uint32_t i, b,c, candidate_reclaim=0, used[NBLOCKS], unused[NBLOCKS], deleted[NBLOCKS];
  int candidate=-1;
  for(i=0;i<NBLOCKS;i++) { used[i]=unused[i]=deleted[i]=0; }
  LOG(2, "[i] Block stats\n");
  for(b=0;b<NBLOCKS;b++) {
    for(c=0;c<CHUNKS_PER_BLOCK;c++) {
      switch(blocks[b][c].type) {
      case(Empty): { unused[b]++; break; }
      case(Deleted): { deleted[b]++; break; }
      default: { used[b]++; break; }
      }
    }
    if((unused[b]+deleted[b])>candidate_reclaim) {
      candidate=b;
      candidate_reclaim=(unused[b]+deleted[b]);
    } else if((unused[b]+deleted[b])>(candidate_reclaim*9)/10 && random()%4==0) {
      candidate=b;
      candidate_reclaim=(unused[b]+deleted[b]);
    }
  }
  for(b=0;b<NBLOCKS;b++) {
    LOG(2, "\t%d %4d %4d %4d\n", b, unused[b], used[b], deleted[b]);
  }
  if(candidate==-1) {
    // fail
    LOG(1, "[x] vacuum reserved: %d candidate: %d\n", reserved_block, candidate);
    errno = E_VAC;
    return -1;
  }
  LOG(2, "[i] vacuuming from %d to %d\n", candidate, reserved_block);
  i=0;
  for(c=0;c<CHUNKS_PER_BLOCK;c++) {
    if(blocks[candidate][c].type==Inode || blocks[candidate][c].type==Data) {
      write_chunk(&blocks[reserved_block][i++], &blocks[candidate][c], sizeof(Chunk));
    }
  }
  // erase candidate
  memset(&blocks[candidate],0xff,CHUNKS_PER_BLOCK*CHUNK_SIZE);
  reserved_block=candidate;
  return 0;
}

static int store_chunk(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], Chunk *chunk) {
  uint32_t b=0, c=0;
  //printf("[i] store_chunk\n");
  if(find_chunk(blocks, Empty, 0, 0, 0, &b, &c)==NULL) {
        // no no free chunk found try to vacuum
        if(vacuum(blocks)!=0) {
          // failed vacuuming filesystem is full
          LOG(1, "[!] device is full\n");
          errno = E_FULL;
          return -1;
        }
        // vacuum successful
        b=c=0;
        if(find_chunk(blocks, Empty, 0, 0, 0, &b, &c)==NULL) {
          // fail no empty chunk found - should be impossible,
          // since we just vacuumed
          LOG(1, "[!] has no free chunk! even after vacuuming!\n");
          errno = E_FULL;
          return -1;
        }
  }
  //printf("[i] storing to %d %d\n", b,c);
  return write_chunk(&blocks[b][c], chunk, sizeof(*chunk));
}

static uint8_t is_oid_available(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], const uint32_t oid) {
  uint32_t b,c;
  if (oid < 2) return 0;
  for(b=0;b<NBLOCKS;b++) {
    if(b==reserved_block) continue;
    for(c=0;c<CHUNKS_PER_BLOCK;c++) {
      if(blocks[b][c].type==Inode && blocks[b][c].inode.oid == oid) return 0;
    }
  }
  return 1;
}

static uint32_t new_oid(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  uint32_t b,c;
  for(b=0;b<NBLOCKS;b++) {
    if(b==reserved_block) continue;
    for(c=0;c<CHUNKS_PER_BLOCK;c++) {
      if(blocks[b][c].type==Inode) {
        uint32_t oid = blocks[b][c].inode.oid + 1;
        if (is_oid_available(blocks, oid)) return oid;
      }
    }
  }
  // if execution reaches this, we ran out of OIDs or the FS is empty
  return 2;
}

static void del_chunk(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], const uint32_t b, const uint32_t c) {
  Chunk chunk;
  memset(&chunk,0,sizeof(chunk));
  chunk.type=Deleted;
  write_chunk(&blocks[b][c], &chunk, sizeof(chunk));
}


int opendir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, ReaddirCTX *ctx) {
  memset((uint8_t*) ctx,0,sizeof(*ctx));
  const uint32_t last=strlen((char*) path)-1;
  uint32_t oid, b=0, c=0;
  if(path[last]=='/') {
    path[last]=0;
    oid = oid_by_path(blocks, path,&b,&c);
    path[last]='/';
  } else {
    oid = oid_by_path(blocks, path,&b,&c);
  }
  if(oid==0) {
    // fail path not found
    return -1;
  }
  LOG(3, "[i] oid directory %x\n", oid);
  ctx->oid=oid;
  ctx->block=0;
  ctx->chunk=0;
  return 0;
}

const Inode_t* readdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], ReaddirCTX *ctx) {
  const Chunk *chunk=find_chunk(blocks, Inode, 0, ctx->oid, 0, &(ctx->block), &(ctx->chunk));
  if(chunk==NULL) return NULL;
  if(ctx->chunk+1>=CHUNKS_PER_BLOCK) {
    ctx->block++;
    ctx->chunk=0;
  } else {
    ctx->chunk++;
  }
  return &chunk->inode;
}

static uint8_t* split_path(uint8_t *path) {
  uint32_t i;
  uint8_t* ptr=NULL;
  for(i=0;path[i]!=0 && i<(2^32)-1;i++) {
    if(path[i]=='/') ptr=&(path[i]);
  }
  if(ptr==NULL) {
    // fail, not an absolute path
    errno = E_RELPATH;
    return NULL;
  }
  *ptr=0; // terminate path
  return ptr+1;
}

static int create_obj(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, Chunk *chunk) {
  int ret=0;
  uint8_t *fname=split_path(path);
  if(fname==NULL) {
    errno=E_INVNAME;
    return -1;
  }
  if(chunk==NULL) {
    errno=E_NOCHUNK;
    ret=-1;
    goto exit;
  }
  if(memcmp(fname, "..", 3)==0 ||
     memcmp(fname, ".", 2)==0 ||
     fname[0]==0) {
    errno = E_INVNAME;
    ret=-1;
    goto exit;
  }

  //printf("[i] split path: '%s' fname: '%s'\n", path, fname);
  uint32_t b=0,c=0;
  const uint32_t parent=oid_by_path(blocks, path,&b,&c);
  if(parent==0) {
    LOG(1, "[x] '%s' not found by oid\n", path);
    // fail no such directory
    errno = E_NOTFOUND;
    ret=-1;
    goto exit;
  }

  // check if object already exists
  ReaddirCTX ctx={.oid=parent,.block=0,.chunk=0};
  const Inode_t *inode;
  const uint32_t fsize=strlen((char*) fname);
  while((inode=readdir(blocks, &ctx))!=0) {
    if(fsize==inode->name_len &&
       memcmp(inode->name, fname, inode->name_len)==0) {
      // fail parent has already a child named fname
      LOG(1, "[x] '%s' has already a child %s\n", path, fname);
      errno = E_EXISTS;
      ret=-1;
      goto exit;
    }
  }

  LOG(3, "[i] parent inode: %x\n", parent);
  const uint32_t nsize = strlen((char*) fname);
  if(nsize>32 || nsize <1) {
    // fail name not valid size
    LOG(1, "invalid fname size\n");
    errno = E_NAMESIZE;
    ret=-1;
    goto exit;
  }

  chunk->inode.parent=parent;
  chunk->inode.name_len=nsize;
  memcpy(chunk->inode.name, fname, nsize);
 exit:
  fname[-1]='/'; // recover from split_path
  return ret;
}

int stfs_mkdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path) {
  LOG(2, "[x] mkdir %s\n", path);

  Chunk chunk;
  memset(&chunk,0,sizeof(chunk));
  chunk.type=Inode;
  chunk.inode.type=Directory;
  chunk.inode.size=0;
  chunk.inode.oid=new_oid(blocks);
  //printf("[i] new oid: 0x%x\n", chunk.inode.oid);

  if(create_obj(blocks, path, &chunk)==-1) {
    // fail
    LOG(1, "[x] create obj failed\n");
    return -1;
  }

  // store chunk
  if(store_chunk(blocks, &chunk)==-1) {
    // fail to store chunk
    LOG(1, "failed to store chunk\n");
    return -1;
  }
  return 0;
}

int stfs_rmdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path) {
  uint32_t b=0, c=0;
  const uint32_t self=oid_by_path(blocks, path, &b, &c);
  if(self==0) {
    LOG(1, "[x] path doesn't exist '%s'\n", path);
    // fail no such directory
    return -1;
  }
  if(self==1) {
    LOG(1, "[x] can't delete /\n");
    // fail no such directory
    errno = E_DELROOT;
    return -1;
  }
  // check if self is indeed a directory
  if(blocks[b][c].type==Inode && blocks[b][c].inode.type!=Directory) {
    // fail
    LOG(1, "[x] path '%s' is not a directory\n", path);
    return -1;
  }

  // check if directory is empty
  ReaddirCTX ctx={.oid=self, .block=0, .chunk=0};
  if(readdir(blocks, &ctx)!=0) {
    // fail directory is not empty
    LOG(1, "[x] directory '%s' is not empty\n", path);
    return -1;
  }

  // del chunk
  del_chunk(blocks, b, c);
  return 0;
}

int stfs_geterrno(void) {
  return errno;
}

int stfs_open(uint8_t *path, uint32_t oflag, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  // oflag maybe: O_RDONLY O_RDWR O_WRONLY O_SYNC(caching?) O_EXCL
  // oflags: O_APPEND O_CREAT O_TRUNC(seek)

  // find free fdesc
  uint32_t fd;
  for(fd=0;fd<MAX_OPEN_FILES;fd++) {
    if(fdesc[fd].free!=0) break;
  }
  if(fd>=MAX_OPEN_FILES) {
    // fail no free file descriptors available
    errno = E_NOFDS;
    return -1;
  }

  memset(&fdesc[fd], 0xff, sizeof(STFS_File));

  if(oflag == O_CREAT) {
    // create file

    // check if file doesn't exist
    uint32_t b=0, c=0;
    const uint32_t self=oid_by_path(blocks, path, &b, &c);
    if(self!=0) {
      LOG(1, "[x] path already exists '%s'\n", path);
      // fail no such directory
      errno = E_EXISTS;
      return -1;
    }

    if(create_obj(blocks, path, &fdesc[fd].ichunk)==-1) {
      // fail
      LOG(1, "[x] create obj failed\n");
      return -1;
    }
    uint32_t i;
    for(i=0;i<MAX_OPEN_FILES;i++) {
      if(i==fd ||fdesc[i].free!=0) continue;
      if(fdesc[i].ichunk.inode.name_len == fdesc[fd].ichunk.inode.name_len &&
         fdesc[i].ichunk.inode.parent == fdesc[fd].ichunk.inode.parent &&
         memcmp(fdesc[i].ichunk.inode.name, fdesc[fd].ichunk.inode.name, fdesc[fd].ichunk.inode.name_len)==0) {
        LOG(1, "[x] double open\n");
        errno = E_FDREOPEN;
        return -1;
      }
    }

    fdesc[fd].idirty=1;
    fdesc[fd].free=0;
    fdesc[fd].fptr=0;
    fdesc[fd].ichunk.type=Inode;
    fdesc[fd].ichunk.inode.type=File;
    fdesc[fd].ichunk.inode.size=0;
    fdesc[fd].ichunk.inode.oid=new_oid(blocks);

    if(store_chunk(blocks, &fdesc[fd].ichunk)==-1) {
      return -1;
    }

    return fd;
  } else if(oflag == 0) {
    uint32_t b=0, c=0;
    const uint32_t self=oid_by_path(blocks, path, &b, &c);
    if(self==0) {
      LOG(1, "[x] path not found '%s'\n", path);
      // fail no such file
      return -1;
    }
    if(self==1 || blocks[b][c].inode.type!=File) {
      LOG(1, "[x] cannot open directory '%s'\n", path);
      errno = E_OPEN;
      // fail no such file
      return -1;
    }
    fdesc[fd].idirty=0;
    fdesc[fd].free=0;
    fdesc[fd].fptr=0;
    memcpy(&fdesc[fd].ichunk, &blocks[b][c], sizeof(Chunk));
    return fd;
  }
  return -1;
}

off_t stfs_lseek(uint32_t fildes, off_t offset, int whence) {
  VALIDFD(fildes);
  uint32_t newfptr=fdesc[fildes].fptr;
  switch(whence) {
  case(SEEK_SET): {newfptr=offset; break;}
  case(SEEK_CUR): {newfptr+=offset; break;}
  case(SEEK_END): {newfptr=fdesc[fildes].ichunk.inode.size+offset; break;}}
  if(newfptr<0) {
    // fail seek beyond sof
    LOG(1, "[x] cannot seek before start of file\n");
    errno = E_NOSEEKSOF;
    return -1;
  }
  if(newfptr>fdesc[fildes].ichunk.inode.size) {
    // fail seek beyond eof
    LOG(1, "[x] cannot seek beyond eof set\n");
    errno = E_NOSEEKEOF;
    return -1;
  }
  fdesc[fildes].fptr=newfptr;
  return newfptr;
}

ssize_t stfs_write(uint32_t fildes, const void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  // check if fildes is valid
  // before writing a chunk check if it changed
  // update inode if neccessary
  if(nbyte<1) return 0;
  if(buf==NULL) return 0;
  VALIDFD(fildes)
  if(fdesc[fildes].fptr+nbyte>MAX_FILE_SIZE) {
    // fail too big
    LOG(1, "[x] too big, %d\n", fdesc[fildes].fptr+nbyte);
    errno = E_TOOBIG;
    nbyte=MAX_FILE_SIZE-fdesc[fildes].fptr;
  }

  if(fdesc[fildes].fptr>fdesc[fildes].ichunk.inode.size) {
    // due to lseek pointing behind eof there would be holes if we
    // write to this position.
    LOG(1, "[i] todo 0xff extend then append existing data\n");
    LOG(1, "[x] only if seek allows it, but it won't\n");
    errno = E_INVFP;
    return -1;
  }

  uint32_t written=0;
  if(fdesc[fildes].fptr<=fdesc[fildes].ichunk.inode.size) {
    // append to end of file
    uint32_t b,c;
    Chunk chunk;
    if(fdesc[fildes].fptr<fdesc[fildes].ichunk.inode.size) {
      // we are overwriting some chunks, delete them all
      // this is most important for the case that the fs is full
      // then every chunk overwrite would trigger a full vacuum
      uint32_t startseq=(fdesc[fildes].fptr)/DATA_PER_CHUNK;
      uint32_t endseq=(fdesc[fildes].fptr+nbyte-1)/DATA_PER_CHUNK;
      LOG(1,"[.] %d %d\n",startseq, endseq);
      uint32_t i;
      for(i=startseq;i<endseq;i++) {
        b=c=0;
        if(find_chunk(blocks, Data, fdesc[fildes].ichunk.inode.oid, 0, i, &b, &c)==NULL) {
          // fail, couldn't find chunk
          LOG(1, "[x] couldn't find chunk to overwrite: %d\n", i);
          errno = E_NOCHUNK;
          return -1;
        }
        // todo: sacrificing performance check if the overwritten
        // block changes in a way that needs deletion. otherwise we
        // could skip deleting it.
        del_chunk(blocks, b, c);
      }
      LOG(3,"[i] deleted %d chunks to be overwritten\n",endseq-startseq);
    }
    for(written=0;written<nbyte;) {
      memset(&chunk,0xff,sizeof(chunk));
      chunk.type=Data;
      chunk.data.oid=fdesc[fildes].ichunk.inode.oid;
      chunk.data.seq=(fdesc[fildes].fptr+written)/DATA_PER_CHUNK;

      LOG(3,"[i] writing chunk %d\n", chunk.data.seq);
      b=c=0;
      const uint32_t towrite=((nbyte-written>DATA_PER_CHUNK-(fdesc[fildes].fptr+written)%DATA_PER_CHUNK)?
                         DATA_PER_CHUNK-((fdesc[fildes].fptr+written)%DATA_PER_CHUNK):
                         (nbyte-written));
      if(find_chunk(blocks, Data, chunk.data.oid, 0, chunk.data.seq, &b, &c)!=NULL) {
        // found chunk, check if write is necessary, if so partial, or full?
        memcpy(chunk.data.data, &blocks[b][c].data.data, DATA_PER_CHUNK);
        memcpy(chunk.data.data+((fdesc[fildes].fptr+written)%DATA_PER_CHUNK), ((uint8_t*) buf)+written,towrite);
        uint32_t i;
        // can we update the chunk, or have to del,create a new one?
        for(i=0;i<sizeof(Chunk);i++) {
          if((((uint8_t*) &blocks[b][c])[i] & ((uint8_t*) &chunk)[i]) != ((uint8_t*) &chunk)[i]) {
            break;
          }
        }
        if(i<sizeof(Chunk)) { // we have to create a new chunk
          del_chunk(blocks, b, c);
          //dump_chunk(&chunk);
          if(store_chunk(blocks, &chunk)==-1) {
            // fail to store chunk
            LOG(1, "failed to store chunk\n");
            goto exit;
          }
        } else { // we can update the chunk \o/
          //dump_chunk(&chunk);
          write_chunk(&blocks[b][c], &chunk, sizeof(chunk));
        }
        written+=towrite;
      } else {
        // prepare chunk for writing
        memcpy(chunk.data.data, ((uint8_t*) buf)+written, (nbyte-written>DATA_PER_CHUNK)?DATA_PER_CHUNK:(nbyte-written));
        //dump_chunk(&chunk);
        if(store_chunk(blocks, &chunk)==-1) {
          // fail to store chunk
          LOG(1, "failed to store chunk\n");
          goto exit;
        }
        written+=(nbyte-written>DATA_PER_CHUNK)?DATA_PER_CHUNK:(nbyte-written);
      }
    }
  }
 exit:
  // update inode
  if(written+fdesc[fildes].fptr>fdesc[fildes].ichunk.inode.size) {
    // file grows update inode
    fdesc[fildes].ichunk.inode.size=written+fdesc[fildes].fptr;
    fdesc[fildes].idirty=1;
  }

  fdesc[fildes].fptr+=written;

  return written;
}

ssize_t stfs_read(uint32_t fildes, void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  if(nbyte<1) return 0;
  if(buf==NULL) return 0;
  VALIDFD(fildes)
  uint32_t read=0;
  uint32_t b,c;
  if(nbyte+fdesc[fildes].fptr>fdesc[fildes].ichunk.inode.size) {
    // read only as much there is available, not beyond eof
    nbyte=fdesc[fildes].ichunk.inode.size-fdesc[fildes].fptr;
    LOG(3, "[i] changed nbyte to %d, size is %d\n",nbyte, fdesc[fildes].ichunk.inode.size);
  }
  for(read=0;read<nbyte;) {
    uint32_t seq;
    const Chunk *chunk;
    seq=(fdesc[fildes].fptr+read)/DATA_PER_CHUNK;
    b=c=0;
    const uint32_t oid=fdesc[fildes].ichunk.inode.oid;
    if((chunk=find_chunk(blocks, Data, oid, 0, seq, &b, &c))!=NULL) {
      //printf("[i] found chunk %d\n",seq);
      uint32_t coff=(fdesc[fildes].fptr+read)%DATA_PER_CHUNK;
      //printf("[i] coff %d\n", coff);
      memcpy(((uint8_t*) buf)+read, chunk->data.data+coff, ((nbyte-read>DATA_PER_CHUNK)?(DATA_PER_CHUNK-coff):(nbyte-read)));
      read+=((nbyte-read>(DATA_PER_CHUNK-coff))?(DATA_PER_CHUNK-coff):(nbyte-read));
    } else {
      errno = E_NOCHUNK;
      return -1;
    }
  }
  fdesc[fildes].fptr+=read;
  return read;
}

static void del_chunks(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint32_t oid) {
  uint32_t b=0,c=0, n=0;
  const Chunk *chunk=find_chunk(blocks, Data, oid, 0, 0xffff, &b, &c);
  while(chunk) {
    del_chunk(blocks, b, c);
    b=c=0;
    chunk=find_chunk(blocks, Data, oid, 0,0xffff, &b, &c);
    n++;
  }
  LOG(3,"[i] deleted %d chunks from oid %x\n",n, oid);
}

int stfs_close(uint32_t fildes, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  VALIDFD(fildes)

  if(fdesc[fildes].idirty!=0) {
    // check if path is valid
    uint32_t b=0,c=0;
    const Chunk *chunk;
    if(fdesc[fildes].ichunk.inode.parent!=1) {
      chunk=find_chunk(blocks, Inode, fdesc[fildes].ichunk.inode.parent, 0,0, &b, &c);
      while(chunk && chunk->inode.parent!=1) {
        b=c=0;
        chunk=find_chunk(blocks, Inode, chunk->inode.parent, 0,0, &b, &c);
      }
      if(!chunk) {
        LOG(1, "[x] null chunk while resolving path\n");
        del_chunks(blocks, fdesc[fildes].ichunk.inode.oid);
        errno = E_DANGLE;
        return -1;
      }
      if(chunk->inode.type!=0) {
        LOG(1, "[x] invalid path\n");
        del_chunks(blocks, fdesc[fildes].ichunk.inode.oid);
        errno = E_DANGLE;
        return -1;
      }
      if(chunk->inode.parent!=1) {
        LOG(1, "[x] while resolving path\n");
        del_chunks(blocks, fdesc[fildes].ichunk.inode.oid);
        errno = E_DANGLE;
        return -1;
      }
    }
    // need to update inode chunk
    //LOG(3, "[i] tentatively updating inode\n");
    b=c=0;
    chunk=find_chunk(blocks, Inode, fdesc[fildes].ichunk.inode.oid, 0,0, &b, &c);
    if(chunk==NULL || chunk->inode.type!=File) { // if inode is dir, then file
                                                 // has been unlinked and a dir instead created
                                                 // between open and close
      // inode has been deleted, also delete all chunks
      del_chunks(blocks, fdesc[fildes].ichunk.inode.oid);
    } else if(memcmp(chunk,&fdesc[fildes].ichunk, sizeof(*chunk))!=0) {
      // invalidate old chunk
      LOG(3, "[i] deleting old inode at %d %d\n", b, c);
      del_chunk(blocks, b, c);
      // write new chunk
      store_chunk(blocks, &fdesc[fildes].ichunk);
    }
  }

  fdesc[fildes].free=1;
  fdesc[fildes].idirty=0;
  fdesc[fildes].fptr=0;

  return 0;
}

int stfs_unlink(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path) {
  uint32_t b=0, c=0;
  const uint32_t self=oid_by_path(blocks, path, &b, &c);
  if(self==0) {
    LOG(1, "[x] path doesn't exist '%s'\n", path);
    // fail no such file
    errno = E_NOTFOUND;
    return -1;
  }
  if(self==1 || blocks[b][c].inode.type!=File) {
    LOG(1, "[x] cannot unlink directory '%s'\n", path);
    errno = E_OPEN;
    // fail no such file
    return -1;
  }
  // check if self is indeed a file
  if(blocks[b][c].type==Inode && blocks[b][c].inode.type!=File) {
    // fail
    LOG(1, "[x] path '%s' is not a File\n", path);
    errno = E_WRONGOBJ;
    return -1;
  }

  uint32_t oid=blocks[b][c].inode.oid;

  // del inode chunk
  LOG(3, "[i] deleting inode chunk %d %d\n", b,c);
  del_chunk(blocks, b, c);

  // del data chunks
  const Chunk*chunk;
  b=c=0;
  while((chunk=find_chunk(blocks, Data, oid, 0, 0xffff, &b, &c))!=NULL) {
    LOG(3, "[i] deleting data chunk %d %d\n", b,c);
    del_chunk(blocks, b, c);
  }
  return 0;
}

int stfs_truncate(uint8_t *path, uint32_t length, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  LOG(2, "[i] truncating '%s' to %d\n", path, length);
  uint32_t b=0, c=0;
  const uint32_t self=oid_by_path(blocks, path, &b, &c);
  if(self==0) {
    LOG(1, "[x] path doesn't exist '%s'\n", path);
    // fail no such file
    errno = E_NOTFOUND;
    return -1;
  }
  if(self==1 || blocks[b][c].inode.type!=File) {
    LOG(1, "[x] cannot truncate directory '%s'\n", path);
    errno = E_OPEN;
    // fail no such file
    return -1;
  }
  // check if self is indeed a file
  if(blocks[b][c].type==Inode && blocks[b][c].inode.type!=File) {
    // fail
    LOG(1, "[x] path '%s' is not a File\n", path);
    errno = E_WRONGOBJ;
    return -1;
  }
  if(blocks[b][c].inode.size<length) {
    // fail
    LOG(1, "[x] path '%s' is not a File\n", path);
    errno = E_NOEXT;
    return -1;
  }

  // store new inode
  Chunk nchunk;
  memcpy(&nchunk, &blocks[b][c], sizeof(Chunk));
  nchunk.inode.size=length;
  store_chunk(blocks, &nchunk);

  uint32_t oid=blocks[b][c].inode.oid;

  // del inode chunk
  LOG(3, "[i] deleting inode chunk %d %d\n", b,c);
  del_chunk(blocks, b, c);

  // del data chunks
  const Chunk*chunk;
  uint32_t seq=length/DATA_PER_CHUNK;
  if(length%DATA_PER_CHUNK>0) {
    Chunk dchunk;
    b=c=0;
    if((chunk=find_chunk(blocks, Data, oid, 0, seq++, &b, &c))==NULL) {
      LOG(1, "[x] no chunk to truncate from found\n");
      // todo errno
      errno = E_NOCHUNK;
      return -1;
    }
    memcpy(&dchunk, &blocks[b][c], sizeof(Chunk));
    memset(&dchunk.data.data[length%DATA_PER_CHUNK], 0xff, (DATA_PER_CHUNK-length%DATA_PER_CHUNK));
    del_chunk(blocks, b, c);
    store_chunk(blocks, &dchunk);
  }
  b=c=0;
  while((chunk=find_chunk(blocks, Data, oid, 0, seq++, &b, &c))!=NULL) {
    LOG(3, "[i] deleting data chunk %d %d\n", b,c);
    del_chunk(blocks, b, c);
    b=c=0;
  }
  return 0;
}

int stfs_init(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  // check if at least one block is empty for migration
  uint32_t b, free, rcan, i;
  for(b=0,free=0;b<NBLOCKS;b++) {
    if(blocks[b][0].type==Empty) free++;
  }
  if(free==0) {
    // fail no empty blocks
    return -1;
  }
  rcan=random()%free;
  for(b=0,i=0;b<NBLOCKS;b++) {
    if(blocks[b][0].type==Empty) {
      if(i++==rcan) {
        reserved_block=b;
        break;
      }
    }
  }
  if(b>=NBLOCKS) {
    // fail no empty blocks
    return -1;
  }

  memset(fdesc,0xff,sizeof(fdesc));

  return 0;
}

void dump_info(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]) {
  uint32_t i, b,c, candidate_reclaim=0, used[NBLOCKS], unused[NBLOCKS], deleted[NBLOCKS];
  int candidate=-1, reserved=-1;
  for(i=0;i<NBLOCKS;i++) { used[i]=0; unused[i]=0; deleted[i]=0; }
  LOG(2, "[i] Block stats\n");
  for(b=0;b<NBLOCKS;b++) {
    for(c=0;c<CHUNKS_PER_BLOCK;c++) {
      switch(blocks[b][c].type) {
      case(Empty): { unused[b]++; break; }
      case(Deleted): { deleted[b]++; break; }
      default: { used[b]++; break; }
      }
    }
    if(unused[b]==CHUNKS_PER_BLOCK && reserved==-1) {
      reserved=b;
    } else if((unused[b]+deleted[b])>candidate_reclaim) {
      candidate=b;
      candidate_reclaim=(unused[b]+deleted[b]);
    }
  }
  for(b=0;b<NBLOCKS;b++) {
    fprintf(stderr, "\t%d %4d %4d %4d\n", b, unused[b], used[b], deleted[b]);
  }
  if(reserved==-1 || candidate==-1) {
    fprintf(stderr, "[x] vacuum reserved: %d candidate: %d\n", reserved, candidate);
  } else {
    fprintf(stderr, "[i] would be vacuuming from %d to %d\n", candidate, reserved);
  }
}
