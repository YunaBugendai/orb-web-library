'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const L = require('../lib');

test('toWslPath converts drive and UNC paths', () => {
    assert.strictEqual(L.toWslPath('C:\\Users\\me\\a b\\x.orb'), '/mnt/c/Users/me/a b/x.orb');
    assert.strictEqual(L.toWslPath('d:/work/x.orb'), '/mnt/d/work/x.orb');
    assert.strictEqual(L.toWslPath('C:\\'), '/mnt/c');
    assert.strictEqual(L.toWslPath('\\\\wsl$\\Ubuntu\\home\\me\\x.orb'), '/home/me/x.orb');
    assert.strictEqual(L.toWslPath('\\\\wsl.localhost\\Ubuntu-22.04\\home\\me'), '/home/me');
    assert.strictEqual(L.toWslPath('/already/linux'), '/already/linux');
});

test('fromWslPath is the inverse for /mnt paths', () => {
    assert.strictEqual(L.fromWslPath('/mnt/c/Users/me/x.orb'), 'C:\\Users\\me\\x.orb');
    assert.strictEqual(L.fromWslPath('/home/me/x.orb'), '/home/me/x.orb');
    assert.strictEqual(L.fromWslPath(L.toWslPath('E:\\a\\b.orb')), 'E:\\a\\b.orb');
});

test('decideMode', () => {
    assert.strictEqual(L.decideMode({ platform: 'win32', setting: 'auto', hasNative: false }), 'wsl');
    assert.strictEqual(L.decideMode({ platform: 'win32', setting: 'auto', hasNative: true }), 'native');
    assert.strictEqual(L.decideMode({ platform: 'linux', setting: 'auto', hasNative: false }), 'native');
    assert.strictEqual(L.decideMode({ platform: 'darwin', setting: 'wsl', hasNative: true }), 'wsl');
    assert.strictEqual(L.decideMode({ platform: 'win32', setting: 'native', hasNative: false }), 'native');
});

test('buildCommand native and wsl', () => {
    const n = L.buildCommand({ platform: 'win32', mode: 'native', args: ['run', { path: 'C:\\x\\a.orb' }] });
    assert.deepStrictEqual(n, { command: 'orb.exe', args: ['run', 'C:\\x\\a.orb'] });
    const l = L.buildCommand({ platform: 'linux', mode: 'native', orbPath: '/opt/orb', args: ['check', { path: '/p/a.orb' }] });
    assert.deepStrictEqual(l, { command: '/opt/orb', args: ['check', '/p/a.orb'] });
    const w = L.buildCommand({ platform: 'win32', mode: 'wsl', wslDistro: 'Ubuntu', cwd: 'C:\\proj', args: ['run', { path: 'C:\\proj\\main.orb' }, '--unchecked'] });
    assert.deepStrictEqual(w, { command: 'wsl.exe', args: ['-d', 'Ubuntu', '--cd', '/mnt/c/proj', '-e', 'orb', 'run', '/mnt/c/proj/main.orb', '--unchecked'] });
});

test('quoting for each shell', () => {
    assert.strictEqual(L.quoteArg('plain-1.orb', 'posix'), 'plain-1.orb');
    assert.strictEqual(L.quoteArg("it's here", 'posix'), "'it'\\''s here'");
    assert.strictEqual(L.quoteArg("it's here", 'powershell'), "'it''s here'");
    assert.strictEqual(L.quoteArg('a b', 'cmd'), '"a b"');
    assert.strictEqual(L.commandLine({ command: 'C:\\Program Files\\orb\\orb.exe', args: ['run', 'my file.orb'] }, 'powershell'),
        "& 'C:\\Program Files\\orb\\orb.exe' run 'my file.orb'");
    assert.strictEqual(L.commandLine({ command: 'orb', args: ['run', 'a.orb'] }, 'posix'), 'orb run a.orb');
});

test('shellFlavor', () => {
    assert.strictEqual(L.shellFlavor('C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', 'win32'), 'powershell');
    assert.strictEqual(L.shellFlavor('C:\\Program Files\\PowerShell\\7\\pwsh.exe', 'win32'), 'powershell');
    assert.strictEqual(L.shellFlavor('C:\\Windows\\System32\\cmd.exe', 'win32'), 'cmd');
    assert.strictEqual(L.shellFlavor('/bin/zsh', 'darwin'), 'posix');
});

