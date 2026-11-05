import sys,struct,zlib,ctypes,hashlib
lzo=ctypes.CDLL("liblzo2.so.2")
ING="/mnt/user-data/attachments/0c8d0b28-9f17-5bdb-8394-7ae370b9a9e0/cbd81e27-INGE.bin"
ing=open(ING,"rb").read(); rp=ing[0x358000:0x658000]
raw=open("work/rootfs.cpio","rb").read()
F=["ino","mode","uid","gid","nlink","mtime","filesize","devmajor","devminor","rdevmajor","rdevminor","namesize","check"]
def parse(r):
    p=0;e=[]
    while True:
        if r[p:p+6]!=b'070701':break
        h={f:int(r[p+6+i*8:p+6+i*8+8],16) for i,f in enumerate(F)}
        ns=h["namesize"];nm=r[p+110:p+110+ns-1];do=p+110+ns+((-(110+ns))%4)
        d=r[do:do+h["filesize"]];e.append([h,nm,d]);p=do+h["filesize"]+((-h["filesize"])%4)
        if nm==b'TRAILER!!!':break
    return e
def emit(e):
    o=bytearray()
    for h,nm,d in e:
        o+=b'070701';x=dict(h);x["namesize"]=len(nm)+1;x["filesize"]=len(d)
        for f in F:o+=b"%08X"%(x[f]&0xffffffff)
        o+=nm+b'\0'+b'\0'*((-(110+len(nm)+1))%4)+d+b'\0'*((-len(d))%4)
    return bytes(o)
e=parse(raw);wrap=open("work/sdkencode.wrap","rb").read();so=open("work/h264tee.so","rb").read()
mino=max(h["ino"] for h,_,_ in e);tmpl=None
for x in e:
    if x[1]==b'usr/bin/sdkencode':tmpl=dict(x[0]);x[1]=b'usr/bin/sdkencode.real'
def mk(n,d,i):h=dict(tmpl);h["ino"]=i;h["nlink"]=1;h["filesize"]=len(d);h["namesize"]=len(n)+1;return[h,n,d]
tr=e.pop();e+=[mk(b'usr/bin/sdkencode',wrap,mino+1),mk(b'usr/lib/h264tee.so',so,mino+2),tr]
mod=emit(e);mod=mod+b'\0'*((-len(mod))%512)
# 999 compress, lzop framing (adler-D)
h=bytearray(rp[:56]);h[0:4]=bytes([0x89,0x4c,0x5a,0x4f]);out=bytearray(h);i=0;BS=0x40000
def c999(b):
    dst=ctypes.create_string_buffer(len(b)+len(b)//16+128);dl=ctypes.c_ulong(len(dst.raw));wk=ctypes.create_string_buffer(1<<20)
    lzo.lzo1x_999_compress(b,ctypes.c_ulong(len(b)),dst,ctypes.byref(dl),wk);return dst.raw[:dl.value]
while i<len(mod):
    blk=mod[i:i+BS];i+=BS;c=c999(blk);src=c if len(c)<len(blk) else blk
    out+=struct.pack(">I",len(blk))+struct.pack(">I",len(src))+struct.pack(">I",zlib.adler32(blk)&0xffffffff)+src
out+=struct.pack(">I",0)
L=len(out)
if L>0x300000: raise SystemExit("overflow 0x%x"%L)
final=bytearray(out);final[0:4]=struct.pack("<I",L);final+=b'\0'*(0x300000-len(final))
# simulate checkenv
clen=struct.unpack_from("<I",final,0)[0];sim=bytearray(final[:clen]);sim[0:4]=bytes([0x89,0x4c,0x5a,0x4f])
p=56;res=bytearray()
while True:
    dl=struct.unpack_from(">I",sim,p)[0];p+=4
    if dl==0:break
    sl=struct.unpack_from(">I",sim,p)[0];p+=8;b=bytes(sim[p:p+sl]);p+=sl
    if sl<dl:
        buf=ctypes.create_string_buffer(dl);ol=ctypes.c_ulong(dl);lzo.lzo1x_decompress_safe(b,ctypes.c_ulong(sl),buf,ctypes.byref(ol),None);res+=buf.raw[:ol.value]
    else:res+=b
assert bytes(res)==mod,"checkenv-sim roundtrip FAIL"
ingb=bytearray(ing);ingb[0x358000:0x658000]=bytes(final)
outp=sys.argv[1]
open(outp,"wb").write(bytes(ingb))
print("stream L=0x%x header-len-ok, checkenv-sim OK, size fits"%L)
print("%s sha256: %s"%(outp,hashlib.sha256(bytes(ingb)).hexdigest()))
