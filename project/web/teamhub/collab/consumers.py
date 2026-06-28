import asyncio
import datetime
import fnmatch
import json
import os
import time

from channels.generic.websocket import AsyncWebsocketConsumer

import redis.asyncio as aioredis
from django.conf import settings

try:
    from google import genai
    from google.genai import errors as genai_errors
    _GENAI_AVAILABLE = True
except ImportError:
    _GENAI_AVAILABLE = False

_GEMINI_API_KEY: str = os.environ.get("GEMINI_API_KEY", "")
_AI_ENABLED: bool = os.environ.get("AI_ENABLED", "true").lower() == "true"
_genai_client = None
if _GENAI_AVAILABLE and _GEMINI_API_KEY:
    _genai_client = genai.Client(api_key=_GEMINI_API_KEY)


class _MemRedis:
    def __init__(self):
        self._data: dict = {}

    def _str(self, v) -> str:
        return v if isinstance(v, str) else str(v)

    async def get(self, key: str):
        v = self._data.get(key)
        return v if isinstance(v, str) else None

    async def set(self, key: str, value):
        self._data[key] = self._str(value)

    def _h(self, key: str) -> dict:
        if not isinstance(self._data.get(key), dict):
            self._data[key] = {}
        return self._data[key]

    async def hget(self, key: str, field):
        return self._h(key).get(self._str(field))

    async def hset(self, key: str, field=None, value=None, mapping=None):
        h = self._h(key)
        if mapping:
            h.update({self._str(k): self._str(v) for k, v in mapping.items()})
        elif field is not None:
            h[self._str(field)] = self._str(value) if value is not None else ""

    async def hsetnx(self, key: str, field, value):
        h = self._h(key)
        f = self._str(field)
        if f not in h:
            h[f] = self._str(value)

    async def hgetall(self, key: str) -> dict:
        return dict(self._h(key))

    async def hdel(self, key: str, *fields):
        h = self._h(key)
        for f in fields:
            h.pop(self._str(f), None)

    async def hexists(self, key: str, field) -> bool:
        return self._str(field) in self._h(key)

    async def hkeys(self, key: str) -> list:
        return list(self._h(key).keys())

    def _s(self, key: str) -> set:
        if not isinstance(self._data.get(key), set):
            self._data[key] = set()
        return self._data[key]

    async def sadd(self, key: str, *members):
        self._s(key).update(self._str(m) for m in members)

    async def srem(self, key: str, *members):
        s = self._s(key)
        for m in members:
            s.discard(self._str(m))

    async def smembers(self, key: str) -> set:
        return set(self._s(key))

    async def scard(self, key: str) -> int:
        return len(self._s(key))

    async def sismember(self, key: str, member) -> bool:
        return self._str(member) in self._s(key)

    def _l(self, key: str) -> list:
        if not isinstance(self._data.get(key), list):
            self._data[key] = []
        return self._data[key]

    async def rpush(self, key: str, *values):
        if values:
            self._l(key).extend(self._str(v) for v in values)

    async def lrange(self, key: str, start: int, end: int) -> list:
        lst = self._l(key)
        return list(lst[start:] if end == -1 else lst[start:end + 1])

    async def delete(self, *keys):
        for k in keys:
            self._data.pop(k, None)

    async def keys(self, pattern: str) -> list:
        return [k for k in self._data if fnmatch.fnmatch(k, pattern)]

    async def exists(self, *keys) -> int:
        return sum(1 for k in keys if k in self._data)

    async def ping(self):
        return True


_redis_pool: aioredis.Redis | None = None
_mem_redis: _MemRedis | None = None
_redis_available: bool | None = None


async def get_redis():
    global _redis_pool, _mem_redis, _redis_available
    if _redis_available is False:
        return _mem_redis
    if _redis_pool is not None:
        return _redis_pool
    try:
        pool = await aioredis.from_url(
            settings.REDIS_URL, encoding="utf-8", decode_responses=True
        )
        await pool.ping()
        _redis_pool = pool
        _redis_available = True
        return _redis_pool
    except Exception:
        _redis_available = False
        _mem_redis = _MemRedis()
        return _mem_redis


