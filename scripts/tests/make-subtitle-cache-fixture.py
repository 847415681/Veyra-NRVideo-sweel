"""Five real PGS tracks whose decoded cue runs cross the historical 64 MiB cap.

75 MiB bounded compressed-cue representation, no video/GPU pressure. Independently
authored Matroska and PGS bytes are decoded by the product FFmpeg. Explicit output.
"""
from pathlib import Path
import struct, sys, zlib

def e(tag, data):
    if isinstance(data, str): data = data.encode('utf8')
    if isinstance(data, int): data = data.to_bytes(max(1, (data.bit_length()+7)//8), 'big')
    n = next(n for n in range(1, 9) if len(data) < (1 << (7*n))-1)
    return tag.to_bytes((tag.bit_length()+7)//8, 'big') + ((1 << (7*n)) | len(data)).to_bytes(n, 'big') + data

def segment(kind, payload):
    return struct.pack('>BH', kind, len(payload)) + payload

def display(seq):
    pcs = struct.pack('>HHBHBBBB',3840,2160,0x10,seq,0x80,0,0,1) + struct.pack('>HBBHH',0,0,0,200,1900)
    out = segment(0x16,pcs) + segment(0x17,struct.pack('>BBHHHH',1,0,0,0,3840,2160))
    out += segment(0x14,bytes([0,seq,0,16,128,128,0,1,235,128,128,255,2,16,128,128,255]))
    pixels = (bytes([1,2])*256 + b'\0\0')*192
    first = struct.pack('>HBB',0,seq,0x80) + (len(pixels)+4).to_bytes(3,'big') + struct.pack('>HH',512,192)
    out += segment(0x15,first+pixels[:65000])
    out += segment(0x15,struct.pack('>HBB',0,seq,0x40)+pixels[65000:])
    return out + segment(0x80,b'')

ebml = e(0x1A45DFA3,b''.join(e(t,v) for t,v in [(0x4286,1),(0x42F7,1),(0x42F2,4),(0x42F3,8),(0x4282,'matroska'),(0x4287,4),(0x4285,2)]))
info = e(0x1549A966,e(0x2AD7B1,1000000)+e(0x4489,struct.pack('>d',121000))+e(0x4D80,'Veyra cache regression')+e(0x5741,'Veyra cache regression'))
tracks = b''
for i in range(1,6):
    entry = b''.join(e(t,v) for t,v in [(0xD7,i),(0x73C5,i),(0x83,17),(0x86,'S_HDMV/PGS'),(0x536E,f'PGS cache {i}')])
    entry += e(0x6D80,e(0x6240,e(0x5031,0)+e(0x5032,1)+e(0x5033,0)+e(0x5034,e(0x4254,0))))
    tracks += e(0xAE,entry)
clusters = []
for seq in range(40):
    block_time = 1000 + seq*3000
    cluster = e(0xE7,block_time)
    for i in range(1,6):
        block = bytes([0x80|i])+struct.pack('>hB',0,0)+zlib.compress(display(seq))
        cluster += e(0xA0,e(0xA1,block)+e(0x9B,2000))
    clusters.append(e(0x1F43B675,cluster))
target = Path(sys.argv[1]); target.parent.mkdir(parents=True,exist_ok=True)
target.write_bytes(ebml+e(0x18538067,info+e(0x1654AE6B,tracks)+b''.join(clusters)))
print(target, target.stat().st_size)
