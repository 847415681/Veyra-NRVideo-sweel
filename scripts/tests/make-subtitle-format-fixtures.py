"""Small authored subtitle fixtures; no media or dependency enters source Git.

The compressed and BlockDuration MKVs are independent Matroska writers so the
test does not depend on the same demuxer/muxer applying matching assumptions.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
p.add_argument('--dvd-fixture', type=Path, required=True, help='Authored bitmap fixture encoded as DVD subtitles')
a = p.parse_args()
root = a.output
root.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(Path(__file__).with_name('make-bitmap-subtitle-fixture.py')),
                str(root / 'bitmap.sup')], check=True)
(root / 'bitmap.pgs').write_bytes((root / 'bitmap.sup').read_bytes())


def element(tag, data):
    if isinstance(data, str):
        data = data.encode('utf8')
    if isinstance(data, int):
        data = data.to_bytes(max(1, (data.bit_length() + 7) // 8), 'big')
    length = len(data)
    size = next(n for n in range(1, 9) if length < (1 << (7*n)) - 1)
    return tag.to_bytes((tag.bit_length()+7)//8, 'big') + ((1 << (7*size)) | length).to_bytes(size, 'big') + data


sets = {}
data = (root / 'bitmap.sup').read_bytes()
while data:
    assert data[:2] == b'PG'
    pts, _, kind, size = struct.unpack('>IIBH', data[2:13])
    sets.setdefault(round(pts/90), bytearray()).extend(data[10:13+size])
    data = data[13+size:]


def matroska(name, compressed=False, duration=False, corrupt=False, languages=1):
    ebml = element(0x1A45DFA3, b''.join(element(t, v) for t, v in
        [(0x4286, 1), (0x42F7, 1), (0x42F2, 4), (0x42F3, 8),
         (0x4282, 'matroska'), (0x4287, 4), (0x4285, 2)]))
    info = element(0x1549A966, element(0x2AD7B1, 1000000) + element(0x4489, struct.pack('>d', 8000))
                   + element(0x4D80, 'subtitle fixture') + element(0x5741, 'subtitle fixture'))
    tracks = b''
    for index in range(1, languages+1):
        entry = b''.join(element(t, v) for t, v in [(0xD7,index),(0x73C5,index),(0x83,17),
            (0x86,'S_HDMV/PGS'),(0x22B59C,'zho' if index==1 else 'eng'),(0x536E,f'PGS {index}')])
        if compressed:
            entry += element(0x6D80, element(0x6240, element(0x5031,0) + element(0x5032,1)
                                            + element(0x5033,0) + element(0x5034,element(0x4254,0))))
        tracks += element(0xAE, entry)
    cluster = element(0xE7, 0)
    for time, payload in sets.items():
        if duration and time in (3000, 7000):
            continue  # Deliberately no later clear set.
        for index in range(1, languages+1):
            value = b'\x16\xff\xff\x00' if corrupt else bytes(payload)
            if compressed:
                value = zlib.compress(value)
            block = bytes([0x80 | index]) + struct.pack('>hB', time, 0) + value
            cluster += element(0xA0, element(0xA1,block) + (element(0x9B,2000) if duration else b''))
    segment = info + element(0x1654AE6B,tracks) + element(0x1F43B675,cluster)
    (root/name).write_bytes(ebml + element(0x18538067,segment))


matroska('compressed.mkv', compressed=True, languages=5)
matroska('plain.mkv', languages=5)
matroska('duration-only.mkv', duration=True)
matroska('corrupt.mkv', corrupt=True)

texts = {
    'microdvd.sub': '{1}{1}25.000\n{25}{75}First|Two\n{125}{175}Second\n',
    'mpl2.mpl': '[10][30]First|Two\n[50][70]Second\n',
    'subviewer.sub': '[INFORMATION]\n[TITLE]Test\n[END INFORMATION]\n[SUBTITLE]\n'
                     '00:00:01.00,00:00:03.00\nFirst[br]Two\n\n00:00:05.00,00:00:07.00\nSecond\n',
    'subviewer.sbv': '0:00:01.000,0:00:03.000\nFirst[br]Two\n\n0:00:05.000,0:00:07.000\nSecond\n',
    'spruce.stl': '$FontName = Arial\n00:00:01:00 , 00:00:03:00 , First|Two\n00:00:05:00 , 00:00:07:00 , Second\n',
    'sami.smi': '<SAMI>\n<HEAD><STYLE TYPE="text/css">.ENCC {Name: English; lang: en-US; SAMIType: CC;}</STYLE></HEAD>\n'
                '<BODY>\n<SYNC Start=1000><P Class=ENCC>First<br>Two\n<SYNC Start=3000><P Class=ENCC>&nbsp;\n'
                '<SYNC Start=5000><P Class=ENCC>Second\n<SYNC Start=7000><P Class=ENCC>&nbsp;\n</BODY></SAMI>\n',
    '中文 路径.srt': '1\n00:00:01,000 --> 00:00:03,000\n中文，第一行\nEnglish, second line\n\n'
                   '2\n00:00:05,000 --> 00:00:07,000\nSecond\n',
    'broken.sup': 'PG truncated',
    'missing.idx': '# VobSub index file, v7 (do not modify this line!)\n',
}
for name, text in texts.items():
    (root/name).write_text(text,encoding='utf8',newline='\n')
(root/'sami.sami').write_bytes((root/'sami.smi').read_bytes())
(root/'字幕.txt').write_bytes((root/'中文 路径.srt').read_bytes())
(root/'orphan.sub').write_bytes(b'\0\0\1\xba'+b'\0'*12)
with (root/'oversized.srt').open('wb') as oversized:
    oversized.truncate(17*1024*1024)

# VobSub stores DVD subtitle PES packets in .sub and language/timestamps in
# .idx. Each fixture language has its own substream ID and file positions.
probe = json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','s',
    '-show_packets','-show_streams','-show_data','-of','json',str(a.dvd_fixture)]))


def unhex(dump):
    return bytes.fromhex(''.join(line.split(':',1)[1].split('  ',1)[0].replace(' ','')
                                for line in dump.splitlines() if ':' in line))


payloads = [unhex(packet['data']) for packet in probe['packets']]
assert len(payloads) == 2
header = unhex(probe['streams'][0]['extradata']).decode('ascii').rstrip('\0')
ps = bytearray()
positions = {0:[],1:[]}
for at, payload in zip((1,5),payloads):
    for language in (0,1):
        positions[language].append(len(ps))
        pts = at*90000
        stamp = bytes([0x21|((pts>>29)&14),(pts>>22)&255,((pts>>14)&254)|1,(pts>>7)&255,((pts<<1)&254)|1])
        body = b'\x80\x80\x05'+stamp+bytes([0x20+language])+payload
        ps += bytes.fromhex('000001ba44000400040189c3f800') + b'\0\0\1\xbd'+struct.pack('>H',len(body))+body
idx = '# VobSub index file, v7 (do not modify this line!)\n'+header+'\n'
for language, name in ((0,'zh'),(1,'en')):
    idx += f'id: {name}, index: {language}\n'
    for time, position in zip((1,5),positions[language]):
        idx += f'timestamp: 00:00:{time:02d}:000, filepos: {position:09x}\n'
(root/'vobsub.idx').write_text(idx,encoding='ascii')
(root/'vobsub.sub').write_bytes(ps)
(root/'oversized-vobsub.sub').write_bytes(ps)
with (root/'oversized-vobsub.idx').open('wb') as oversized:
    oversized.truncate(17*1024*1024)
print(root)
