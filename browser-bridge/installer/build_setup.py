import json
import os
import shutil
import subprocess
import base64 as _b64
import hashlib as _hl
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WS = os.path.dirname(ROOT)               # D:/dsh
TMP = os.path.join(WS, "_tmp", "mth_payload")
DIST_SETUP = os.path.join(ROOT, "dist-setup")
CSC = "C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
EXT_ID = open(os.path.join(ROOT, "config", "extension_id.txt"), encoding="utf-8").read().strip()
TAB = chr(9)
def _tlv(tag, content):
    l = len(content)
    if l < 0x80:
        return bytes([tag, l]) + content
    lb = l.to_bytes((l.bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(lb)]) + lb + content

def _read_tlv(der, pos):
    tag = der[pos]; pos += 1
    l = der[pos]; pos += 1
    if l & 0x80:
        n = l & 0x7f
        l = int.from_bytes(der[pos:pos+n], "big"); pos += n
    return tag, der[pos:pos+l], pos+l

RSA_OID = bytes.fromhex("06092a864886f70d010101")

def _rsa_priv_to_spki(priv_seq_content):
    pos = 0
    _tag, _ver, pos = _read_tlv(priv_seq_content, 0)
    _tag, n, pos = _read_tlv(priv_seq_content, pos)
    _tag, e, pos = _read_tlv(priv_seq_content, pos)
    pub = _tlv(0x30, _tlv(0x02, n) + _tlv(0x02, e))
    alg = _tlv(0x30, RSA_OID + _tlv(0x05, b""))
    return _tlv(0x30, alg + _tlv(0x03, bytes([0]) + pub))

def pem_to_spki_der(pem_path):
    text = open(pem_path, "r", encoding="utf-8").read()
    body = "".join(l.strip() for l in text.splitlines() if not l.startswith("-----"))
    der = _b64.b64decode(body)
    _tag, seq_content, _end = _read_tlv(der, 0)
    _tag, _first, pos = _read_tlv(seq_content, 0)
    _tag, second, pos = _read_tlv(seq_content, pos)
    if _tag == 0x30:
        _tag, octet, pos = _read_tlv(seq_content, pos)
        _tag, priv_seq, _end2 = _read_tlv(octet, 0)
        return _rsa_priv_to_spki(priv_seq)
    return _rsa_priv_to_spki(seq_content)

def derive_id(spki_der):
    d = _hl.sha256(spki_der).digest()[:16]
    return "".join(chr(ord("a") + (b >> 4)) + chr(ord("a") + (b & 15)) for b in d)

def find_chrome():
    for cand in [
        "C:/Program Files/Google/Chrome/Application/chrome.exe",
        "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
        os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google", "Chrome", "Application", "chrome.exe"),
    ]:
        if os.path.isfile(cand):
            return cand
    return None
LF = chr(10)

def sh(cmd, cwd=None):
    print("  $", " ".join(cmd))
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.stdout.strip():
        print(r.stdout)
    if r.stderr.strip():
        print(r.stderr)
    if r.returncode != 0:
        sys.exit("command failed: " + " ".join(cmd))
    return r

