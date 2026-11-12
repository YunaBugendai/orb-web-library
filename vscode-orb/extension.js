'use strict';
// Orb! for Visual Studio Code: run / build / check commands, inline diagnostics, tasks and CodeLens.
// On Windows the extension uses a native orb.exe when one is available and otherwise routes every
// command through WSL (paths are translated automatically). In a VS Code "WSL" window it is plain Linux.
const vscode = require('vscode');
const cp = require('child_process');
const fs = require('fs');
const path = require('path');
const L = require('./lib');

let diagnostics, output, terminal, warnedMissing = false;
const perDocUris = new Map();

const cfg = () => vscode.workspace.getConfiguration('orb');

function resolveMode() {
    const c = cfg();
    const platform = process.platform;
    const orbPath = c.get('path') || '';
    let hasNative = true;
    if (platform === 'win32') {
        hasNative = !!(orbPath && fs.existsSync(orbPath)) ||
            !!L.findOnPath('orb', process.env.PATH || '', platform, p => { try { return fs.statSync(p).isFile(); } catch (e) { return false; } });
    }
    return { mode: L.decideMode({ platform, setting: c.get('mode') || 'auto', hasNative }), orbPath };
}

// args: strings or { path } objects. progArgs are passed to the program after `--`.
function invocation(args, cwd, progArgs) {
    const { mode, orbPath } = resolveMode();
    const all = [...args, ...(cfg().get('extraArgs') || [])];
    if (progArgs && progArgs.length) all.push('--', ...progArgs);
    const cmd = L.buildCommand({ platform: process.platform, mode, orbPath, wslDistro: cfg().get('wslDistro') || '', args: all, cwd });
    cmd.mode = mode;
    return cmd;
}

async function targetFile(uri) {
    let doc;
    if (uri && uri.fsPath) {
        doc = vscode.workspace.textDocuments.find(d => d.uri.fsPath === uri.fsPath);
        if (!doc) return uri.fsPath;
    } else {
        const ed = vscode.window.activeTextEditor;
        if (!ed) { vscode.window.showWarningMessage('Open an .orb file first.'); return null; }
        doc = ed.document;
    }
    if (doc.languageId !== 'orb' && !doc.fileName.endsWith('.orb')) { vscode.window.showWarningMessage('This is not an .orb file.'); return null; }
    if (doc.isUntitled) { vscode.window.showWarningMessage('Save the file first.'); return null; }
    if (doc.isDirty) await doc.save();
    return doc.fileName;
}

function getTerminal(cwd) {
    if (!terminal || terminal.exitStatus !== undefined) terminal = vscode.window.createTerminal({ name: 'Orb', cwd });
    return terminal;
}

function runInTerminal(cmd, cwd) {
    const t = getTerminal(cwd);
    t.show(true);
    t.sendText(L.commandLine(cmd, L.shellFlavor(vscode.env.shell, process.platform)), true);
}

function showMissing(err) {
    output.appendLine('Could not start the Orb compiler: ' + (err && err.message));
    if (warnedMissing) return;
    warnedMissing = true;
    const { mode } = resolveMode();
    const msg = mode === 'wsl'
        ? 'Orb: could not run `orb` inside WSL. Install it there (see docs/PLATFORMS.md) or set "orb.path".'
        : 'Orb: the `orb` compiler was not found. Install it or set "orb.path" in the settings.';
    vscode.window.showErrorMessage(msg, 'Open Settings', 'Run Doctor').then(c => {
        if (c === 'Open Settings') vscode.commands.executeCommand('workbench.action.openSettings', 'orb.path');
        if (c === 'Run Doctor') vscode.commands.executeCommand('orb.doctor');
    });
}

const norm = p => (process.platform === 'win32' ? path.resolve(p).toLowerCase() : path.resolve(p));

function checkFile(file, docUri) {
    if (!cfg().get('checkOnSave')) return Promise.resolve();
    const cwd = path.dirname(file);
    const cmd = invocation(['check', { path: file }], cwd);
    return new Promise(resolve => {
        cp.execFile(cmd.command, cmd.args, { cwd, timeout: 30000, windowsHide: true, encoding: 'utf8', maxBuffer: 4 << 20 }, (err, stdout, stderr) => {
            if (err && (err.code === 'ENOENT' || err.code === 'EACCES')) { showMissing(err); return resolve(); }
            const text = (stderr || '') + '\n' + (stdout || '');
            const mapPath = cmd.mode === 'wsl' ? L.fromWslPath : (p => p);
            const found = L.parseDiagnostics(text, mapPath);
            const key = docUri.toString();
            for (const u of perDocUris.get(key) || []) diagnostics.delete(vscode.Uri.parse(u));
            const byFile = new Map();
            for (const d of found) {
                let abs = d.file;
                if (!path.isAbsolute(abs)) abs = path.resolve(cwd, abs);
                const k = norm(abs);
                if (!byFile.has(k)) byFile.set(k, { file: abs, list: [] });
                byFile.get(k).list.push(d);
            }
            const used = [];
            for (const { file: f, list } of byFile.values()) {
                if (f.startsWith('<std>')) continue;
                const uri = vscode.Uri.file(f);
                const open = vscode.workspace.textDocuments.find(x => norm(x.fileName) === norm(f));
                const ds = list.map(d => {
                    let range = new vscode.Range(d.line, d.col, d.line, d.col + 1);
                    if (open && d.line < open.lineCount) {
                        const w = open.getWordRangeAtPosition(new vscode.Position(d.line, d.col), /[A-Za-z_][A-Za-z0-9_]*|\S/);
                        if (w) range = w;
                    }
                    const sev = d.severity === 'error' ? vscode.DiagnosticSeverity.Error
                        : d.severity === 'warning' ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Information;
                    const diag = new vscode.Diagnostic(range, d.message, sev);
                    diag.source = 'orb';
                    return diag;
                });
                diagnostics.set(uri, ds);
                used.push(uri.toString());
            }
            perDocUris.set(key, used);
            if (err && !found.length && !(err.code === 1)) output.appendLine('orb check: ' + (stderr || err.message));
            resolve();
        });
    });
}