def _rk(*parts: str) -> str:
    return "collab:" + ":".join(parts)


async def redis_get_json(r: aioredis.Redis, key: str):
    raw = await r.get(key)
    return json.loads(raw) if raw else None


async def redis_set_json(r: aioredis.Redis, key: str, value):
    await r.set(key, json.dumps(value, ensure_ascii=False))


async def redis_lpush_json(r: aioredis.Redis, key: str, value):
    await r.rpush(key, json.dumps(value, ensure_ascii=False))


async def redis_lrange_json(r: aioredis.Redis, key: str) -> list:
    items = await r.lrange(key, 0, -1)
    return [json.loads(i) for i in items]



_rooms: dict[str, set[str]] = {}
_channel_sid: dict[str, int | None] = {}



class CollabConsumer(AsyncWebsocketConsumer):

    async def connect(self):
        self.room = self.scope["url_route"]["kwargs"]["room"]
        self.group_name = f"collab_{self.room}"

        await self.channel_layer.group_add(self.group_name, self.channel_name)
        _rooms.setdefault(self.room, set()).add(self.channel_name)
        _channel_sid[self.channel_name] = None

        await self.accept()

        r = await get_redis()
        await self._ensure_room_defaults(r)
        await self._send_room_state(r)

    async def disconnect(self, code):
        r = await get_redis()
        sid = _channel_sid.pop(self.channel_name, None)
        is_host = sid is not None and sid == await self._get_host(r)

        _rooms.get(self.room, set()).discard(self.channel_name)
        await self.channel_layer.group_discard(self.group_name, self.channel_name)

        if sid is not None:
            await r.srem(_rk(self.room, "sids"), sid)
            await r.hdel(_rk(self.room, "usernames"), sid)
            await r.hdel(_rk(self.room, "avatars"), sid)
            await r.hdel(_rk(self.room, "peer_roles"), sid)
            await r.hdel(_rk(self.room, "join_times"), sid)

        if not _rooms.get(self.room):
            _rooms.pop(self.room, None)
        elif is_host:
            await self._broadcast_all({"type": "session_ended"})
        else:
            if sid is not None:
                await self._broadcast_except(self.channel_name, {
                    "type": "cursor_leave", "siteId": sid
                })
            await self._broadcast_user_list(r)

    async def receive(self, text_data=None, bytes_data=None):
        if not text_data:
            return
        try:
            payload = json.loads(text_data)
        except Exception:
            return

        r = await get_redis()
        t = payload.get("type")
        file_key = payload.get("file", "")

        if t == "register":
            await self._handle_register(r, payload)
        elif t == "kick":
            await self._handle_kick(r, payload)
        elif t == "cursor":
            await r.hset(_rk(self.room, "cursors", self.channel_name), file_key, json.dumps(payload))
            await self._broadcast_except(self.channel_name, payload)
        elif t == "snapshot":
            await self._handle_snapshot(r, file_key, payload)
        elif t in ("insert", "delete", "undelete"):
            await self._handle_rga_op(r, t, file_key, payload)
        elif t == "cursor_leave":
            await r.hdel(_rk(self.room, "cursors", self.channel_name), file_key)
            await self._broadcast_except(self.channel_name, payload)
        elif t == "file_focus":
            await r.set(_rk(self.room, "file_focus", self.channel_name), json.dumps(payload))
            await self._broadcast_except(self.channel_name, payload)
        elif t == "run_output":
            await self._broadcast_except(self.channel_name, payload)
        elif t == "file_create":
            await self._handle_file_create(r, payload)
        elif t == "file_rename":
            await self._handle_file_rename(r, payload)
        elif t == "file_delete":
            await self._handle_file_delete(r, payload)
        elif t == "final_state":
            files = payload.get("files", {})
            if isinstance(files, dict):
                for fname, text in files.items():
                    await r.hset(_rk(self.room, "final_states"), fname, text)
        elif t == "role_change":
            await self._handle_role_change(r, payload)
        elif t == "session_report_request":
            await self._handle_session_report(r, broadcast_all=True)
        elif t == "end_session":
            await self._handle_end_session(r)


    async def collab_message(self, event):
        await self.send(text_data=event["text"])

    async def collab_binary(self, event):
        pass


    async def _broadcast_except(self, exclude_channel: str, payload: dict):
        text = json.dumps(payload, ensure_ascii=False)
        for ch in list(_rooms.get(self.room, set())):
            if ch != exclude_channel:
                await self.channel_layer.send(ch, {"type": "collab.message", "text": text})

    async def _broadcast_all(self, payload: dict):
        text = json.dumps(payload, ensure_ascii=False)
        for ch in list(_rooms.get(self.room, set())):
            await self.channel_layer.send(ch, {"type": "collab.message", "text": text})

    async def _send_to(self, payload: dict):
        await self.send(text_data=json.dumps(payload, ensure_ascii=False))


    async def _ensure_room_defaults(self, r: aioredis.Redis):
        await r.hsetnx(_rk(self.room, "meta"), "mode", "readwrite")
        await r.hsetnx(_rk(self.room, "meta"), "start_time", str(time.time()))

    async def _get_host(self, r: aioredis.Redis) -> int | None:
        raw = await r.hget(_rk(self.room, "meta"), "host_sid")
        return int(raw) if raw is not None else None

    async def _get_peer_roles(self, r: aioredis.Redis) -> dict[int, str]:
        raw = await r.hgetall(_rk(self.room, "peer_roles"))
        return {int(k): v for k, v in raw.items()}

    async def _get_usernames(self, r: aioredis.Redis) -> dict[int, str]:
        raw = await r.hgetall(_rk(self.room, "usernames"))
        return {int(k): v for k, v in raw.items()}

    async def _get_avatars(self, r: aioredis.Redis) -> dict[int, str]:
        raw = await r.hgetall(_rk(self.room, "avatars"))
        return {int(k): v for k, v in raw.items()}

    async def _get_project_files(self, r: aioredis.Redis) -> list[str]:
        return await r.lrange(_rk(self.room, "project_files"), 0, -1)

    async def _get_sid_list(self, r: aioredis.Redis) -> list[int]:
        raw = await r.smembers(_rk(self.room, "sids"))
        return [int(x) for x in raw]


    async def _send_room_state(self, r: aioredis.Redis):
        host_sid = await self._get_host(r)
        peer_roles = await self._get_peer_roles(r)
        usernames = await self._get_usernames(r)
        avatars = await self._get_avatars(r)
        sids = await self._get_sid_list(r)

        if sids:
            users = [
                {
                    "siteId": sid,
                    "username": usernames.get(sid, f"user_{sid}"),
                    "avatarUrl": avatars.get(sid, ""),
                    "role": peer_roles.get(sid, "host" if sid == host_sid else "write"),
                }
                for sid in sids
            ]
            await self._send_to({"type": "user_list", "users": users})

        project_files = await self._get_project_files(r)
        if project_files:
            meta = await r.hgetall(_rk(self.room, "meta"))
            await self._send_to({
                "type": "project_init",
                "host": host_sid,
                "files": project_files,
                "mode": meta.get("mode", "readwrite"),
                "session_start": float(meta.get("start_time", time.time())),
            })


        snap_keys = await r.hkeys(_rk(self.room, "snapshots"))
        for fname in snap_keys:
            raw = await r.hget(_rk(self.room, "snapshots"), fname)
            if raw:
                await self._send_to(json.loads(raw))


        hist_keys = await r.smembers(_rk(self.room, "history_files"))
        for fname in hist_keys:
            ops = await redis_lrange_json(r, _rk(self.room, "history", fname))
            for op in ops:
                clean = {k: v for k, v in op.items() if not k.startswith("_")}
                await self._send_to(clean)

        cursor_channels = await r.keys(_rk(self.room, "cursors", "*"))
        for ch_key in cursor_channels:
            ch = ch_key.split(":")[-1]
            if ch == self.channel_name:
                continue
            cursors = await r.hgetall(ch_key)
            for fc in cursors.values():
                await self._send_to(json.loads(fc))

        ff_keys = await r.keys(_rk(self.room, "file_focus", "*"))
        for ff_key in ff_keys:
            ch = ff_key.split(":")[-1]
            if ch == self.channel_name:
                continue
            raw = await r.get(ff_key)
            if raw:
                await self._send_to(json.loads(raw))


    async def _broadcast_user_list(self, r: aioredis.Redis):
        host_sid = await self._get_host(r)
        peer_roles = await self._get_peer_roles(r)
        usernames = await self._get_usernames(r)
        avatars = await self._get_avatars(r)
        sids = await self._get_sid_list(r)

        users = [
            {
                "siteId": sid,
                "username": usernames.get(sid, f"user_{sid}"),
                "avatarUrl": avatars.get(sid, ""),
                "role": peer_roles.get(sid, "host" if sid == host_sid else "write"),
            }
            for sid in sids
        ]
        await self._broadcast_all({"type": "user_list", "users": users})


    async def _handle_register(self, r: aioredis.Redis, payload: dict):
        sid = payload.get("siteId")
        role = payload.get("role", "guest")
        host_sid = await self._get_host(r)

        if role != "host" and host_sid is None:
            await self._send_to({"type": "error", "message": "Room not found"})
            await self.close(1000)
            return

        _channel_sid[self.channel_name] = sid
        if sid is not None:
            await r.sadd(_rk(self.room, "sids"), sid)
            await r.hset(_rk(self.room, "usernames"), sid, payload.get("username", f"user_{sid}"))
            await r.hset(_rk(self.room, "avatars"), sid, payload.get("avatarUrl", ""))
            await r.hset(_rk(self.room, "join_times"), sid, str(time.time()))

        if role == "host":
            await r.hset(_rk(self.room, "meta"), "host_sid", sid)
            await r.hset(_rk(self.room, "peer_roles"), sid, "host")
            files = payload.get("files", [])
            files_key = _rk(self.room, "project_files")
            await r.delete(files_key)
            if files:
                await r.rpush(files_key, *files)
            await r.hset(_rk(self.room, "meta"), "mode", payload.get("mode", "readwrite"))
            meta = await r.hgetall(_rk(self.room, "meta"))
            await self._broadcast_except(self.channel_name, {
                "type": "project_init",
                "host": sid,
                "files": files,
                "mode": meta.get("mode", "readwrite"),
            })

        await self._broadcast_user_list(r)

    async def _handle_kick(self, r: aioredis.Redis, payload: dict):
        sender_sid = _channel_sid.get(self.channel_name)
        host_sid = await self._get_host(r)
        if sender_sid is None or sender_sid != host_sid:
            return
        target_sid = payload.get("siteId")
        for ch, sid in list(_channel_sid.items()):
            if sid == target_sid and ch != self.channel_name and ch in _rooms.get(self.room, set()):
                await self.channel_layer.send(ch, {
                    "type": "collab.message",
                    "text": json.dumps({"type": "kicked"}),
                })
                break

    async def _handle_snapshot(self, r: aioredis.Redis, file_key: str, payload: dict):
        exists = await r.hexists(_rk(self.room, "snapshots"), file_key)
        if not exists:
            await r.hset(_rk(self.room, "snapshots"), file_key, json.dumps(payload))
            await self._broadcast_except(self.channel_name, payload)

    async def _handle_rga_op(self, r: aioredis.Redis, op_type: str, file_key: str, payload: dict):
        sender_sid = _channel_sid.get(self.channel_name)
        peer_roles = await self._get_peer_roles(r)
        if peer_roles.get(sender_sid, "write") == "read":
            return
        stored = dict(payload)
        if op_type == "delete":
            stored["_actor"] = sender_sid
        await r.sadd(_rk(self.room, "history_files"), file_key)
        await redis_lpush_json(r, _rk(self.room, "history", file_key), stored)
        await self._broadcast_except(self.channel_name, payload)

    async def _handle_file_create(self, r: aioredis.Redis, payload: dict):
        fp = payload.get("file", "")
        if fp:
            files = await self._get_project_files(r)
            if fp not in files:
                await r.rpush(_rk(self.room, "project_files"), fp)
                await r.hset(_rk(self.room, "snapshots"), fp, json.dumps({
                    "type": "snapshot", "file": fp,
                    "text": payload.get("text", ""), "sequence": [],
                }))
        await self._broadcast_except(self.channel_name, payload)

    async def _handle_file_rename(self, r: aioredis.Redis, payload: dict):
        old, new = payload.get("old", ""), payload.get("new", "")
        files = await self._get_project_files(r)
        if old in files:
            idx = files.index(old)
            files[idx] = new
            files_key = _rk(self.room, "project_files")
            await r.delete(files_key)
            if files:
                await r.rpush(files_key, *files)
            old_snap = await r.hget(_rk(self.room, "snapshots"), old)
            if old_snap:
                snap = json.loads(old_snap)
                snap["file"] = new
                await r.hset(_rk(self.room, "snapshots"), new, json.dumps(snap))
                await r.hdel(_rk(self.room, "snapshots"), old)
            in_hist = await r.sismember(_rk(self.room, "history_files"), old)
            if in_hist:
                old_ops = await redis_lrange_json(r, _rk(self.room, "history", old))
                await r.delete(_rk(self.room, "history", old))
                await r.srem(_rk(self.room, "history_files"), old)
                if old_ops:
                    await r.sadd(_rk(self.room, "history_files"), new)
                    for op in old_ops:
                        await redis_lpush_json(r, _rk(self.room, "history", new), op)
        await self._broadcast_except(self.channel_name, payload)

    async def _handle_file_delete(self, r: aioredis.Redis, payload: dict):
        fp = payload.get("file", "")
        files = await self._get_project_files(r)
        if fp in files:
            files.remove(fp)
            files_key = _rk(self.room, "project_files")
            await r.delete(files_key)
            if files:
                await r.rpush(files_key, *files)
        await r.hdel(_rk(self.room, "snapshots"), fp)
        await r.delete(_rk(self.room, "history", fp))
        await r.srem(_rk(self.room, "history_files"), fp)
        await self._broadcast_except(self.channel_name, payload)

    async def _handle_role_change(self, r: aioredis.Redis, payload: dict):
        sender_sid = _channel_sid.get(self.channel_name)
        host_sid = await self._get_host(r)
        if sender_sid is None or sender_sid != host_sid:
            return
        target_sid = payload.get("siteId")
        new_role = payload.get("role", "write")
        if target_sid is not None:
            await r.hset(_rk(self.room, "peer_roles"), target_sid, new_role)
        await self._broadcast_all(payload)


    async def _handle_end_session(self, r: aioredis.Redis):
        sender_sid = _channel_sid.get(self.channel_name)
        host_sid = await self._get_host(r)
        is_host = sender_sid is not None and sender_sid == host_sid
        if is_host:
            await self._handle_session_report(r, broadcast_all=True)
        else:
            report = await self._build_report(r)
            await self._send_to({"type": "session_report", "data": report})
            asyncio.create_task(self._send_ai_to_self(r, report))

    async def _handle_session_report(self, r: aioredis.Redis, broadcast_all: bool):
        report = await self._build_report(r)
        payload = {"type": "session_report", "data": report}
        if broadcast_all:
            await self._broadcast_all(payload)
        else:
            await self._send_to(payload)
        asyncio.create_task(self._send_ai_insights(r, report, broadcast_all))

    async def _send_ai_to_self(self, r: aioredis.Redis, report: dict):
        text = await self._generate_ai_insights(report)
        if text:
            await self._send_to({"type": "session_report_ai", "data": {"text": text}})

    async def _send_ai_insights(self, r: aioredis.Redis, report: dict, broadcast_all: bool):
        text = await self._generate_ai_insights(report)
        if not text:
            return
        payload = {"type": "session_report_ai", "data": {"text": text}}
        if broadcast_all:
            await self._broadcast_all(payload)
        else:
            await self._send_to(payload)


    async def _build_report(self, r: aioredis.Redis) -> dict:
        now = time.time()
        meta = await r.hgetall(_rk(self.room, "meta"))
        start = float(meta.get("start_time", now))
        duration = max(0, int(now - start))
        host_sid = int(meta["host_sid"]) if "host_sid" in meta else None

        hist_files = await r.smembers(_rk(self.room, "history_files"))
        ins: dict[int, int] = {}
        dels: dict[int, int] = {}
        user_files: dict[int, set] = {}
        file_editors: dict[str, set[int]] = {}

        for fname in hist_files:
            ops = await redis_lrange_json(r, _rk(self.room, "history", fname))
            for op in ops:
                if op["type"] == "insert":
                    sid = op["node"]["id"]["siteId"]
                    ins[sid] = ins.get(sid, 0) + 1
                    user_files.setdefault(sid, set()).add(fname)
                    file_editors.setdefault(fname, set()).add(sid)
                elif op["type"] == "delete":
                    actor = op.get("_actor")
                    if actor is not None:
                        dels[actor] = dels.get(actor, 0) + 1

        join_times_raw = await r.hgetall(_rk(self.room, "join_times"))
        join_times = {int(k): float(v) for k, v in join_times_raw.items()}
        usernames = await self._get_usernames(r)

        all_sids: set[int] = set(join_times.keys()) | set(ins.keys()) | set(dels.keys())
        sids = await self._get_sid_list(r)
        all_sids.update(sids)

        participants = []
        for sid in sorted(all_sids):
            join_ts = join_times.get(sid, start)
            participants.append({
                "site_id": sid,
                "username": usernames.get(sid, f"user_{sid}"),
                "is_host": sid == host_sid,
                "active_sec": max(0, int(now - join_ts)),
                "total_inserts": ins.get(sid, 0),
                "total_deletes": dels.get(sid, 0),
                "files_touched": sorted(user_files.get(sid, set())),
            })

        files = [
            {"name": fname, "editors": sorted(editors)}
            for fname, editors in sorted(file_editors.items())
        ]

        def fmt(ts: float) -> str:
            return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%dT%H:%M:%S")

        return {
            "room": self.room,
            "start_time": fmt(start),
            "end_time": fmt(now),
            "duration_sec": duration,
            "participants": participants,
            "files": files,
        }

    async def _generate_ai_insights(self, report: dict) -> str | None:
        if not _AI_ENABLED or _genai_client is None:
            return None

        r = await get_redis()
        usernames = await self._get_usernames(r)

        hist_files = await r.smembers(_rk(self.room, "history_files"))
        snap_keys = await r.hkeys(_rk(self.room, "snapshots"))
        all_fnames = sorted(set(hist_files) | set(snap_keys))

        file_sections = []
        for fname in all_fnames:
            chars = await self._replay_rga(r, fname)
            lines: list[list[tuple[str, int]]] = []
            current: list[tuple[str, int]] = []
            for ch, sid in chars:
                if ch == "\n":
                    lines.append(current)
                    current = []
                else:
                    current.append((ch, sid))
            if current:
                lines.append(current)

            out = []
            for i, line_chars in enumerate(lines):
                if not line_chars:
                    continue
                text = "".join(c for c, _ in line_chars)
                counts: dict[int, int] = {}
                for _, sid in line_chars:
                    counts[sid] = counts.get(sid, 0) + 1
                dominant = max(counts, key=counts.get)
                author = "existing" if dominant == 0 else usernames.get(dominant, f"user_{dominant}")
                out.append(f"L{i + 1:03d} | {author} | {text}")
            if out:
                file_sections.append(f"[{fname}]:\n" + "\n".join(out))

        duration_min = report["duration_sec"] // 60
        prompt = f"Спільна сесія програмування, тривалість: {duration_min} хв.\n\n"

        if file_sections:
            prompt += (
                "=== АВТОРСТВО ПО РЯДКАХ (git blame стиль) ===\n"
                "(Кожен рядок: номер | автор | текст. "
                "Автор — учасник який написав більшість символів у цьому рядку. "
                "'existing' = рядки що існували до сесії і не були суттєво змінені.)\n\n"
                + "\n\n".join(file_sections) + "\n\n"
            )

        stats_lines = []
        for p in report["participants"]:
            sid = p["site_id"]
            role = "host" if p["is_host"] else "guest"
            files_str = ", ".join(p.get("files_touched", [])) or "—"
            stats_lines.append(
                f"  {p.get('username', f'user_{sid}')} ({role}): "
                f"{p['total_inserts']} вставок, {p['total_deletes']} видалень, файли: {files_str}"
            )
        if stats_lines:
            prompt += "=== СТАТИСТИКА УЧАСНИКІВ ===\n" + "\n".join(stats_lines) + "\n\n"

        prompt += (
            "Проаналізуй сесію і напиши звіт українською мовою у такому форматі:\n\n"
            "Спочатку — один абзац із загальним підсумком: що загалом було зроблено за сесію.\n\n"
            "Потім — окремий абзац для КОЖНОГО учасника, починаючи з їхнього імені. "
            "Детально поясни що конкретно зробив цей учасник: "
            "які функції написав, які класи чи методи додав, що змінив. "
            "Якщо видно назви функцій чи змінних — згадай їх. "
            "Авторство по рядках — це точна інформація, орієнтуйся насамперед на неї. "
            "Якщо учасник мало що вніс — так і напиши.\n\n"
            "Відповідай тільки звичайним текстом без JSON, без markdown, без зірочок."
        )

        delays = [5, 15, 45]
        for attempt, delay in enumerate(delays + [None], start=1):
            try:
                response = await asyncio.wait_for(
                    _genai_client.aio.models.generate_content(
                        model="gemini-flash-latest",
                        contents=prompt,
                        config=genai.types.GenerateContentConfig(temperature=0.4),
                    ),
                    timeout=90.0,
                )
                text = response.text
                return text.strip() if text else None
            except Exception as e:
                err_str = str(e)
                if "429" in err_str or "RESOURCE_EXHAUSTED" in err_str:
                    if delay is not None:
                        await asyncio.sleep(delay)
                    else:
                        return None
                else:
                    return None
        return None

    async def _replay_rga(self, r: aioredis.Redis, fname: str) -> list[tuple[str, int]]:
        raw_snap = await r.hget(_rk(self.room, "snapshots"), fname)
        snap = json.loads(raw_snap) if raw_snap else {}
        text = snap.get("text", "")

        ROOT = (0, 0)
        nodes: dict = {
            ROOT: {"enc": ROOT, "parent": None, "val": "", "siteId": -1, "tombstone": True, "next": None}
        }

        prev = ROOT
        for i, ch in enumerate(text):
            enc = (i + 1, 0)
            nodes[enc] = {"enc": enc, "parent": prev, "val": ch, "siteId": 0, "tombstone": False, "next": None}
            nodes[prev]["next"] = enc
            prev = enc

        def rga_insert(enc, parent_enc, val, site_id):
            if enc in nodes or parent_enc not in nodes:
                return
            nodes[enc] = {"enc": enc, "parent": parent_enc, "val": val, "siteId": site_id, "tombstone": False, "next": None}
            skipped: set = set()
            prev_local = parent_enc
            curr = nodes[parent_enc]["next"]
            while curr is not None:
                n = nodes[curr]
                if n["parent"] == parent_enc:
                    if curr > enc:
                        skipped.add(curr)
                        prev_local = curr
                        curr = n["next"]
                    else:
                        break
                elif n["parent"] in skipped:
                    skipped.add(curr)
                    prev_local = curr
                    curr = n["next"]
                else:
                    break
            nodes[prev_local]["next"] = enc
            nodes[enc]["next"] = curr

        ops = await redis_lrange_json(r, _rk(self.room, "history", fname))
        for op in ops:
            t = op.get("type")
            if t == "insert":
                n = op.get("node", {})
                nid = n.get("id", {})
                par = n.get("parent", {})
                enc = (nid.get("timestamp", 0), nid.get("siteId", 0))
                parent_enc = (par.get("timestamp", 0), par.get("siteId", 0))
                rga_insert(enc, parent_enc, n.get("val", ""), nid.get("siteId", 0))
            elif t == "delete":
                did = op.get("id", {})
                enc = (did.get("timestamp", 0), did.get("siteId", 0))
                if enc in nodes:
                    nodes[enc]["tombstone"] = True
            elif t == "undelete":
                uid = op.get("id", {})
                enc = (uid.get("timestamp", 0), uid.get("siteId", 0))
                if enc in nodes:
                    nodes[enc]["tombstone"] = False

        result = []
        curr = nodes[ROOT]["next"]
        while curr is not None:
            n = nodes[curr]
            if not n["tombstone"]:
                result.append((n["val"], n["siteId"]))
            curr = n["next"]
        return result
