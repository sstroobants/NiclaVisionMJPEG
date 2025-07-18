
import socket
from collections import defaultdict

PORT = 5005
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(('0.0.0.0', PORT))

frames = defaultdict(lambda: {'chunks': {}, 'total': None})

while True:
    data, addr = sock.recvfrom(8192)
    if len(data) < 8:
        continue
    ts = int.from_bytes(data[:4], 'little')
    chunk_idx = int.from_bytes(data[4:6], 'little')
    chunk_total = int.from_bytes(data[6:8], 'little')
    chunk_data = data[8:]

    print(f"ts={ts} chunk={chunk_idx} total={chunk_total} data={len(chunk_data)}")

    frame = frames[ts]
    # If we see the same chunk_idx twice for this frame, warn and skip
    if chunk_idx in frame['chunks']:
        print(f"Duplicate chunk {chunk_idx} for frame {ts}")
        continue

    frame['chunks'][chunk_idx] = chunk_data
    frame['total'] = chunk_total

    if len(frame['chunks']) == chunk_total:
        missing = [i for i in range(chunk_total) if i not in frame['chunks']]
        if missing:
            print(f"Missing chunks for frame {ts}: {missing}")
            del frames[ts]
            continue
        jpeg_bytes = b''.join(frame['chunks'][i] for i in range(chunk_total))
        filename = f'frame_{ts}.jpg'
        with open(filename, 'wb') as f:
            f.write(jpeg_bytes)
        print(f"Saved {filename} ({len(jpeg_bytes)} bytes, {chunk_total} chunks) header: {jpeg_bytes[:10].hex()}")
        del frames[ts]