"""Stand-in for the Android companion app, for testing previews in the emulator.

The emulator has no companion app, so the preview window never gets a frame.
This script connects to the emulator's phone simulator and answers the watch
the way OW Camera 2 does: it builds preview messages like PebbleHelper,
PebbleMessagePacker and PebblePixelCompressor (same header bytes, palette,
raw deflate and chunk splitting) and serves REQUEST_NEXT_FRAME and
REQUEST_NEXT_CHUNK. Keep it in step with those files - a harness that encodes
its own idea of the protocol only proves both sides share the same assumption.

Run it with the pebble-tool interpreter, which ships libpebble2 and Pillow:

    pebble install --emulator emery
    ~/.local/share/uv/tools/pebble-tool/bin/python tools/fake_phone.py emery --noise 80

--noise adds grain to the test image. Grain compresses badly, which is how a
real dithered camera frame looks to deflate; on emery --noise 80 yields
three-message color frames.
"""
import argparse
import json
import random
import struct
import threading
import time
import uuid
import zlib

from PIL import Image, ImageDraw
from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.websocket import WebsocketTransport
from libpebble2.services.appmessage import AppMessageService, ByteArray

APP_UUID = uuid.UUID("c187457d-3067-4062-8b1a-f8fde467b545")
KEY_CAPTURE = 1
KEY_PICTURE_TAKEN = 2
KEY_PREVIEW_DATA = 3
KEY_REQUEST_NEXT_FRAME = 4
KEY_CAPTURE_ACK = 5
KEY_REQUEST_NEXT_CHUNK = 7

# PebbleModel in PebbleImageConverter.kt, indexed by the watch's model enum
MODEL_SIZES = {0: (144, 168), 1: (144, 168), 2: (144, 168), 3: (144, 168), 4: (180, 180), 5: (200, 228)}
CHALK = 4
ACTION_BAR_WIDTH = 30
CHALK_ACTION_BAR_WIDTH = 28
# PebbleHelper.sendColorPreviewFrame: 8192 minus the first message's overhead
MAX_CHUNK_SIZE = 8192 - 21

# Any 16 Pebble colors will do; the phone picks the most frequent per frame
PALETTE_RGB = [(r * 85, g * 85, b * 85) for (r, g, b) in [
    (0, 0, 0), (3, 3, 3), (3, 0, 0), (0, 3, 0), (0, 0, 3), (3, 3, 0), (0, 3, 3), (3, 0, 3),
    (2, 2, 2), (1, 1, 1), (3, 2, 0), (0, 2, 3), (2, 0, 2), (2, 3, 1), (3, 1, 1), (1, 1, 3)]]


def gcolor8(rgb):
    r, g, b = (round(c / 85) for c in rgb)
    return (3 << 6) | (r << 4) | (g << 2) | b


