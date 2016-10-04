#ifndef STFS_H
#define STFS_H

#include <stdint.h>
#include <unistd.h>

#define CHUNK_SIZE 128
#define CHUNKS_PER_BLOCK 1024
#define NBLOCKS 5
#define DATA_PER_CHUNK (CHUNK_SIZE-7)
#define MAX_FILE_SIZE 65535
#define MAX_OPEN_FILES 4

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
  int name_len :6;
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

int opendir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path, ReaddirCTX *ctx);
const Inode_t* readdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], ReaddirCTX *ctx);
int stfs_mkdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_rmdir(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_open(uint8_t *path, int oflag, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
off_t stfs_lseek(int fildes, off_t offset, int whence);
ssize_t stfs_write(int fildes, const void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
ssize_t stfs_read(int fildes, void *buf, size_t nbyte, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_close(int fildes, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_unlink(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK], uint8_t *path);
int stfs_truncate(uint8_t *path, int length, Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);
int stfs_init(Chunk blocks[NBLOCKS][CHUNKS_PER_BLOCK]);

#endif //STFS_H
