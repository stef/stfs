#!/usr/bin/env python

import construct

# typedef struct Inode_Struct {
#   InodeType type :1;
#   int name_len :6;
#   int padding: 1;
#   uint16_t size;
#   uint32_t parent;
#   uint32_t oid;
#   uint8_t name[32];
#   uint8_t data[CHUNK_SIZE - 44];
# } __attribute((packed)) Inode_t;

Inode = construct.Struct(
    'bits'/construct.BitStruct(
        'type'/construct.Flag,
        "name_len"/construct.BitsInteger(6),
        construct.Padding(1)),
    "size"/construct.Int16ul,
    'parent'/construct.Int32ul,
    'oid'/construct.Int32ul,
    'name'/construct.String(length=32),
    'data'/construct.String(length=84),
)

# typedef struct Data_Struct {
#   uint16_t seq;
#   uint32_t oid;
#   uint8_t data[CHUNK_SIZE-8];
# } __attribute((packed)) Data_t;

Dnode = construct.Struct(
    'seq'/construct.Int16ul,
    'oid'/construct.Int32ul,
    'data'/construct.String(length=121),
)

# typedef enum {
#   Deleted          = 0x00,
#   Inode            = 0xAA,
#   Data             = 0xCC,
#   Empty            = 0xff
# } ChunkType;
ChunkType = construct.Enum(
    construct.Int8ul,
    Deleted = 0x00,
    Inode   = 0xAA,
    Data    = 0xCC,
    Empty   = 0xff)

# typedef struct Chunk_Struct {
#   ChunkType type :8;
#   union {
#     Inode_t inode;
#     Data_t data;
#   };
# } __attribute((packed)) Chunk;

Chunk = construct.Struct(
    "type"/ChunkType,
    "node"/construct.Union("dnode"/Dnode,
                           "inode"/Inode),
)
