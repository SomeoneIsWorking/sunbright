#!/usr/bin/env python3
# Compare mean RGB of named boxes between the GX oracle and ngx PPMs (same present).
import sys
def load(p):
    f=open(p,'rb'); assert f.readline().strip()==b'P6'
    w,h=map(int,f.readline().split()); f.readline()
    d=f.read(w*h*3); return w,h,d
def mean(d,w,x0,y0,x1,y1):
    r=g=b=n=0
    for y in range(y0,y1):
        for x in range(x0,x1):
            i=(y*w+x)*3; r+=d[i]; g+=d[i+1]; b+=d[i+2]; n+=1
    return (r//n,g//n,b//n)
gx=sys.argv[1]; ng=sys.argv[2]
w,h,dg=load(gx); _,_,dn=load(ng)
print(f"{w}x{h}")
# boxes as fractions of WxH -> name:(fx0,fy0,fx1,fy1)
boxes={'sky_TL':(.02,.02,.18,.18),'sky_TR':(.82,.02,.98,.18),
       'cloud_BL':(.02,.80,.18,.98),'logo_center':(.40,.30,.60,.55),
       'sky_mid_left':(.02,.40,.14,.60)}
for nm,(a,b_,c,d) in boxes.items():
    x0,y0,x1,y1=int(a*w),int(b_*h),int(c*w),int(d*h)
    G=mean(dg,w,x0,y0,x1,y1); N=mean(dn,w,x0,y0,x1,y1)
    ratio=tuple(round((N[i]+1)/(G[i]+1),2) for i in range(3))
    print(f"  {nm:14s} GX={G} ngx={N} ngx/gx={ratio}")
