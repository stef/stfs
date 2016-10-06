#!/usr/bin/env python

import random
import sys
from sh import afl
from stfs import Chunk
from binascii import hexlify

ops = ('m', 'l', 'n', 'x', 'o', 'w', 'r', 's', 'c', 'd', 't', 'c', 'c', 'c' )
dirs=set([''])
files=set()
fds=[None, None, None, None, None]

def random_path():
    path = random.choice(tuple(dirs))
    name = ''.join([chr(random.randint(65,66)) for _ in xrange(random.randint(1,2))])
    path = path + "/" + name
    return path

def getimg():
    with open("test.img", 'r') as fd:
        return fd.read()

def split_by_n( seq, n ):
    """A generator to divide a sequence into chunks of n units.
       src: http://stackoverflow.com/questions/9475241/split-python-string-every-nth-character"""
    while seq:
        yield seq[:n]
        seq = seq[n:]

def check():
    img = getimg()
    objects = {1: {'oid': 1, 'path': '/'}}
    empty=[0,0,0,0,0,0]
    used=[0,0,0,0,0,0]
    deleted=[0,0,0,0,0,0]

    for b, block in enumerate(split_by_n(img,128*1024)):
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
            elif chunk.type == "Inode":
                used[b]+=1
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
                    else:
                        nsize = chunk.node.inode.bits.name_len
                        name = chunk.node.inode.name[:nsize]
                        objects[chunk.node.inode.oid]['type']=chunk.node.inode.bits.type
                        objects[chunk.node.inode.oid]['parent']=chunk.node.inode.parent
                        objects[chunk.node.inode.oid]['size']=chunk.node.inode.size
                        objects[chunk.node.inode.oid]['name']=name
            elif chunk.type == "Empty":
                empty[b]+=1
            elif chunk.type == "Deleted":
                deleted[b]+=1

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
            print "dangling object", obj['path']
            sys.exit(0)
        # check if seq list could be valid
        #for i, seq in enumerate(sorted(obj['seq'])):
        #    if i!=seq:
        #        print "missing chunk %d, from" % i, obj
        if obj['type']==1: # files
            if (obj['size']/121)+1>len(obj['seq']) and obj['size']%121!=0:
               print "[x] only %d chunks (%d B) for %d bytes - %s" % (len(obj['seq']), len(obj['seq'])*121, obj['size'], obj['path'])
            elif (obj['size']<len(obj['seq'])):
               print "[i] %d chunks (%d B) for only %d bytes - %s" % (len(obj['seq']), len(obj['seq'])*121, obj['size'], obj['path'])
            else:
                print "[o] %d bytes - %s" % (obj['size'], obj['path'])
        else: # directories
            print "[ ] directory %s" % (obj['path'])

    print "free/used/deleted"
    for i in xrange(5):
        print empty[i], used[i], deleted[i]
    return objects

script=[]
if len(sys.argv)==2:
    with open(sys.argv[1],'r') as fd:
        script.extend(x.strip() for x in fd.readlines())

def xqt(cmd):
    try:
        ret = afl(_in='\n'.join(script)+'\n'+cmd+'\n')
    except:
        import traceback
        traceback.print_exc()
        print "[!] afl returned !0, cmd:", cmd
        sys.exit(0)

    retline=-1
    while ' returns: ' not in ret.stderr.split('\n')[retline]:
          retline-=1
    if ret.stderr.split('\n')[retline].strip()[-2:] != '-1':
        print cmd
        print ret.stderr.split('\n')[retline]
        script.append(cmd)
        with open("fuzz.script", "a") as fd:
            fd.write(cmd)
            fd.write('\n')
        op = cmd[0]
        if op == 'm' and ret.stderr.split('\n')[retline].endswith("returns: 0"):
            dirs.add(cmd.split()[-1])
        elif op == 'x' and ret.stderr.split('\n')[retline].endswith("returns: 0"):
            dirs.remove(cmd.split()[-1])
        elif op == 'o':
            fds[int(ret.stderr.split('\n')[retline].split()[-1])]=cmd.split()[-1]
            try:
                fds[int(ret.stderr.split('\n')[retline].split()[-1])]=cmd.split()[-1]
            except:
                pass
        elif op == 'c' and ret.stderr.split('\n')[retline].endswith("returns: 0"):
            fd = -1
            fd = int(cmd.split()[-1])
            try:
                fd = int(cmd.split()[-1])
            except:
                pass
            if fd>=0 and fd<5 and fds[fd]:
                files.add(fds[fd])
                fds[fd]=None
        elif op == 'd' and ret.stderr.split('\n')[retline].endswith("returns: 0"):
            files.remove(cmd.split()[-1])
        #print "files", files
        #print "dirs", dirs
        print "fds", fds
        objs=check()
        #print objs
        print "--------"

#for line in sys.stdin:
#    xqt(line.strip())
#sys.exit(0)

#for _ in xrange(50000): #random.randint(4,1000)):
while 1:
    op = random.choice(ops)
    if op == 'm':
        if random.randint(0,5)>0: continue
        #if len(dirs)>2: continue
        path=random_path()
        xqt("m %d %s" % (len(path), path))
    elif op == 'x':
        if len(dirs)<1: continue
        if random.randint(0,2)>0: continue
        path=random.choice(tuple(dirs))
        xqt("x %d %s" % (len(path), path))
    elif op == 'o':
        if len([x for x in fds if x])==len(fds): continue
        path=random_path()
        mode=random.choice((0, 64))
        xqt("o %d %d %s" % (mode, len(path), path))
    elif op == 'w':
        if len([x for x in fds if x])==0: continue
        fd=random.choice([i for i, x in enumerate(fds) if x])
        size=random.randint(0, 65535) # randomize this better also bigger ranges, rarely
        xqt("w %d %d" % (fd, size))
    elif op == 'r':
        if len([x for x in fds if x])==0: continue
        fd=random.choice([i for i, x in enumerate(fds) if x])
        size=random.randint(0, 65535) # randomize this better also bigger ranges, rarely
        xqt("r %d %d" % (fd, size))
    elif op == 's':
        if len([x for x in fds if x])==0: continue
        fd=random.choice([i for i, x in enumerate(fds) if x])
        off=random.randint(-65535, 65535) # randomize this better also bigger ranges, rarely
        whence=random.choice((0, 1, 2)) # randomize this better also bigger ranges, rarely
        xqt("s %d %d %d" % (fd, off, whence))
    elif op == 'c':
        if random.randint(0,3)>0: continue
        if len([x for x in fds if x])==0: continue
        fd=random.choice([i for i, x in enumerate(fds) if x])
        xqt("c %d" % fd)
    elif op == 'd':
        if len(files)<1: continue
        #if random.randint(0,2)>0: continue
        path=random.choice(tuple(files))
        xqt("d %d %s" % (len(path), path))
    elif op == 't':
        if len([x for x in files if x])==0: continue
        #if random.randint(0,2)>0: continue
        path=random.choice(tuple(files))
        off=random.randint(0, 65535) # randomize this better also bigger ranges, rarely
        xqt("t %d %d %s" % (off, len(path), path))


