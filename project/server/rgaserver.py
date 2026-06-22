import asyncio
import datetime
import json
import os
import time

import websockets
from google import genai
from google.genai import errors as genai_errors

HOST = "localhost"
PORT = 8765

_env: dict[str, str] = {}
_env_path = os.path.join(os.path.dirname(__file__), ".env")
if os.path.exists(_env_path):
    with open(_env_path, encoding="utf-8") as _f:
        for _line in _f:
            _line = _line.strip()
            if _line and not _line.startswith("#") and "=" in _line:
                _k, _, _v = _line.partition("=")
                _env[_k.strip()] = _v.strip().strip('"').strip("'")

GEMINI_API_KEY: str = _env.get("GEMINI_API_KEY", "")
AI_ENABLED: bool = _env.get("AI_ENABLED", "true").lower() == "true"

_genai_client: genai.Client | None = (
    genai.Client(api_key=GEMINI_API_KEY) if GEMINI_API_KEY else None
)

rooms: dict[str, set] = {}
room_users: dict[str, dict] = {}
room_host: dict[str, int | None] = {}
room_mode: dict[str, str] = {}
room_usernames: dict[str, dict[int, str]] = {}
room_avatars: dict[str, dict[int, str]] = {}
project_files: dict[str, list] = {}
file_snapshots: dict[str, dict] = {}
file_history: dict[str, dict] = {}
cursor_state: dict[str, dict] = {}
user_file_state: dict[str, dict] = {}
room_start_time: dict[str, float] = {}
room_user_joins: dict[str, dict] = {}
final_file_states: dict[str, dict[str, str]] = {}
room_peer_roles: dict[str, dict[int, str]] = {}


def get_room(path: str) -> str:
    r = path.split("/")[-1]
    return r if r else "default"


def fmt_time(ts: float) -> str:
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%dT%H:%M:%S")


async def send_json(ws, payload: dict):
    try:
        await ws.send(json.dumps(payload, ensure_ascii=False))
    except Exception:
        pass


async def broadcast(room: str, sender, payload: dict):
    for ws in list(rooms.get(room, set())):
        if ws != sender:
            await send_json(ws, payload)


async def broadcast_all(room: str, payload: dict):
    for ws in list(rooms.get(room, set())):
        await send_json(ws, payload)


async def broadcast_user_list(room: str):
    names = room_usernames.get(room, {})
    avatars = room_avatars.get(room, {})
    host_sid = room_host.get(room)
    peer_roles = room_peer_roles.get(room, {})
    users = [
        {
            "siteId": sid,
            "username": names.get(sid, f"user_{sid}"),
            "avatarUrl": avatars.get(sid, ""),
            "role": peer_roles.get(sid, "host" if sid == host_sid else "write"),
        }
        for sid in room_users.get(room, {}).values()
        if sid is not None
    ]
    msg = {"type": "user_list", "users": users}
    for ws in list(rooms.get(room, set())):
        await send_json(ws, msg)


async def send_room_state(ws, room: str):
    host_sid = room_host.get(room)
    peer_roles = room_peer_roles.get(room, {})
    existing_users = [
        {
            "siteId": sid,
            "username": room_usernames.get(room, {}).get(sid, f"user_{sid}"),
            "avatarUrl": room_avatars.get(room, {}).get(sid, ""),
            "role": peer_roles.get(sid, "host" if sid == host_sid else "write"),
        }
        for sid in room_users.get(room, {}).values()
        if sid is not None
    ]
    if existing_users:
        await send_json(ws, {"type": "user_list", "users": existing_users})

    if project_files.get(room):
        await send_json(ws, {
            "type": "project_init",
            "host": host_sid,
            "files": project_files[room],
            "mode": room_mode.get(room, "readwrite"),
            "session_start": room_start_time.get(room, time.time()),
        })

    for snap in file_snapshots.get(room, {}).values():
        await send_json(ws, snap)

    for ops in file_history.get(room, {}).values():
        for op in ops:
            clean = {k: v for k, v in op.items() if not k.startswith("_")}
            await send_json(ws, clean)

    for other_ws, file_cursors in cursor_state.get(room, {}).items():
        if other_ws is not ws:
            for payload in file_cursors.values():
                await send_json(ws, payload)

    for other_ws, fp in user_file_state.get(room, {}).items():
        if other_ws is not ws and fp is not None:
            await send_json(ws, fp)



