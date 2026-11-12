'use strict';
// Pure helpers for the Orb! VS Code extension (no dependency on the vscode module, so they can be unit tested).
const path = require('path');

// ---------------------------------------------------------------- WSL path translation
function toWslPath(p) {
    if (typeof p !== 'string') return p;
    let m = /^\\\\(?:wsl\$|wsl\.localhost)\\[^\\]+\\?(.*)$/i.exec(p);          // \\wsl$\Ubuntu\home\me\x.orb
    if (m) return '/' + m[1].replace(/\\/g, '/');
    m = /^([A-Za-z]):[\\/]?(.*)$/.exec(p);                                      // C:\Users\me\x.orb
    if (m) return '/mnt/' + m[1].toLowerCase() + (m[2] ? '/' + m[2].replace(/\\/g, '/') : '');
    return p.replace(/\\/g, '/');
}

function fromWslPath(p) {
    const m = /^\/mnt\/([a-zA-Z])(?:\/(.*))?$/.exec(p);
    if (m) return m[1].toUpperCase() + ':\\' + (m[2] ? m[2].replace(/\//g, '\\') : '');
    return p;
}

// ---------------------------------------------------------------- command construction
function decideMode({ platform, setting, hasNative }) {
    if (setting === 'native') return 'native';
    if (setting === 'wsl') return 'wsl';
    if (platform === 'win32' && !hasNative) return 'wsl';
    return 'native';
}

// args: strings, or { path: '...' } for values that are file system paths (translated for WSL)
function buildCommand({ platform, mode, orbPath, wslDistro, args, cwd }) {
    const plain = args.map(a => (typeof a === 'string' ? a : a.path));
    if (mode === 'wsl') {
        const wargs = [];
        if (wslDistro) wargs.push('-d', wslDistro);
        if (cwd) wargs.push('--cd', toWslPath(cwd));
        wargs.push('-e', orbPath || 'orb');
        for (const a of args) wargs.push(typeof a === 'string' ? a : toWslPath(a.path));
        return { command: 'wsl.exe', args: wargs };
    }
    const exe = orbPath || (platform === 'win32' ? 'orb.exe' : 'orb');
    return { command: exe, args: plain };
}

function quoteArg(arg, flavor) {
    if (/^[A-Za-z0-9_@%+=:,.\/\\-]+$/.test(arg) && !(flavor === 'cmd' && /[&|<>^]/.test(arg))) return arg;
    if (flavor === 'powershell') return "'" + arg.replace(/'/g, "''") + "'";
    if (flavor === 'cmd') return '"' + arg.replace(/"/g, '""') + '"';
    return "'" + arg.replace(/'/g, "'\\''") + "'";
}

function commandLine(cmd, flavor) {
    const parts = [cmd.command, ...cmd.args].map(a => quoteArg(a, flavor));
    if (flavor === 'powershell' && parts[0] !== cmd.command) parts[0] = '& ' + parts[0];
    return parts.join(' ');
}

function shellFlavor(shellPath, platform) {
    const s = (shellPath || '').toLowerCase();
    if (s.includes('pwsh') || s.includes('powershell')) return 'powershell';
    if (/(^|[\\/])cmd(\.exe)?$/.test(s)) return 'cmd';
    if (!s && platform === 'win32') return 'powershell';
    return 'posix';
}

// ---------------------------------------------------------------- compiler output
// Lines look like:  path/to/file.orb:12:5: error: message      (followed by a source excerpt we ignore)
function parseDiagnostics(text, mapPath) {
    const out = [];
    const re = /^(.+):(\d+):(\d+): (error|warning|note): (.*)$/;
    for (const raw of String(text).split(/\r?\n/)) {
        const m = re.exec(raw);
        if (!m) continue;
        out.push({
            file: mapPath ? mapPath(m[1]) : m[1],
            line: Math.max(0, parseInt(m[2], 10) - 1),
            col: Math.max(0, parseInt(m[3], 10) - 1),
            severity: m[4],
            message: m[5],
        });
    }
    return out;
}

// finds an executable on PATH (exists is injected for testing)
function findOnPath(name, envPath, platform, exists) {
    const sep = platform === 'win32' ? ';' : ':';
    const exts = platform === 'win32' ? ['', '.exe', '.cmd', '.bat'] : [''];
    for (const dir of (envPath || '').split(sep)) {
        if (!dir) continue;
        for (const e of exts) {
            const full = path.join(dir, name + e);
            if (exists(full)) return full;
        }
    }
    return null;
}

function defaultExeName(source, platform) {
    const base = source.split(/[\\/]/).pop().replace(/\.orb$/, '');
    return platform === 'win32' ? base + '.exe' : base;
}

module.exports = { toWslPath, fromWslPath, decideMode, buildCommand, quoteArg, commandLine, shellFlavor, parseDiagnostics, findOnPath, defaultExeName };