def main():
    print("== 1. 构建浏览器扩展 dist ==\n")
    print("== 0. 准备扩展签名密钥与 ID ==")
    pem = os.path.join(ROOT, "installer", "extension_key.pem")
    if not os.path.exists(pem):
        sys.exit("缺少 installer/extension_key.pem（签名私钥，已被 .gitignore 排除）。请用 chrome.exe --pack-extension=<扩展目录> 生成后复制为 installer/extension_key.pem")
    spki = pem_to_spki_der(pem)
    pub_b64 = _b64.b64encode(spki).decode("ascii")
    eid = derive_id(spki)
    os.makedirs(os.path.join(ROOT, "config"), exist_ok=True)
    open(os.path.join(ROOT, "config", "public_key.b64"), "w", encoding="ascii").write(pub_b64)
    open(os.path.join(ROOT, "config", "extension_id.txt"), "w", encoding="ascii").write(eid)
    print("  扩展 ID:", eid)

    sh(["node", "build.js"], cwd=ROOT)

    print("\n== 2. 校验 MThDownloader.exe 支持 URL ==\n")
    import pefile
    print("== 1.5 打包浏览器插件 CRX ==")
    chrome = find_chrome()
    if not chrome:
        sys.exit("未找到 Chrome，无法打包 CRX")
    pem = os.path.join(ROOT, "installer", "extension_key.pem")
    if not os.path.exists(pem):
        sys.exit("缺少 installer/extension_key.pem（签名私钥）")
    crx_dir = os.path.join(WS, "_tmp", "crx_final")
    if os.path.isdir(crx_dir):
        shutil.rmtree(crx_dir)
    os.makedirs(crx_dir)
    for f in os.listdir(os.path.join(ROOT, "dist", "chrome")):
        src = os.path.join(ROOT, "dist", "chrome", f)
        dst = os.path.join(crx_dir, f)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    crx_path = crx_dir + ".crx"
    if os.path.exists(crx_path):
        os.remove(crx_path)
    pack_cmd = [chrome, "--pack-extension=" + crx_dir, "--pack-extension-key=" + pem,
                "--no-message-box", "--user-data-dir=" + os.path.join(WS, "_tmp", "packprofile")]
    sh(pack_cmd)
    if not os.path.exists(crx_path):
        sys.exit("CRX 打包失败")

    exe_src = os.path.join(WS, "MThDownloader.exe")
    pe = pefile.PE(exe_src)
    has = set()
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in entry.imports:
            n = imp.name.decode() if imp.name else ""
            if n in ("GetCommandLineW", "CommandLineToArgvW"):
                has.add(n)
    if not {"GetCommandLineW", "CommandLineToArgvW"}.issubset(has):
        sys.exit("MThDownloader.exe 不支持命令行 URL，无法打包")
    print("  OK: 支持 MThDownloader.exe <url>")

    print("\n== 3. 组装 payload ==\n")
    print("== 2.5 编译卸载器 (csc) ==")
    os.makedirs(os.path.join(WS, "_tmp", "mth_uninstaller"), exist_ok=True)
    uninstaller_exe = os.path.join(WS, "_tmp", "mth_uninstaller", "MThDownloader_Uninstall.exe")
    if os.path.exists(uninstaller_exe):
        os.remove(uninstaller_exe)
    ico = os.path.join(ROOT, "installer", "installer.ico")
    ucmd = [CSC, "/nologo", "/target:winexe", "/platform:anycpu", "/optimize+", "/utf8output"]
    if os.path.exists(ico):
        ucmd.append("/win32icon:" + ico)
    ucmd.append("/out:" + uninstaller_exe)
    ucmd.append(os.path.join(ROOT, "installer", "Uninstall.cs"))
    sh(ucmd, cwd=ROOT)
    if not os.path.exists(uninstaller_exe):
        sys.exit("卸载器编译失败")

    print("== 2.5 确保 C 原生消息宿主已编译 ==")
    host_exe = os.path.join(ROOT, "native-host", "mth_bridge_host.exe")
    if not os.path.exists(host_exe):
        gcc = shutil.which("gcc")
        if not gcc:
            sys.exit("缺少 native-host/mth_bridge_host.exe，且未找到 gcc。请先编译 native-host/mth_bridge_host.c")
        host_src = os.path.join(ROOT, "native-host", "mth_bridge_host.c")
        sh([gcc, "-O2", "-o", host_exe, host_src], cwd=ROOT)

    if os.path.isdir(TMP):
        shutil.rmtree(TMP)
    os.makedirs(TMP)

    def add(src, rel):
        dst = os.path.join(TMP, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print("  +", rel)

    add(os.path.join(WS, "MThDownloader.exe"), "MThDownloader.exe")
    add(os.path.join(WS, "MThDownloader.history.jsonl"), "MThDownloader.history.jsonl")
    add(os.path.join(WS, "MThDownloader.ini"), "MThDownloader.ini")
    add(os.path.join(WS, "engine", "aria2c.exe"), "engine/aria2c.exe")
    add(crx_path, "extension.crx")

    dist = os.path.join(ROOT, "dist", "chrome")
    for f in ["manifest.json", "background.js", "options.html", "options.js", "options.css"]:
        add(os.path.join(dist, f), "extension/" + f)
    for f in ["icon16.png", "icon48.png", "icon128.png"]:
        add(os.path.join(dist, "icons", f), "extension/icons/" + f)

    nh = os.path.join(ROOT, "native-host")
    add(os.path.join(nh, "mth_bridge_host.exe"), "native-host/mth_bridge_host.exe")
    add(os.path.join(nh, "mth_bridge_host.py"), "native-host/mth_bridge_host.py")
    add(os.path.join(nh, "run_host.bat"), "native-host/run_host.bat")
    add(uninstaller_exe, "MThDownloader_Uninstall.exe")

    print("\n== 4. 生成资源清单与图标 ==\n")
    rels = []
    for dirpath, _dirs, files in os.walk(TMP):
        for fn in files:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, TMP).replace(os.sep, "/")
            rels.append(rel)
    rels.sort()

    manifest_path = os.path.join(TMP, "..", "mth_manifest.txt")
    manifest_path = os.path.normpath(manifest_path)
    resource_args = []
    with open(manifest_path, "w", encoding="utf-8") as mf:
        for i, rel in enumerate(rels, 1):
            rid = "mth_payload_%04d" % i
            resource_args.append("/resource:" + os.path.join(TMP, rel.replace("/", os.sep)) + "," + rid)
            mf.write(rid + TAB + rel + LF)
    resource_args.append("/resource:" + manifest_path + ",mth_payload_manifest")
    print("  payload files:", len(rels))

    ico = os.path.join(ROOT, "installer", "installer.ico")
    try:
        from PIL import Image
        img = Image.open(os.path.join(dist, "icons", "icon128.png"))
        img.save(ico, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128)])
        print("  icon written")
    except Exception as e:
        print("  icon skipped:", e)
        ico = None

    print("\n== 5. 编译安装器 (csc) ==\n")
    os.makedirs(DIST_SETUP, exist_ok=True)
    out = os.path.join(DIST_SETUP, "MThDownloader_Setup.exe")
    if os.path.exists(out):
        os.remove(out)

    cmd = [CSC, "/nologo", "/target:winexe", "/platform:anycpu", "/optimize+", "/utf8output"]
    if ico:
        cmd.append("/win32icon:" + ico)
    cmd.append("/out:" + out)
    cmd.append(os.path.join(ROOT, "installer", "Setup.cs"))
    cmd.extend(resource_args)
    sh(cmd, cwd=ROOT)

    if not os.path.exists(out):
        sys.exit("编译产物未生成")

    print("\n== 完成 ==\n")
    print("安装器:", out)
    print("扩展ID:", EXT_ID)

if __name__ == "__main__":
    main()
