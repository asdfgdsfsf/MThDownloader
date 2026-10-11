// 构建脚本：生成 dist/chrome、dist/edge 两个可加载目录，以及原生消息宿主清单。
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = __dirname;
const SRC = path.join(ROOT, 'src');
const DIST = path.join(ROOT, 'dist');
const NATIVE = path.join(ROOT, 'native-host');
const CONFIG = path.join(ROOT, 'config');

const HOST_NAME = 'com.mthdownloader.bridge';

function readPublicKey() {
  const p = path.join(CONFIG, 'public_key.b64');
  return fs.readFileSync(p, 'utf8').trim();
}

// Chromium 扩展 ID 派生算法：取公钥(DER/SPKI) SHA-256 的前 16 字节，每字节两个十六进制位映射为 a-p。
function deriveExtensionId(publicKeyB64) {
  const der = Buffer.from(publicKeyB64, 'base64');
  const digest = crypto.createHash('sha256').update(der).digest();
  let id = '';
  for (let i = 0; i < 16; i++) {
    const b = digest[i];
    id += String.fromCharCode(0x61 + (b >> 4));
    id += String.fromCharCode(0x61 + (b & 0x0f));
  }
  return id;
}

function copyExtension(target) {
  fs.rmSync(target, { recursive: true, force: true });
  fs.cpSync(SRC, target, { recursive: true });

  const manifestPath = path.join(target, 'manifest.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));

  const publicKey = readPublicKey();
  const id = deriveExtensionId(publicKey);

  if (manifest.key && manifest.key !== publicKey) {
    throw new Error('src/manifest.json 中的 key 与 config/public_key.b64 不一致');
  }
  manifest.key = publicKey;

  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
  return { id, target };
}

function writeHostManifest(extensionId) {
  const templatePath = path.join(NATIVE, 'manifest.template.json');
  const outPath = path.join(NATIVE, HOST_NAME + '.json');
  const manifest = JSON.parse(fs.readFileSync(templatePath, 'utf8'));

  // 独立 C 宿主 (零依赖); run_host.bat + mth_bridge_host.py 仅作为备用方案保留
  manifest.path = path.join(NATIVE, 'mth_bridge_host.exe');
  manifest.allowed_origins = ['chrome-extension://' + extensionId + '/'];

  fs.writeFileSync(outPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
  fs.writeFileSync(path.join(CONFIG, 'extension_id.txt'), extensionId + '\n', 'utf8');
  return outPath;
}

function main() {
  console.log('构建 MThDownloader 下载桥接扩展 ...\n');

  const chrome = copyExtension(path.join(DIST, 'chrome'));
  const edge = copyExtension(path.join(DIST, 'edge'));
  const hostManifest = writeHostManifest(chrome.id);

  if (chrome.id !== edge.id) {
    throw new Error('内部错误：Chrome 与 Edge 的扩展 ID 不一致');
  }

  console.log('扩展 ID :', chrome.id);
  console.log('Chrome  :', chrome.target);
  console.log('Edge    :', edge.target);
  console.log('宿主清单:', hostManifest);
  console.log('');
  console.log('下一步：运行 native-host\install.bat 注册宿主，然后按 README.md 加载扩展。');
}

main();