test('parseDiagnostics understands real compiler output', () => {
    const out = [
        '/tmp/t/e1.orb:2:14: error: operator \'+\' needs operands of the same type, found i64 and str (use `as` to convert)',
        '    2 |     return x + "a";',
        '      |              ^',
        'C:\\proj\\main.orb:7:3: warning: something',
        '/mnt/c/proj/lib.orb:10:1: note: previous definition is here',
        'orb: 3 errors',
    ].join('\n');
    const d = L.parseDiagnostics(out, L.fromWslPath);
    assert.strictEqual(d.length, 3);
    assert.deepStrictEqual(d[0], { file: '/tmp/t/e1.orb', line: 1, col: 13, severity: 'error', message: "operator '+' needs operands of the same type, found i64 and str (use `as` to convert)" });
    assert.strictEqual(d[1].file, 'C:\\proj\\main.orb');
    assert.strictEqual(d[2].file, 'C:\\proj\\lib.orb');
    assert.strictEqual(d[2].severity, 'note');
});

test('parseDiagnostics works on output produced by the real compiler (if built)', () => {
    const orb = path.join(__dirname, '..', '..', 'build', 'orb');
    if (!fs.existsSync(orb)) return;
    const { spawnSync } = require('child_process');
    const tmp = fs.mkdtempSync(path.join(require('os').tmpdir(), 'orbvs-'));
    const f = path.join(tmp, 'bad.orb');
    fs.writeFileSync(f, 'fn main() {\n    x: i64 = "s";\n    println(nope);\n}\n');
    const r = spawnSync(orb, ['check', f], { encoding: 'utf8' });
    const d = L.parseDiagnostics(r.stderr + r.stdout);
    assert.strictEqual(d.length, 2);
    assert.strictEqual(d[0].line, 1);
    assert.match(d[0].message, /type mismatch/);
    assert.match(d[1].message, /unknown name 'nope'/);
});

test('findOnPath', () => {
    const exists = p => p === path.join('/usr/local/bin', 'orb');
    assert.strictEqual(L.findOnPath('orb', '/bin:/usr/local/bin', 'linux', exists), path.join('/usr/local/bin', 'orb'));
    assert.strictEqual(L.findOnPath('orb', '/bin', 'linux', exists), null);
});

test('defaultExeName', () => {
    assert.strictEqual(L.defaultExeName('/x/main.orb', 'linux'), 'main');
    assert.strictEqual(L.defaultExeName('C:\\x\\main.orb', 'win32'), 'main.exe');
});

test('package.json, grammar, snippets and language config are valid', () => {
    const read = f => JSON.parse(fs.readFileSync(path.join(__dirname, '..', f), 'utf8'));
    const pkg = read('package.json');
    assert.ok(pkg.contributes.commands.length >= 5);
    for (const c of pkg.contributes.commands) assert.ok(c.command.startsWith('orb.'));
    read('language-configuration.json');
    const sn = read('snippets/orb.json');
    assert.ok(Object.keys(sn).length > 10);
    const g = read('syntaxes/orb.tmLanguage.json');
    // every include resolves and every regex compiles
    const repo = g.repository;
    const walk = (node) => {
        if (Array.isArray(node)) return node.forEach(walk);
        if (node && typeof node === 'object') {
            if (node.include && node.include.startsWith('#')) assert.ok(repo[node.include.slice(1)], 'missing rule ' + node.include);
            for (const k of ['match', 'begin', 'end']) if (typeof node[k] === 'string') new RegExp(node[k]);
            Object.values(node).forEach(walk);
        }
    };
    walk(g);
    // sanity: the keyword rule matches real keywords
    const kw = new RegExp(repo.keywords.patterns[0].match);
    assert.ok(kw.test('while') && kw.test('catch') && !kw.test('whilex'));
    // main entry points exist
    assert.ok(fs.existsSync(path.join(__dirname, '..', pkg.main)));
});
