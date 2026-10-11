#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MThDownloader 浏览器下载桥接 - Chrome/Edge Native Messaging 宿主（Windows）。

协议（Chrome Native Messaging 标准）：
  stdin  : uint32(小端, 消息长度) + UTF-8 JSON 消息
  stdout : uint32(小端, 消息长度) + UTF-8 JSON 响应

收到的消息：
  {"action": "ping"}                                    -> 健康检查
  {"action": "download", "url": "...", "filename": ...} -> 启动 MThDownloader

启动命令由同目录下的 bridge_config.json 决定（executable + args_template），
命令行模板支持占位符 {url} {filename} {referrer} {mime}。

安全约束：
  - 仅接受 bridge_config.json 中 allowed_schemes 白名单内的 URL（默认 http/https/ftp）。
  - 可执行文件路径只来自本地配置文件，不信任浏览器侧传入的路径。
"""

import json
import os
import struct
import subprocess
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.environ.get("MTH_BRIDGE_CONFIG") or os.path.join(HERE, "bridge_config.json")

DEFAULT_CONFIG = {
    "executable": os.path.join(HERE, "MThDownloader.exe"),
    "args_template": ["{url}"],
    "allowed_schemes": ["http", "https", "ftp"],
    "max_url_length": 8192,
}


def log(msg):
    # 仅写入 stderr，stdout 必须只承载协议数据。
    print(msg, file=sys.stderr, flush=True)


def read_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
    except FileNotFoundError:
        log("config not found: %s" % CONFIG_PATH)
        return dict(DEFAULT_CONFIG)
    except Exception as exc:  # noqa: BLE001
        log("config parse error: %s" % exc)
        return dict(DEFAULT_CONFIG)

    merged = dict(DEFAULT_CONFIG)
    merged.update(cfg or {})
    return merged


def read_exact(stream, size):
    buf = bytearray()
    while len(buf) < size:
        chunk = stream.read(size - len(buf))
        if not chunk:
            raise EOFError("unexpected end of stream")
        buf.extend(chunk)
    return bytes(buf)


def read_message():
    header = read_exact(sys.stdin.buffer, 4)
    length = struct.unpack("<I", header)[0]
    if length <= 0 or length > 64 * 1024 * 1024:
        raise ValueError("invalid message length: %d" % length)
    payload = read_exact(sys.stdin.buffer, length)
    return json.loads(payload.decode("utf-8"))


def send_message(obj):
    payload = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(struct.pack("<I", len(payload)))
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def validate_url(url, allowed_schemes):
    if not isinstance(url, str) or not url:
        return False, "empty url"
    try:
        parsed = urllib.parse.urlsplit(url)
    except ValueError as exc:
        return False, "invalid url: %s" % exc
    if parsed.scheme.lower() not in allowed_schemes:
        return False, "scheme not allowed: %s" % parsed.scheme
    return True, ""


def build_command(cfg, msg):
    template = cfg.get("args_template") or DEFAULT_CONFIG["args_template"]
    mapping = {
        "url": msg.get("url", ""),
        "filename": msg.get("filename", ""),
        "referrer": msg.get("referrer", ""),
        "mime": msg.get("mime", ""),
    }
    args = []
    for part in template:
        if not isinstance(part, str):
            part = str(part)
        for key, value in mapping.items():
            part = part.replace("{" + key + "}", str(value))
        args.append(part)
    return [cfg["executable"]] + args


def launch(cfg, cmd):
    exe = cfg["executable"]
    cwd = os.path.dirname(os.path.abspath(exe)) or None
    kwargs = {"cwd": cwd}
    if os.name == "nt":
        # 0x08000000: CREATE_NO_WINDOW，避免控制台程序弹出黑窗。
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
        kwargs["close_fds"] = True
    subprocess.Popen(cmd, **kwargs)


def handle_download(cfg, msg):
    url = (msg.get("url") or "").strip()
    if len(url) > int(cfg.get("max_url_length", 8192)):
        return {"ok": False, "error": "url too long"}

    allowed = [str(s).lower() for s in cfg.get("allowed_schemes", DEFAULT_CONFIG["allowed_schemes"])]
    ok, why = validate_url(url, allowed)
    if not ok:
        return {"ok": False, "error": why}

    exe = cfg["executable"]
    if not exe or not os.path.isfile(exe):
        return {"ok": False, "error": "executable not found: %s" % exe}

    cmd = build_command(cfg, msg)
    try:
        launch(cfg, cmd)
    except Exception as exc:  # noqa: BLE001
        log("launch failed: %s" % exc)
        return {"ok": False, "error": "launch failed: %s" % exc}

    log("launched: %s" % subprocess.list2cmdline(cmd))
    return {"ok": True, "action": "download", "command": cmd}


def main():
    cfg = read_config()
    exe = cfg.get("executable", "")
    exe_ready = bool(exe) and os.path.isfile(exe)
    log("host started; config=%s; executable=%s; ready=%s" % (CONFIG_PATH, exe, exe_ready))

    while True:
        try:
            msg = read_message()
        except EOFError:
            break
        except Exception as exc:  # noqa: BLE001
            log("read error: %s" % exc)
            break

        if not isinstance(msg, dict):
            send_message({"ok": False, "error": "invalid message"})
            continue

        action = msg.get("action")
        if action == "ping":
            send_message({
                "ok": True,
                "action": "ping",
                "executable": exe,
                "executable_ready": exe_ready,
            })
            continue

        if action == "download":
            send_message(handle_download(cfg, msg))
            continue

        send_message({"ok": False, "error": "unsupported action: %s" % action})


if __name__ == "__main__":
    main()