def compute_op_stats(room: str) -> tuple[dict[int, int], dict[int, int], dict[int, set]]:
    ins: dict[int, int] = {}
    dels: dict[int, int] = {}
    files: dict[int, set] = {}

    for fname, ops in file_history.get(room, {}).items():
        for op in ops:
            if op["type"] == "insert":
                sid = op["node"]["id"]["siteId"]
                ins[sid] = ins.get(sid, 0) + 1
                files.setdefault(sid, set()).add(fname)
            elif op["type"] == "delete":
                actor = op.get("_actor")
                if actor is not None:
                    dels[actor] = dels.get(actor, 0) + 1

    return ins, dels, files


def build_report(room: str) -> dict:
    now = time.time()
    start = room_start_time.get(room, now)
    duration = max(0, int(now - start))

    ins, dels, user_files = compute_op_stats(room)

    file_editors: dict[str, set[int]] = {}
    for fname, ops in file_history.get(room, {}).items():
        for op in ops:
            if op["type"] == "insert":
                sid = op["node"]["id"]["siteId"]
                file_editors.setdefault(fname, set()).add(sid)

    host_sid = room_host.get(room)
    join_times = room_user_joins.get(room, {})

    all_sids: set[int] = set(join_times.keys())
    for sid in list(ins.keys()) + list(dels.keys()):
        all_sids.add(sid)
    for ws, sid in room_users.get(room, {}).items():
        if sid is not None:
            all_sids.add(sid)

    names = room_usernames.get(room, {})
    participants = []
    for sid in sorted(all_sids):
        join_ts = join_times.get(sid, start)
        active_s = max(0, int(now - join_ts))
        files_touched = sorted(user_files.get(sid, set()))
        participants.append({
            "site_id": sid,
            "username": names.get(sid, f"user_{sid}"),
            "is_host": (sid == host_sid),
            "active_sec": active_s,
            "total_inserts": ins.get(sid, 0),
            "total_deletes": dels.get(sid, 0),
            "files_touched": files_touched,
        })

    files = [
        {"name": fname, "editors": sorted(editors)}
        for fname, editors in sorted(file_editors.items())
    ]

    return {
        "room": room,
        "start_time": fmt_time(start),
        "end_time": fmt_time(now),
        "duration_sec": duration,
        "participants": participants,
        "files": files,
    }


def replay_rga(room: str, fname: str) -> list[tuple[str, int]]:
    snap = file_snapshots.get(room, {}).get(fname, {})
    text = snap.get("text", "")

    ROOT = (0, 0)
    nodes: dict = {ROOT: {"enc": ROOT, "parent": None, "val": "", "siteId": -1, "tombstone": True, "next": None}}

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

    for op in file_history.get(room, {}).get(fname, []):
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


def get_line_attribution(room: str, fname: str, names: dict) -> str:
    chars = replay_rga(room, fname)

    lines: list[list[tuple[str, int]]] = []
    current: list[tuple[str, int]] = []
    for ch, sid in chars:
        if ch == '\n':
            lines.append(current)
            current = []
        else:
            current.append((ch, sid))
    if current:
        lines.append(current)

    out = []
    for i, line_chars in enumerate(lines):
        text = "".join(ch for ch, _ in line_chars)
        if not line_chars:
            continue
        counts: dict[int, int] = {}
        for _, sid in line_chars:
            counts[sid] = counts.get(sid, 0) + 1
        dominant = max(counts, key=counts.get)
        author = "existing" if dominant == 0 else names.get(dominant, f"user_{dominant}")
        out.append(f"L{i + 1:03d} | {author} | {text}")
    return "\n".join(out)


