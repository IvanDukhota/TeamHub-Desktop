import asyncio
import json
import os
import time
import urllib.error
import urllib.request

import websockets

rooms = {}
clients = {}
room_host: dict[str, int] = {}

_env: dict[str, str] = {}
_env_path = os.path.join(os.path.dirname(__file__), ".env")
if os.path.exists(_env_path):
    with open(_env_path, encoding="utf-8") as _f:
        for _line in _f:
            _line = _line.strip()
            if _line and not _line.startswith("#") and "=" in _line:
                _k, _, _v = _line.partition("=")
                _env[_k.strip()] = _v.strip().strip('"').strip("'")

DJANGO_BASE_URL: str = _env.get("DJANGO_BASE_URL", "http://localhost:8000").rstrip("/")
VOICE_AUTH_ENABLED: bool = _env.get("VOICE_AUTH_ENABLED", "true").lower() == "true"
VERIFY_CACHE_TTL = 60

_verify_cache: dict[tuple[str, str, str], tuple[float, bool]] = {}


def _check_room_access(token: str, team_id: str, room_id: str) -> bool:
    url = f"{DJANGO_BASE_URL}/api/teams/{team_id}/rooms/{room_id}/"
    req = urllib.request.Request(url, headers={"Authorization": f"Token {token}"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status == 200
    except urllib.error.HTTPError:
        return False
    except urllib.error.URLError:
        return False


async def verify_room_access(token: str, team_id: str, room_id: str) -> bool:
    if not VOICE_AUTH_ENABLED:
        return True
    if not token or not team_id or not room_id:
        return False

    cache_key = (token, team_id, room_id)
    now = time.monotonic()
    cached = _verify_cache.get(cache_key)
    if cached is not None and now - cached[0] < VERIFY_CACHE_TTL:
        return cached[1]

    if len(_verify_cache) > 1000:
        _verify_cache.clear()

    loop = asyncio.get_running_loop()
    ok = await loop.run_in_executor(None, _check_room_access, token, team_id, room_id)
    _verify_cache[cache_key] = (now, ok)
    return ok


async def broadcast_room(room):
    if room not in rooms:
        return

    msg = json.dumps({
        "type": "peer_list",
        "peers": rooms[room]["peers"],
        "host_id": room_host.get(room, -1),
    })

    for ws, info in clients.items():
        if info["room"] == room:
            try:
                await ws.send(msg)
            except:
                pass


async def relay_audio(room, sender_ws, data):
    for ws, info in clients.items():
        if info["room"] == room and ws != sender_ws:
            if info["mode"] in ["relay", "hybrid"]:
                try:
                    await ws.send(data)
                except:
                    pass


async def handle_client(websocket):
    try:
        async for message in websocket:

            if isinstance(message, bytes):
                info = clients.get(websocket)
                if info:
                    await relay_audio(info["room"], websocket, message)
                continue

            data = json.loads(message)
            msg_type = data.get("type")

            if msg_type in ["register", "join"]:
                room = data.get("room", "default")
                token = data.get("token", "")
                team_id = data.get("team_id", "")

                if not await verify_room_access(token, team_id, room):
                    try:
                        await websocket.send(json.dumps({
                            "type": "register_denied",
                            "reason": "Not authorized for this voice room.",
                        }))
                        await websocket.close(4001, "unauthorized")
                    except:
                        pass
                    continue

                peer = {
                    "ip": data["ip"],
                    "port": data["port"],
                    "id": data["id"],
                    "mode": data.get("mode", "hybrid"),
                    "username": data.get("username", ""),
                    "avatarUrl": data.get("avatarUrl", ""),
                }

                clients[websocket] = {
                    "room": room,
                    "id": data["id"],
                    "ip": data["ip"],
                    "port": data["port"],
                    "mode": data.get("mode", "hybrid"),
                    "username": data.get("username", ""),
                    "avatarUrl": data.get("avatarUrl", ""),
                }

                if room not in rooms:
                    rooms[room] = {"peers": []}
                    room_host[room] = peer["id"]

                rooms[room]["peers"] = [
                    p for p in rooms[room]["peers"]
                    if p["id"] != peer["id"]
                ]
                rooms[room]["peers"].append(peer)

                await broadcast_room(room)
                continue

            if msg_type == "speaking":
                info = clients.get(websocket)
                if info:
                    for ws, c in list(clients.items()):
                        if c["room"] == info["room"] and ws != websocket:
                            try:
                                await ws.send(message)
                            except Exception:
                                pass
                continue

            if msg_type == "voip_kick":
                sender_id = clients.get(websocket, {}).get("id")
                sender_room = clients.get(websocket, {}).get("room")
                if room_host.get(sender_room) != sender_id:
                    continue
                target_id = data.get("target")
                for ws, info in list(clients.items()):
                    if info["id"] == target_id and info["room"] == sender_room:
                        try:
                            await ws.send(json.dumps({"type": "voip_kicked"}))
                            await ws.close(1000, "kicked")
                        except:
                            pass
                        break
                continue

            if msg_type == "mode":
                if websocket in clients:
                    clients[websocket]["mode"] = data["mode"]

    except:
        pass

    finally:
        if websocket in clients:
            info = clients.pop(websocket)
            room = info["room"]
            pid = info["id"]

            if room in rooms:
                rooms[room]["peers"] = [
                    p for p in rooms[room]["peers"]
                    if p["id"] != pid
                ]

                if room_host.get(room) == pid:
                    remaining = rooms[room].get("peers", [])
                    if remaining:
                        room_host[room] = remaining[0]["id"]
                    else:
                        room_host.pop(room, None)

                await broadcast_room(room)


async def main():
    print("Unified VoIP server ws://0.0.0.0:9000")
    if VOICE_AUTH_ENABLED:
        print(f"[Auth] Verifying rooms against {DJANGO_BASE_URL}")
    else:
        print("[Auth] VOICE_AUTH_ENABLED=false — rooms are NOT verified against Django")
    async with websockets.serve(handle_client, "0.0.0.0", 9000):
        await asyncio.Future()

asyncio.run(main())
