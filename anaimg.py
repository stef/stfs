#!/usr/bin/env python

import sys
from binascii import hexlify
from stfs import Chunk

dirs=set([''])
files=set()
objects = {1: {'oid': 1, 'path': '/'}}
empty=[0,0,0,0,0]
used=[0,0,0,0,0]
deleted=[0,0,0,0,0]

def getimg():
    with open(sys.argv[1], 'r') as fd:
        return fd.read()

img = getimg()

def split_by_n( seq, n ):
    """A generator to divide a sequence into chunks of n units.
       src: http://stackoverflow.com/questions/9475241/split-python-string-every-nth-character"""
    while seq:
        yield seq[:n]
        seq = seq[n:]

def dump_chunks(prev):
    if prev[0]=='d':
        print "data\t%3d blocks of %d [%s..%s]" % (len(prev[2]), prev[1], str(prev[2][:3])[1:-1], str(prev[2][-3:])[1:-1])
    elif prev[0]=='e':
        print "empty\t%3d blocks" % (prev[1])
    elif prev[0]=='x':
        print "deleted\t%3d blocks" % (prev[1])

for b, block in enumerate(split_by_n(img,128*1024)):
    print "[i] block", b
    prev = None
    for c, raw in enumerate(split_by_n(block,128)):
        chunk=Chunk.parse(raw)
        if chunk.type == "Data":
            used[b]+=1
            if chunk.node.dnode.oid not in objects:
                objects[chunk.node.dnode.oid]={'seq': [chunk.node.dnode.seq,],
                                               'oid': chunk.node.dnode.oid}
            else:
                if chunk.node.dnode.seq in objects[chunk.node.dnode.oid]['seq']:
                    print "[x] already seen seq", chunk.node.dnode.seq
                else:
                    objects[chunk.node.dnode.oid]['seq'].append(chunk.node.dnode.seq)
            if prev and prev[0]=='d' and prev[1]==chunk.node.dnode.oid:
                prev[2].append(chunk.node.dnode.seq)
            else:
                if prev: dump_chunks(prev)
                prev = ('d', chunk.node.dnode.oid, [chunk.node.dnode.seq])
        elif chunk.type == "Inode":
            used[b]+=1
            name = ""
            if chunk.node.inode.oid not in objects:
                nsize = chunk.node.inode.bits.name_len
                name = chunk.node.inode.name[:nsize]
                objects[chunk.node.inode.oid]={'seq': [],
                                               'oid': chunk.node.inode.oid,
                                               'type': chunk.node.inode.bits.type,
                                               'parent': chunk.node.inode.parent,
                                               'size': chunk.node.inode.size,
                                               'name': name}
            else:
                if 'parent' in objects[chunk.node.inode.oid]:
                    print "[x] double inode %s", chunk.node.inode
                    continue
                else:
                    nsize = chunk.node.inode.bits.name_len
                    name = chunk.node.inode.name[:nsize]
                    objects[chunk.node.inode.oid]['type']=chunk.node.inode.bits.type
                    objects[chunk.node.inode.oid]['parent']=chunk.node.inode.parent
                    objects[chunk.node.inode.oid]['size']=chunk.node.inode.size
                    objects[chunk.node.inode.oid]['name']=name

            if prev:
                dump_chunks(prev)
                prev = None
            print "inode:\t%s '%s' inode(%d) %dB parent: %d" % (
                "File" if (chunk.node.inode.bits.type==1) else "Directory",
                name,
                chunk.node.inode.oid,
                chunk.node.inode.size,
                chunk.node.inode.parent)
            prev = None
        elif chunk.type == "Empty":
            empty[b]+=1
            if prev and prev[0]=='e':
                prev[1]+=1
            else:
                if prev: dump_chunks(prev)
                prev = ['e', 1]
        elif chunk.type == "Deleted":
            deleted[b]+=1
            if prev and prev[0]=='x':
                prev[1]+=1
            else:
                if prev: dump_chunks(prev)
                prev = ['x', 1]
    if prev:
        dump_chunks(prev)

ls = []
for oid, obj in objects.items():
    if not 'name' in obj:
        if obj['oid']!=1:
            print "orphan\t%3d blocks (%dB) of %x [%s..%s]" % (
                len(obj['seq']),
                len(obj['seq'])*121,
                obj['oid'],
                str(obj['seq'][:3])[1:-1],
                str(obj['seq'][-3:])[1:-1])
        continue
    path=['/'+obj['name']]
    o=objects.get(obj['parent'])
    while o and o['oid']!=1:
        path.append('/'+o['name'])
        o=objects.get(o['parent'])
    obj['path']=''.join(reversed(path))
    if not o or o['oid']!=1:
        print "dangling object", obj['path'], hex(obj.get('parent'))
    # check if seq list could be valid
    #for i, seq in enumerate(sorted(obj['seq'])):
    #    if i!=seq:
    #        print "missing chunk %d, from" % i, obj
    if obj['type']==1: # files
        if (obj['size']/121)+1>len(obj['seq']) and obj['size']%121!=0:
           print "[x] only %d chunks (%d B) for %d bytes - %s" % (len(obj['seq']), len(obj['seq'])*121, obj['size'], obj['path'])
           print obj
        elif (obj['size']<len(obj['seq'])):
           print "[i] %d chunks (%d B) for only %d bytes - %s" % (len(obj['seq']), len(obj['seq'])*121, obj['size'], obj['path'])
        else:
            ls.append("%s %d" % (obj['path'], obj['size']))
    else: # directories
        ls.append("%s/" % (obj['path']))

print '\n'.join(sorted(ls))

print "free/used/deleted"
for i in xrange(5):
    print empty[i], used[i], deleted[i]
