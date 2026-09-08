#!/usr/bin/env python3
"""position_stls.py — translate binary STLs to explicit bed positions.
OrcaSlicer CLI --arrange stacks multi-STL inputs at center (overlap bug),
so we pre-place each model ourselves. Usage:
  python3 position_stls.py out_dir model1.stl model2.stl ...
Places models on a 3+2 grid over a 256mm plate with 10mm gaps, preserving Z.
"""
import struct, sys, os

def bbox_and_translate(data, tx, ty):
    n = struct.unpack("<I", data[80:84])[0]
    body = data[84:]
    minx=miny=minz=1e9; maxx=maxy=maxz=-1e9
    # pass 1: bbox
    for i in range(n):
        off = i*50
        for v in range(3):
            x,y,z = struct.unpack_from("<3f", body, off+12+12*v)
            minx=min(minx,x); maxx=max(maxx,x)
            miny=min(miny,y); maxy=max(maxy,y)
            minz=min(minz,z); maxz=max(maxz,z)
    cx,cy = (minx+maxx)/2, (miny+maxy)/2
    out = bytearray(data)
    ob = memoryview(out)[84:]
    for i in range(n):
        off = i*50
        for v in range(3):
            pos = off+12+12*v
            x,y,z = struct.unpack_from("<3f", ob, pos)
            struct.pack_into("<3f", ob, pos, x-cx+tx, y-cy+ty, z)
    return bytes(out), (maxx-minx, maxy-miny, maxz-minz)

def main():
    outdir = sys.argv[1]
    files = sys.argv[2:]
    os.makedirs(outdir, exist_ok=True)
    # grid: 3 on back row (y=190), 2 on front row (y=70); bed 256x256
    # 5-fidget pack layout (256 bed): rex back-left, hyperboloid mid,
    # rings back-right, dragon+snake front strips (order must match args)
    slots = [(55,150),(128,130),(190,155),(128,35),(128,68)]
    placed=[]
    for f,(tx,ty) in zip(files, slots):
        data=open(f,"rb").read()
        new,sizes = bbox_and_translate(data, tx, ty)
        name=os.path.basename(f).replace(".stl", "-pos.stl")
        open(os.path.join(outdir,name),"wb").write(new)
        placed.append((name, round(sizes[0]), round(sizes[1]), tx, ty))
        print(f"{name}: {sizes[0]:.0f}x{sizes[1]:.0f}x{sizes[2]:.0f}mm -> center ({tx},{ty})")
    print("CAUTION: if any model is wider than its slot, edit slots[]")

if __name__=="__main__":
    main()
