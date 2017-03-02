#ifndef STFS_H
#define STFS_H

#include <stdint.h>
#include <unistd.h>

#define CHUNK_SIZE 128
#define CHUNKS_PER_BLOCK 1024
#define NBLOCKS 6
#define DATA_PER_CHUNK (CHUNK_SIZE-7)
#define MAX_FILE_SIZE 65535
#define MAX_OPEN_FILES 4
#define MAX_DIR_SIZE 32

#define O_CREAT 64

#define E_NOFDS     0
#define E_EXISTS    1
#define E_NOTOPEN   2
#define E_INVFD     3
#define E_INVFP     4
#define E_TOOBIG    5
#define E_SHORTWRT  6
#define E_NOSEEKEOF 7
#define E_NOTFOUND  8
#define E_WRONGOBJ  9
#define E_NOCHUNK   10
#define E_NOEXT     11
#define E_RELPATH   12
#define E_NAMESIZE  13
#define E_FULL      14
#define E_BADCHUNK  15
#define E_VAC       16
#define E_INVNAME   17
#define E_OPEN      18
#define E_DELROOT   19
#define E_FDREOPEN  20
#define E_DANGLE    21

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

int opendir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, ReaddirCTX *ctx);
const Inode_t* readdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], ReaddirCTX *ctx);
int stfs_mkdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_rmdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_open(uint8_t *path, uint32_t oflag, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
off_t stfs_lseek(uint32_t fildes, off_t offset, int whence);
ssize_t stfs_write(uint32_t fildes, const void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
ssize_t stfs_read(uint32_t fildes, void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_close(uint32_t fildes, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_unlink(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_truncate(uint8_t *path, uint32_t length, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_init(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_geterrno(void);

uint32_t stfs_size(uint32_t fildes);

#endif //STFS_H
