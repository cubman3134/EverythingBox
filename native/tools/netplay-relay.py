#!/usr/bin/env python3
"""
My Media Vault netplay relay.

Lets two players behind NAT play online without port-forwarding: both connect OUTBOUND to this server, which
pairs them by a short room code and then just forwards raw bytes between the two — the MMV netplay lockstep
protocol runs over that pipe unchanged. Deploy on a public host (e.g. your server) and point clients at it via
Settings > the netplay relay address.

Protocol (line-based handshake, then a raw byte pipe):
  client -> relay :  "HOST <code>\n"   register a room and wait for a peer
                     "JOIN <code>\n"   join an existing room
  relay  -> both  :  "PAIRED\n"        a peer arrived; everything after this is forwarded verbatim
  relay  -> joiner:  "NOHOST\n"        no room with that code (then the relay closes the connection)

No auth, no persistence, one peer pair per room. Rooms that never get a joiner expire after ROOM_TTL seconds.
Run:  python3 netplay-relay.py [--host 0.0.0.0] [--port 55666]
"""
import argparse
import asyncio
import time

ROOM_TTL = 300          # seconds a lone host waits before we give up on it
HANDSHAKE_TIMEOUT = 15  # seconds to send the HOST/JOIN line

rooms = {}  # code -> (host_reader, host_writer, created_ts)


async def pipe(src: asyncio.StreamReader, dst: asyncio.StreamWriter):
    try:
        while True:
            data = await src.read(65536)
            if not data:
                break
            dst.write(data)
            await dst.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            dst.close()
        except Exception:
            pass


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info("peername")
    try:
        line = await asyncio.wait_for(reader.readline(), HANDSHAKE_TIMEOUT)
    except (asyncio.TimeoutError, ConnectionError):
        writer.close()
        return

    parts = line.decode("ascii", "ignore").strip().split()
    if len(parts) != 2 or parts[0] not in ("HOST", "JOIN"):
        writer.close()
        return
    verb, code = parts[0], parts[1][:64]

    # Reap expired rooms opportunistically.
    now = time.time()
    for c in [c for c, (_, _, ts) in rooms.items() if now - ts > ROOM_TTL]:
        try:
            rooms[c][1].close()
        except Exception:
            pass
        rooms.pop(c, None)

    if verb == "HOST":
        if code in rooms:                       # code already hosting -> reject the newcomer
            writer.write(b"BUSY\n"); await writer.drain(); writer.close(); return
        rooms[code] = (reader, writer, now)
        print(f"[+] HOST {code} from {peer}; rooms={len(rooms)}", flush=True)
        # Park here until a joiner pairs us (which starts the pipe) or the connection drops.
        try:
            while code in rooms and rooms[code][1] is writer:
                if reader.at_eof():
                    break
                await asyncio.sleep(1)
        finally:
            if rooms.get(code, (None, None, None))[1] is writer:
                rooms.pop(code, None)
                writer.close()
        return

    # JOIN
    room = rooms.pop(code, None)
    if not room:
        writer.write(b"NOHOST\n"); await writer.drain(); writer.close(); return
    host_reader, host_writer, _ = room
    print(f"[=] PAIR  {code}: {peer} joined; rooms={len(rooms)}", flush=True)
    host_writer.write(b"PAIRED\n"); await host_writer.drain()
    writer.write(b"PAIRED\n"); await writer.drain()
    # Forward bytes both ways until either side closes.
    await asyncio.gather(pipe(host_reader, writer), pipe(reader, host_writer))
    print(f"[-] END   {code}", flush=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=55666)
    args = ap.parse_args()
    server = await asyncio.start_server(handle, args.host, args.port)
    print(f"MMV netplay relay listening on {args.host}:{args.port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
