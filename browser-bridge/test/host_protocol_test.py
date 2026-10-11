#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地桥协议单元测试。

不真实启动 MThDownloader.exe：把 bridge_config.json 中的 executable 替换为
当前 Python 解释器，args_template 替换为“打印 url 到 stderr”的命令，
从而验证：协议帧、ping、下载转发、URL 协议白名单、命令行模板替换。
"""

import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "native-host", "mth_bridge_host.py")

ECHO_CODE = "import sys; sys.stderr.write('url=' + sys.argv[1])"


def make_config():
    fd, path = tempfile.mkstemp(prefix="mth_bridge_", suffix=".json")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(
            {
                "executable": sys.executable,
                "args_template": ["-c", ECHO_CODE, "{url}"],
                "allowed_schemes": ["http", "https", "ftp"],
                "max_url_length": 8192,
            },
            fh,
        )
    return path


def send(proc, obj):
    data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(data)))
    proc.stdin.write(data)
    proc.stdin.flush()


def recv(proc):
    header = proc.stdout.read(4)
    if len(header) < 4:
        raise RuntimeError("host closed stdout unexpectedly")
    length = struct.unpack("<I", header)[0]
    payload = proc.stdout.read(length)
    return json.loads(payload.decode("utf-8"))


def main():
    # Windows 控制台默认 GBK，强制 UTF-8 输出，避免中文乱码。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except AttributeError:
            pass
    config = make_config()
    env = dict(os.environ)
    env["MTH_BRIDGE_CONFIG"] = config

    proc = subprocess.Popen(
        [sys.executable, HOST],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )

    failures = 0

    def check(cond, label):
        nonlocal failures
        if cond:
            print("  ok  -", label)
        else:
            print("  FAIL-", label)
            failures += 1

    try:
        # 1. ping
        send(proc, {"action": "ping"})
        resp = recv(proc)
        check(resp.get("ok") is True and resp.get("action") == "ping", "ping 正常响应")

        # 2. 合法下载：验证 URL 转发与命令行模板替换
        url = "https://example.com/a/file.zip?token=x&v=2"
        send(proc, {"action": "download", "url": url, "filename": "file.zip"})
        resp = recv(proc)
        cmd = resp.get("command") or []
        check(resp.get("ok") is True, "合法 https 下载转发成功")
        check(cmd[:2] == [sys.executable, "-c"], "命令头为 [python, -c]")
        check(len(cmd) == 4 and cmd[3] == url, "模板 {url} 被正确替换")

        # 3. 白名单之外的协议被拒绝
        for bad in ("file:///C:/Windows/notepad.exe", "javascript:alert(1)"):
            send(proc, {"action": "download", "url": bad})
            resp = recv(proc)
            check(resp.get("ok") is False, "拒绝协议: %s" % bad.split(":")[0])

        # 4. 空 URL 被拒绝
        send(proc, {"action": "download", "url": ""})
        resp = recv(proc)
        check(resp.get("ok") is False, "拒绝空 URL")

        # 5. 未知 action
        send(proc, {"action": "nope"})
        resp = recv(proc)
        check(resp.get("ok") is False, "拒绝未知 action")
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=10)
        proc.stderr.read()
        os.unlink(config)

    if failures:
        print("\n%d 项测试失败" % failures)
        sys.exit(1)
    print("\n本地桥协议测试全部通过")


if __name__ == "__main__":
    main()
