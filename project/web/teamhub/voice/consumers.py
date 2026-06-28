import json
import time

from channels.generic.websocket import AsyncWebsocketConsumer


# room → {"peers": [...]}
_rooms: dict[str, dict] = {}
# channel_name → {"room", "id", "ip", "port", "mode", "username", "avatarUrl"}
_clients: dict[str, dict] = {}
# room → host peer id
_room_host: dict[str, int] = {}

_VERIFY_CACHE_TTL = 60
_verify_cache: dict[tuple[str, str, str], tuple[float, bool]] = {}


class VoiceConsumer(AsyncWebsocketConsumer):

    async def connect(self):
        await self.accept()

    async def disconnect(self, code):
        if self.channel_name not in _clients:
            return

        info = _clients.pop(self.channel_name)
        room = info["room"]
        pid = info["id"]

        if room in _rooms:
            _rooms[room]["peers"] = [
                p for p in _rooms[room]["peers"] if p["id"] != pid
            ]
            if _room_host.get(room) == pid:
                remaining = _rooms[room].get("peers", [])
                if remaining:
                    _room_host[room] = remaining[0]["id"]
                else:
                    _room_host.pop(room, None)
            await self._broadcast_room(room)

    async def receive(self, text_data=None, bytes_data=None):
        if bytes_data is not None:
            info = _clients.get(self.channel_name)
            if info:
                await self._relay_audio(info["room"], self.channel_name, bytes_data)
            return

        if not text_data:
            return

        try:
            data = json.loads(text_data)
        except Exception:
            return

        msg_type = data.get("type")

        if msg_type in ("register", "join"):
            await self._handle_register(data)

        elif msg_type == "speaking":
            info = _clients.get(self.channel_name)
            if info:
                await self._broadcast_room_except(info["room"], self.channel_name, text_data)

        elif msg_type == "voip_kick":
            await self._handle_kick(data)

        elif msg_type == "mode":
            if self.channel_name in _clients:
                _clients[self.channel_name]["mode"] = data.get("mode", "hybrid")


    async def voice_text(self, event):
        await self.send(text_data=event["text"])

    async def voice_binary(self, event):
        await self.send(bytes_data=bytes(event["data"]))


    async def _handle_register(self, data: dict):
        room = data.get("room", "default")
        token = data.get("token", "")
        team_id = data.get("team_id", "")

        if not await _verify_room_access(token, team_id, room):
            await self.send(text_data=json.dumps({
                "type": "register_denied",
                "reason": "Not authorized for this voice room.",
            }))
            await self.close(4001)
            return

        peer = {
            "ip": data.get("ip", ""),
            "port": data.get("port", 0),
            "id": data["id"],
            "mode": data.get("mode", "hybrid"),
            "username": data.get("username", ""),
            "avatarUrl": data.get("avatarUrl", ""),
        }

        _clients[self.channel_name] = {
            "room": room,
            "id": data["id"],
            "ip": data.get("ip", ""),
            "port": data.get("port", 0),
            "mode": data.get("mode", "hybrid"),
            "username": data.get("username", ""),
            "avatarUrl": data.get("avatarUrl", ""),
        }

        if room not in _rooms:
            _rooms[room] = {"peers": []}
            _room_host[room] = peer["id"]

        _rooms[room]["peers"] = [p for p in _rooms[room]["peers"] if p["id"] != peer["id"]]
        _rooms[room]["peers"].append(peer)

        await self._broadcast_room(room)

    async def _handle_kick(self, data: dict):
        sender_id = _clients.get(self.channel_name, {}).get("id")
        sender_room = _clients.get(self.channel_name, {}).get("room")
        if _room_host.get(sender_room) != sender_id:
            return
        target_id = data.get("target")
        for ch, info in list(_clients.items()):
            if info["id"] == target_id and info["room"] == sender_room:
                await self.channel_layer.send(ch, {
                    "type": "voice.text",
                    "text": json.dumps({"type": "voip_kicked"}),
                })
                break


    async def _broadcast_room(self, room: str):
        msg = json.dumps({
            "type": "peer_list",
            "peers": _rooms.get(room, {}).get("peers", []),
            "host_id": _room_host.get(room, -1),
        })
        for ch, info in list(_clients.items()):
            if info["room"] == room:
                await self.channel_layer.send(ch, {"type": "voice.text", "text": msg})

    async def _broadcast_room_except(self, room: str, exclude_ch: str, text: str):
        for ch, info in list(_clients.items()):
            if info["room"] == room and ch != exclude_ch:
                await self.channel_layer.send(ch, {"type": "voice.text", "text": text})

    async def _relay_audio(self, room: str, sender_ch: str, data: bytes):
        for ch, info in list(_clients.items()):
            if info["room"] == room and ch != sender_ch:
                if info.get("mode") in ("relay", "hybrid"):
                    await self.channel_layer.send(ch, {
                        "type": "voice.binary",
                        "data": list(data),
                    })



import asyncio
import os
import urllib.error
import urllib.request

_DJANGO_BASE_URL = os.environ.get("DJANGO_BASE_URL", "http://localhost:8000").rstrip("/")
_VOICE_AUTH_ENABLED = os.environ.get("VOICE_AUTH_ENABLED", "false").lower() == "true"


def _check_room_access(token: str, team_id: str, room_id: str) -> bool:
    url = f"{_DJANGO_BASE_URL}/api/teams/{team_id}/rooms/{room_id}/"
    req = urllib.request.Request(url, headers={"Authorization": f"Token {token}"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status == 200
    except (urllib.error.HTTPError, urllib.error.URLError):
        return False


async def _verify_room_access(token: str, team_id: str, room_id: str) -> bool:
    if not _VOICE_AUTH_ENABLED:
        return True
    if not token or not team_id or not room_id:
        return False

    cache_key = (token, team_id, room_id)
    now = time.monotonic()
    cached = _verify_cache.get(cache_key)
    if cached is not None and now - cached[0] < _VERIFY_CACHE_TTL:
        return cached[1]

    if len(_verify_cache) > 1000:
        _verify_cache.clear()

    loop = asyncio.get_running_loop()
    ok = await loop.run_in_executor(None, _check_room_access, token, team_id, room_id)
    _verify_cache[cache_key] = (now, ok)
    return ok