def build_ai_prompt(report: dict, room: str) -> str:
    duration_min = report["duration_sec"] // 60
    names = room_usernames.get(room, {})

    all_fnames = sorted(
        set(file_history.get(room, {}).keys()) | set(file_snapshots.get(room, {}).keys())
    )
    file_sections = []
    for fname in all_fnames:
        attribution = get_line_attribution(room, fname, names)
        if attribution.strip():
            file_sections.append(f"[{fname}]:\n{attribution}")

    stats_lines = []
    for p in report["participants"]:
        sid = p["site_id"]
        uname = p.get("username", f"user_{sid}")
        role = "host" if p["is_host"] else "guest"
        files_str = ", ".join(p.get("files_touched", [])) or "—"
        stats_lines.append(
            f"  {uname} ({role}): {p['total_inserts']} вставок, "
            f"{p['total_deletes']} видалень, файли: {files_str}"
        )

    prompt = f"Спільна сесія програмування, тривалість: {duration_min} хв.\n\n"

    if file_sections:
        prompt += (
                "=== АВТОРСТВО ПО РЯДКАХ (git blame стиль) ===\n"
                "(Кожен рядок: номер | автор | текст. "
                "Автор — учасник який написав більшість символів у цьому рядку. "
                "'existing' = рядки що існували до сесії і не були суттєво змінені.)\n\n"
                + "\n\n".join(file_sections)
                + "\n\n"
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
    return prompt


async def generate_ai_insights(report: dict, room: str) -> str | None:
    if not AI_ENABLED or _genai_client is None:
        return None

    prompt = build_ai_prompt(report, room)
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
        except genai_errors.ClientError as e:
            err_str = str(e)
            if "429" in err_str or "RESOURCE_EXHAUSTED" in err_str:
                if "quota" in err_str.lower() or "limit: 0" in err_str:
                    print("[AI] Gemini quota exceeded — check billing/plan at ai.google.dev")
                    return None
                if delay is not None:
                    print(f"[AI] Gemini 429 rate limit — retry {attempt}/{len(delays)} in {delay}s")
                    await asyncio.sleep(delay)
                else:
                    print("[AI] Gemini 429 — all retries exhausted")
                    return None
            else:
                print(f"[AI] Gemini error: {e}")
                return None
        except asyncio.TimeoutError:
            print("[AI] Gemini timeout (90s)")
            return None
        except Exception as e:
            print(f"[AI] Gemini error: {e}")
            return None
    return None


async def handle_session_report(room: str):
    report = build_report(room)
    await broadcast_all(room, {"type": "session_report", "data": report})

    async def send_ai():
        text = await generate_ai_insights(report, room)
        if text:
            await broadcast_all(room, {"type": "session_report_ai", "data": {"text": text}})
        else:
            print(f"[AI] No insights for room={room}")

    asyncio.create_task(send_ai())


async def handle_client(websocket):
    room = get_room(websocket.request.path)

    rooms.setdefault(room, set()).add(websocket)
    room_users.setdefault(room, {})[websocket] = None
    room_host.setdefault(room, None)
    room_mode.setdefault(room, "readwrite")
    room_usernames.setdefault(room, {})
    room_avatars.setdefault(room, {})
    project_files.setdefault(room, [])
    file_snapshots.setdefault(room, {})
    file_history.setdefault(room, {})
    cursor_state.setdefault(room, {})[websocket] = {}
    user_file_state.setdefault(room, {})[websocket] = None
    room_start_time.setdefault(room, time.time())
    room_user_joins.setdefault(room, {})

    print(f"[JOIN] room={room} clients={len(rooms[room])}")
    await send_room_state(websocket, room)

    try:
        async for message in websocket:
            try:
                payload = json.loads(message)
            except Exception:
                continue

            t = payload.get("type")
            file_key = payload.get("file", "")

            if t == "register":
                sid = payload.get("siteId")
                role = payload.get("role", "guest")

                if role != "host" and room_host.get(room) is None:
                    await send_json(websocket, {"type": "error", "message": "Room not found"})
                    await websocket.close(1000, "room not found")
                    break

                room_users[room][websocket] = sid
                if sid is not None:
                    room_user_joins[room][sid] = time.time()
                    uname = payload.get("username", f"user_{sid}")
                    room_usernames[room][sid] = uname
                    room_avatars[room][sid] = payload.get("avatarUrl", "")
                if role == "host":
                    room_host[room] = sid
                    room_peer_roles.setdefault(room, {})[sid] = "host"
                    files = payload.get("files", [])
                    project_files[room] = files
                    room_mode[room] = payload.get("mode", "readwrite")
                    await broadcast(room, websocket, {
                        "type": "project_init", "host": sid,
                        "files": files, "mode": room_mode[room],
                    })
                await broadcast_user_list(room)
                continue

            if t == "kick":
                sender_sid = room_users[room].get(websocket)
                if sender_sid is not None and sender_sid == room_host.get(room):
                    target_sid = payload.get("siteId")
                    for ws, sid in list(room_users[room].items()):
                        if sid == target_sid and ws is not websocket:
                            await send_json(ws, {"type": "kicked"})
                            await ws.close(1000, "kicked")
                            break
                continue

            if t == "cursor":
                cursor_state[room][websocket][file_key] = payload
                await broadcast(room, websocket, payload)
                continue

            if t == "snapshot":
                fk = file_key or ""
                if fk not in file_snapshots[room]:
                    file_snapshots[room][fk] = payload
                    await broadcast(room, websocket, payload)
                continue

            if t in ("insert", "delete", "undelete"):
                sender_sid = room_users[room].get(websocket)
                sender_role = room_peer_roles.get(room, {}).get(sender_sid, "write")
                if sender_role == "read":
                    continue
                stored = dict(payload)
                if t == "delete":
                    stored["_actor"] = sender_sid
                file_history[room].setdefault(file_key, []).append(stored)
                await broadcast(room, websocket, payload)
                continue

            if t == "cursor_leave":
                if file_key and websocket in cursor_state.get(room, {}):
                    cursor_state[room][websocket].pop(file_key, None)
                await broadcast(room, websocket, payload)
                continue

            if t == "file_focus":
                user_file_state[room][websocket] = payload
                await broadcast(room, websocket, payload)
                continue

            if t == "run_output":
                await broadcast(room, websocket, payload)
                continue

            if t == "file_create":
                fp = payload.get("file", "")
                if fp and fp not in project_files[room]:
                    project_files[room].append(fp)
                    file_snapshots[room][fp] = {
                        "type": "snapshot", "file": fp,
                        "text": payload.get("text", ""), "sequence": [],
                    }
                await broadcast(room, websocket, payload)
                continue

            if t == "file_rename":
                old, new = payload.get("old", ""), payload.get("new", "")
                if old in project_files[room]:
                    idx = project_files[room].index(old)
                    project_files[room][idx] = new
                    if old in file_snapshots[room]:
                        snap = dict(file_snapshots[room].pop(old))
                        snap["file"] = new
                        file_snapshots[room][new] = snap
                    if old in file_history[room]:
                        file_history[room][new] = file_history[room].pop(old)
                await broadcast(room, websocket, payload)
                continue

            if t == "file_delete":
                fp = payload.get("file", "")
                if fp in project_files[room]:
                    project_files[room].remove(fp)
                file_snapshots[room].pop(fp, None)
                file_history[room].pop(fp, None)
                await broadcast(room, websocket, payload)
                continue

            if t == "final_state":
                files = payload.get("files", {})
                if isinstance(files, dict):
                    final_file_states.setdefault(room, {}).update(files)
                continue

            if t == "role_change":
                sender_sid = room_users[room].get(websocket)
                if sender_sid is not None and sender_sid == room_host.get(room):
                    target_sid = payload.get("siteId")
                    new_role = payload.get("role", "write")
                    if target_sid is not None:
                        room_peer_roles.setdefault(room, {})[target_sid] = new_role
                    await broadcast_all(room, payload)
                continue

            if t == "session_report_request":
                await handle_session_report(room)
                continue

            if t == "end_session":
                sender_sid = room_users[room].get(websocket)
                is_sender_host = (sender_sid is not None and sender_sid == room_host.get(room))
                if is_sender_host:
                    await handle_session_report(room)
                else:
                    report = build_report(room)
                    await send_json(websocket, {"type": "session_report", "data": report})
                    async def _send_guest_ai(ws=websocket, r=report):
                        text = await generate_ai_insights(r, room)
                        if text:
                            await send_json(ws, {"type": "session_report_ai", "data": {"text": text}})
                    asyncio.create_task(_send_guest_ai())
                continue

    except websockets.ConnectionClosed:
        pass
    finally:
        sid = room_users.get(room, {}).get(websocket)
        is_host = (sid is not None and sid == room_host.get(room))

        rooms[room].discard(websocket)
        room_users[room].pop(websocket, None)
        cursor_state[room].pop(websocket, None)
        user_file_state[room].pop(websocket, None)

        if not rooms[room]:
            for d in (room_users, room_host, room_mode, room_usernames, room_avatars,
                      project_files, file_snapshots, file_history, cursor_state,
                      user_file_state, room_start_time, room_user_joins,
                      final_file_states, room_peer_roles):
                d.pop(room, None)
            del rooms[room]
        elif is_host:
            for ws in list(rooms[room]):
                await send_json(ws, {"type": "session_ended"})
        else:
            await broadcast_user_list(room)
            if sid is not None:
                await broadcast(room, None, {"type": "cursor_leave", "siteId": sid})

        print(f"[LEAVE] room={room} (host={is_host})")


async def main():
    print(f"[SERVER] ws://{HOST}:{PORT}")
    if GEMINI_API_KEY:
        print(f"[AI] Gemini enabled (key: {GEMINI_API_KEY[:8]}...)")
    else:
        print("[AI] Gemini disabled — set GEMINI_API_KEY in server/.env")
    async with websockets.serve(handle_client, HOST, PORT):
        await asyncio.Future()


asyncio.run(main())