def test_image(width, height, noise):
    img = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(img)
    bars = [(255, 0, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255),
            (0, 0, 255), (255, 0, 255), (255, 255, 255), (0, 0, 0)]
    bar_width = width / len(bars)
    for i, color in enumerate(bars):
        draw.rectangle([int(i * bar_width), 0, int((i + 1) * bar_width), height // 2], fill=color)
    for y in range(height // 2, height):
        v = int(255 * (y - height // 2) / (height // 2))
        draw.line([(0, y), (width, y)], fill=(v, v // 2, 255 - v))
    draw.ellipse([width // 4, height // 3, 3 * width // 4, 2 * height // 3], outline=(255, 255, 255), width=4)
    if noise > 0:
        rnd = random.Random(42)
        px = img.load()
        clamp = lambda c: max(0, min(255, int(c + rnd.gauss(0, noise))))
        for y in range(height):
            for x in range(width):
                r, g, b = px[x, y]
                px[x, y] = (clamp(r), clamp(g), clamp(b))
    return img


def deflate(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(bytes(data)) + compressor.flush()


def split_and_compress(data, num_chunks):
    # Mirrors PebblePixelCompressor.splitAndCompress
    chunks, size, offset = [], (len(data) + num_chunks - 1) // num_chunks, 0
    while offset < len(data):
        part = data[offset:offset + size]
        compressed = deflate(part)
        chunks.extend(split_and_compress(part, 2) if len(compressed) > MAX_CHUNK_SIZE else [compressed])
        offset += size
    return chunks


def build_frame(model, fmt, noise):
    width, height = MODEL_SIZES.get(model, MODEL_SIZES[1])
    width -= CHALK_ACTION_BAR_WIDTH if model == CHALK else ACTION_BAR_WIDTH
    img = test_image(width, height, noise)
    timestamp = struct.pack("<I", int(time.time()))

    if fmt == 0:
        # B&W: one uncompressed message, pixels as a continuous LSB-first bitstream
        px = img.convert("1").load()
        bits = bytearray((width * height + 7) // 8)
        for i in range(width * height):
            if px[i % width, i // width]:
                bits[i // 8] |= 1 << (i % 8)
        return [bytes([0]) + timestamp + bytes(bits)], "%dx%d B&W, %d bytes" % (width, height, len(bits))

    palette_img = Image.new("P", (1, 1))
    flat = [c for rgb in PALETTE_RGB for c in rgb]
    palette_img.putpalette(flat + [0] * (768 - len(flat)))
    indices = img.quantize(palette=palette_img, dither=Image.Dither.FLOYDSTEINBERG).tobytes()
    packed = bytearray((len(indices) + 1) // 2)
    for i, v in enumerate(indices):
        packed[i // 2] |= (v & 0x0F) << (4 if i % 2 == 0 else 0)  # first pixel in the high nibble

    full = deflate(packed)
    if len(full) <= MAX_CHUNK_SIZE:
        chunks = [full]
    else:
        ratio = len(full) / len(packed)
        chunks = split_and_compress(packed, int(ratio * len(packed) / MAX_CHUNK_SIZE) + 1)

    multi = len(chunks) > 1
    palette = bytes(gcolor8(c) for c in PALETTE_RGB)
    messages = [bytes([(3 << 3) | (0x40 if multi else 0)]) + timestamp + palette + chunks[0]]
    for i in range(1, len(chunks)):
        last = i == len(chunks) - 1
        # PebbleMessagePacker numbers continuations from 0, one behind their message index
        messages.append(bytes([0x01 | (((i - 1) & 0x07) << 3) | (0x40 if last else 0)]) + timestamp + chunks[i])
    return messages, "%dx%d color, %d -> %d bytes, chunks %s" % (
        width, height, len(packed), len(full), [len(c) for c in chunks])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("platform", nargs="?", default="emery")
    parser.add_argument("--noise", type=float, default=0.0)
    parser.add_argument("--seconds", type=float, default=60.0)
    args = parser.parse_args()

    emulators = json.load(open("/tmp/pb-emulator.json"))[args.platform]
    port = next(iter(emulators.values()))["pypkjs"]["port"]
    pebble = PebbleConnection(WebsocketTransport("ws://localhost:%d/" % port))
    pebble.connect()
    pebble.run_async()
    service = AppMessageService(pebble)
    state = {"messages": [], "frames": 0}

    def send_preview(data):
        service.send_message(APP_UUID, {KEY_PREVIEW_DATA: ByteArray(data)})

    def on_message(txid, app_uuid, data):
        if app_uuid != APP_UUID:
            return
        if KEY_REQUEST_NEXT_FRAME in data:
            request = data[KEY_REQUEST_NEXT_FRAME]
            state["messages"], info = build_frame(request[5], request[6], args.noise)
            state["frames"] += 1
            print("frame %d: %d message(s), %s" % (state["frames"], len(state["messages"]), info), flush=True)
            send_preview(state["messages"][0])
        elif KEY_REQUEST_NEXT_CHUNK in data:
            index = data[KEY_REQUEST_NEXT_CHUNK][5]
            if index < len(state["messages"]):
                print("  chunk request %d -> sent" % index, flush=True)
                send_preview(state["messages"][index])
            else:
                print("  chunk request %d -> out of range, ignored" % index, flush=True)
        elif KEY_CAPTURE in data:
            request = data[KEY_CAPTURE]
            timer_seconds = request[5] | (request[6] << 8) | (request[7] << 16)
            stamp = lambda: bytes([0]) + struct.pack("<I", int(time.time()))
            print("capture requested, timer %ds" % timer_seconds, flush=True)
            service.send_message(APP_UUID, {KEY_CAPTURE_ACK: ByteArray(stamp())})
            threading.Timer(timer_seconds, lambda: service.send_message(
                APP_UUID, {KEY_PICTURE_TAKEN: ByteArray(stamp())})).start()

    service.register_handler("appmessage", on_message)
    print("connected to %s emulator, waiting for the watch ..." % args.platform, flush=True)
    time.sleep(args.seconds)
    print("served %d frames" % state["frames"], flush=True)


if __name__ == "__main__":
    main()
