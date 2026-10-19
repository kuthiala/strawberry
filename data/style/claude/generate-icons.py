#!/usr/bin/env python3
# Generates the Claude theme's small stylesheet icons (chevrons, close cross) as anti-aliased PNGs, 1x and @2x, light and dark.
# PNG and not SVG: the macOS deps don't ship the Qt SVG image plugin, so SVGs in stylesheets render nothing.
# Usage: python3 data/style/claude/generate-icons.py data/style/claude
# Colours are the text_secondary tokens of ClaudeTheme (src/core/claudetheme.cpp).
import zlib, struct, sys, math
def png(path, w, h, px):
    raw=b''.join(b'\x00'+bytes(v for p in row for v in p) for row in px)
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))
def segdist(px,py,ax,ay,bx,by):
    dx,dy=bx-ax,by-ay; t=max(0,min(1,((px-ax)*dx+(py-ay)*dy)/(dx*dx+dy*dy)))
    return math.hypot(px-(ax+t*dx),py-(ay+t*dy))
def chevron(size, up, rgb):
    s=size/10.0; half=0.75*s  # stroke 1.5 px at 1x
    pts=[(2*s,3.5*s),(5*s,6.5*s),(8*s,3.5*s)]
    if up: pts=[(x,size-y) for x,y in pts]
    rows=[]; N=4
    for y in range(size):
        row=[]
        for x in range(size):
            cov=0
            for i in range(N):
                for j in range(N):
                    px=x+(i+.5)/N; py=y+(j+.5)/N
                    d=min(segdist(px,py,*pts[0],*pts[1]),segdist(px,py,*pts[1],*pts[2]))
                    cov+= d<=half
            row.append((*rgb,round(255*cov/(N*N))))
        rows.append(row)
    return rows
out=sys.argv[1]
for scheme,hexc in (('light','6F6B63'),('dark','A8A49A')):
    rgb=tuple(int(hexc[i:i+2],16) for i in (0,2,4))
    for name,up in (('down',False),('up',True)):
        png(f'{out}/chevron-{name}-{scheme}.png',10,10,chevron(10,up,rgb))
        png(f'{out}/chevron-{name}-{scheme}@2x.png',20,20,chevron(20,up,rgb))

def cross(size, rgb):
    s=size/10.0; half=0.65*s
    segs=[((2.5*s,2.5*s),(7.5*s,7.5*s)),((7.5*s,2.5*s),(2.5*s,7.5*s))]
    rows=[]; N=4
    for y in range(size):
        row=[]
        for x in range(size):
            cov=0
            for i in range(N):
                for j in range(N):
                    px=x+(i+.5)/N; py=y+(j+.5)/N
                    d=min(segdist(px,py,*a,*b) for a,b in segs)
                    cov+= d<=half
            row.append((*rgb,round(255*cov/(N*N))))
        rows.append(row)
    return rows
for scheme,hexc in (('light','6F6B63'),('dark','A8A49A')):
    rgb=tuple(int(hexc[i:i+2],16) for i in (0,2,4))
    png(f'{out}/close-{scheme}.png',10,10,cross(10,rgb))
    png(f'{out}/close-{scheme}@2x.png',20,20,cross(20,rgb))