function maybeCheck(doc) {
    if (doc.languageId === 'orb' && !doc.isUntitled && doc.uri.scheme === 'file') checkFile(doc.fileName, doc.uri);
}

class MainLens {
    provideCodeLenses(doc) {
        const lenses = [];
        for (let i = 0; i < doc.lineCount; i++) {
            if (/^\s*fn\s+main\s*\(/.test(doc.lineAt(i).text)) {
                const r = new vscode.Range(i, 0, i, 0);
                lenses.push(new vscode.CodeLens(r, { title: '$(play) Run', command: 'orb.run', arguments: [doc.uri] }));
                lenses.push(new vscode.CodeLens(r, { title: 'Build', command: 'orb.build', arguments: [doc.uri] }));
            }
        }
        return lenses;
    }
}

function activate(context) {
    diagnostics = vscode.languages.createDiagnosticCollection('orb');
    output = vscode.window.createOutputChannel('Orb');
    context.subscriptions.push(diagnostics, output);
    const { mode } = resolveMode();
    output.appendLine(`Orb extension active (platform ${process.platform}, compiler mode: ${mode})`);

    const reg = (name, fn) => context.subscriptions.push(vscode.commands.registerCommand(name, fn));

    reg('orb.run', async (uri) => {
        const f = await targetFile(uri); if (!f) return;
        runInTerminal(invocation(['run', { path: f }], path.dirname(f)), path.dirname(f));
    });
    reg('orb.runWithArgs', async (uri) => {
        const f = await targetFile(uri); if (!f) return;
        const text = await vscode.window.showInputBox({ prompt: 'Program arguments (space separated)', placeHolder: 'e.g. input.txt --verbose' });
        if (text === undefined) return;
        const args = text.trim() ? text.trim().split(/\s+/) : [];
        runInTerminal(invocation(['run', { path: f }], path.dirname(f), args), path.dirname(f));
    });
    reg('orb.build', async (uri) => {
        const f = await targetFile(uri); if (!f) return;
        const { mode: m } = resolveMode();
        const out = path.join(path.dirname(f), L.defaultExeName(f, m === 'wsl' ? 'linux' : process.platform));
        runInTerminal(invocation(['build', { path: f }, '-o', { path: out }], path.dirname(f)), path.dirname(f));
    });
    reg('orb.check', async (uri) => {
        const f = await targetFile(uri); if (!f) return;
        const prev = cfg().get('checkOnSave');
        await checkFile(f, vscode.Uri.file(f));
        output.appendLine(`checked ${f}`);
        if (!prev) vscode.window.showInformationMessage('Orb: check finished (enable "orb.checkOnSave" for automatic checks).');
    });
    reg('orb.doctor', () => runInTerminal(invocation(['doctor'], undefined), undefined));
    reg('orb.showOutput', () => output.show());

    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(maybeCheck),
        vscode.workspace.onDidOpenTextDocument(maybeCheck),
        vscode.workspace.onDidCloseTextDocument(d => { for (const u of perDocUris.get(d.uri.toString()) || []) diagnostics.delete(vscode.Uri.parse(u)); perDocUris.delete(d.uri.toString()); }),
        vscode.workspace.onDidChangeConfiguration(e => { if (e.affectsConfiguration('orb')) { warnedMissing = false; vscode.workspace.textDocuments.forEach(maybeCheck); } }),
        vscode.languages.registerCodeLensProvider({ language: 'orb' }, new MainLens()),
        vscode.window.onDidCloseTerminal(t => { if (t === terminal) terminal = undefined; })
    );
    vscode.workspace.textDocuments.forEach(maybeCheck);

    // tasks: { "type": "orb", "command": "run" | "build" | "check", "file": "main.orb" }
    const mkTask = (def) => {
        const ed = vscode.window.activeTextEditor;
        const wf = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
        let file = def.file ? (path.isAbsolute(def.file) ? def.file : path.join(wf ? wf.uri.fsPath : '', def.file)) : (ed ? ed.document.fileName : '');
        if (!file) return undefined;
        const cmd = invocation([def.command, { path: file }], path.dirname(file));
        const exec = new vscode.ShellExecution(cmd.command, cmd.args);
        const t = new vscode.Task(def, vscode.TaskScope.Workspace, `${def.command} ${path.basename(file)}`, 'orb', exec, '$orb');
        if (def.command === 'build') t.group = vscode.TaskGroup.Build;
        return t;
    };
    context.subscriptions.push(vscode.tasks.registerTaskProvider('orb', {
        provideTasks: () => ['run', 'build', 'check'].map(c => mkTask({ type: 'orb', command: c })).filter(Boolean),
        resolveTask: (task) => (task.definition.command ? mkTask(task.definition) : undefined),
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
