// 静态校验：JS 语法、JSON 合法性、公钥与扩展 ID 一致性、构建产物一致性。
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..');
const SRC = path.join(ROOT, 'src');
const DIST = path.join(ROOT, 'dist');
const NATIVE = path.join(ROOT, 'native-host');
const CONFIG = path.join(ROOT, 'config');

let failures = 0;

function check(cond, label) {
  if (cond) {
    console.log('  ok  -', label);
  } else {
    console.error('  FAIL-', label);
    failures += 1;
  }
}

function readJson(p) {
  return JSON.parse(fs.readFileSync(p, 'utf8'));
}

function deriveId(publicKeyB64) {
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

function main() {
  console.log('1. JS 语法检查');
  for (const f of ['src/background.js', 'src/options.js', 'build.js']) {
    const r = spawnSync(process.execPath, ['--check', path.join(ROOT, f)], { encoding: 'utf8' });
    check(r.status === 0, `${f} (${r.status === 0 ? 'ok' : r.stderr.trim()})`);
  }

  console.log('2. JSON 合法性');
  const manifest = readJson(path.join(SRC, 'manifest.json'));
  const bridgeConfig = readJson(path.join(NATIVE, 'bridge_config.json'));
  const hostTemplate = readJson(path.join(NATIVE, 'manifest.template.json'));
  check(manifest.manifest_version === 3, 'manifest_version === 3');
  check(Array.isArray(manifest.permissions) && manifest.permissions.includes('nativeMessaging'), '包含 nativeMessaging 权限');
  check(Array.isArray(manifest.permissions) && manifest.permissions.includes('downloads'), '包含 downloads 权限');
  check(typeof bridgeConfig.executable === 'string', 'bridge_config.executable 为字符串');
  check(hostTemplate.name === 'com.mthdownloader.bridge', '宿主名称为 com.mthdownloader.bridge');

  console.log('3. 公钥与扩展 ID 一致性');
  const publicKey = fs.readFileSync(path.join(CONFIG, 'public_key.b64'), 'utf8').trim();
  const storedId = fs.readFileSync(path.join(CONFIG, 'extension_id.txt'), 'utf8').trim();
  const derivedId = deriveId(publicKey);
  check(derivedId === storedId, `config/extension_id.txt 与公钥派生 ID 一致 (${storedId})`);

  console.log('4. 必需文件');
  for (const f of [
    'src/background.js',
    'src/options.html',
    'src/options.js',
    'src/options.css',
    'src/icons/icon16.png',
    'src/icons/icon48.png',
    'src/icons/icon128.png',
    'native-host/mth_bridge_host.py',
    'native-host/run_host.bat',
    'native-host/install.bat',
    'native-host/uninstall.bat',
  ]) {
    check(fs.existsSync(path.join(ROOT, f)), f);
  }

  console.log('5. 构建产物（若已构建）');
  const chromeManifestPath = path.join(DIST, 'chrome', 'manifest.json');
  if (fs.existsSync(chromeManifestPath)) {
    const m = readJson(chromeManifestPath);
    check(m.key === publicKey, 'dist/chrome manifest.key 与 config 公钥一致');
    check(deriveId(m.key) === storedId, 'dist/chrome manifest.key 派生 ID 一致');
    const hostManifest = readJson(path.join(NATIVE, 'com.mthdownloader.bridge.json'));
    check(
      hostManifest.allowed_origins.includes(`chrome-extension://${storedId}/`),
      '宿主清单 allowed_origins 包含扩展 ID'
    );
  } else {
    console.log('  skip - 尚未构建（可先运行 npm run build）');
  }

  if (failures > 0) {
    console.error(`\n${failures} 项校验失败`);
    process.exit(1);
  }
  console.log('\n全部校验通过');
}

main();
