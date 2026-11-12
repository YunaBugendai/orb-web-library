'use strict';
// Exercises extension.js against a minimal mock of the vscode API, using the real orb compiler for `check`.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const Module = require('module');

const orb = path.join(__dirname, '..', '..', 'build', 'orb');

function makeVscode(state) {
    class Position { constructor(l, c) { this.line = l; this.character = c; } }
    class Range { constructor(a, b, c, d) { this.start = new Position(a, b); this.end = new Position(c, d); } }
    class Diagnostic { constructor(range, message, severity) { this.range = range; this.message = message; this.severity = severity; } }
    const handlers = {};
    const commands = {};
    const config = { path: orb, mode: 'native', wslDistro: '', extraArgs: [], checkOnSave: true };
    return {
        Position, Range, Diagnostic,
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2 },
        Uri: { file: p => ({ fsPath: p, scheme: 'file', toString: () => 'file://' + p }), parse: s => ({ fsPath: s.replace('file://', ''), toString: () => s }) },
        CodeLens: class { constructor(range, command) { this.range = range; this.command = command; } },
        TaskScope: { Workspace: 1 }, TaskGroup: { Build: 'build' },
        ShellExecution: class { constructor(c, a) { this.command = c; this.args = a; } },
        Task: class { constructor(def, scope, name, source, exec, matcher) { Object.assign(this, { definition: def, name, source, execution: exec, problemMatcher: matcher }); } },
        env: { shell: '/bin/bash' },
        languages: {
            createDiagnosticCollection: () => {
                const m = new Map();
                state.diag = m;
                return { set: (u, d) => m.set(u.fsPath, d), delete: u => m.delete(u.fsPath), clear: () => m.clear(), dispose() {} };
            },
            registerCodeLensProvider: (sel, p) => { state.lens = p; return { dispose() {} }; },
        },
        window: {
            createOutputChannel: () => ({ appendLine: l => state.log.push(l), show() {}, dispose() {} }),
            createTerminal: opts => { const t = { sent: [], show() {}, sendText(s) { this.sent.push(s); }, exitStatus: undefined }; state.terminals.push(t); return t; },
            onDidCloseTerminal: () => ({ dispose() {} }),
            showWarningMessage: m => state.warn.push(m), showErrorMessage: m => { state.err.push(m); return Promise.resolve(); },
            showInformationMessage() {}, showInputBox: async () => 'a b',
            get activeTextEditor() { return state.editor; },
        },
        workspace: {
            getConfiguration: () => ({ get: k => config[k] }),
            textDocuments: state.docs,
            workspaceFolders: [{ uri: { fsPath: state.dir } }],
            onDidSaveTextDocument: h => { handlers.save = h; return { dispose() {} }; },
            onDidOpenTextDocument: h => { handlers.open = h; return { dispose() {} }; },
            onDidCloseTextDocument: () => ({ dispose() {} }),
            onDidChangeConfiguration: () => ({ dispose() {} }),
        },
        commands: { registerCommand: (n, f) => { commands[n] = f; return { dispose() {} }; }, executeCommand() {} },
        tasks: { registerTaskProvider: (t, p) => { state.taskProvider = p; return { dispose() {} }; } },
        _commands: commands, _handlers: handlers,
    };
}

test('extension: check publishes diagnostics, run/build use the terminal', async (t) => {
    if (!fs.existsSync(orb)) return t.skip('compiler not built');
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'orbext-'));
    const bad = path.join(dir, 'bad.orb');
    fs.writeFileSync(bad, 'fn main() {\n    x: i64 = "s";\n    println(nope);\n}\n');
    const good = path.join(dir, 'good.orb');
    fs.writeFileSync(good, 'fn main() {\n    println("hi");\n}\n');
    const state = { log: [], warn: [], err: [], terminals: [], docs: [], dir };
    const doc = (f) => ({ fileName: f, uri: { fsPath: f, scheme: 'file', toString: () => 'file://' + f }, languageId: 'orb', isUntitled: false, isDirty: false,
        lineCount: fs.readFileSync(f, 'utf8').split('\n').length, lineAt: i => ({ text: fs.readFileSync(f, 'utf8').split('\n')[i] }),
        getWordRangeAtPosition: () => undefined, save: async () => {} });
    const vs = makeVscode(state);
    const origLoad = Module._load;
    Module._load = function (req, ...rest) { return req === 'vscode' ? vs : origLoad.call(this, req, ...rest); };
    try {
        const ext = require('../extension.js');
        const ctx = { subscriptions: [] };
        ext.activate(ctx);

        // diagnostics via the save handler
        const d = doc(bad);
        state.docs.push(d);
        vs._handlers.save(d);
        await new Promise(r => setTimeout(r, 1500));
        const diags = state.diag.get(bad);
        assert.ok(diags && diags.length === 2, 'expected 2 diagnostics, got ' + JSON.stringify(diags));
        assert.strictEqual(diags[0].range.start.line, 1);
        assert.strictEqual(diags[0].severity, 0);
        assert.match(diags[0].message, /type mismatch/);
        assert.match(diags[1].message, /unknown name 'nope'/);

        // fixing the file clears them
        fs.writeFileSync(bad, 'fn main() {\n    println("ok");\n}\n');
        vs._handlers.save(d);
        await new Promise(r => setTimeout(r, 1500));
        assert.ok(!state.diag.has(bad) || state.diag.get(bad).length === 0);

        // run in terminal
        state.editor = { document: doc(good) };
        await vs._commands['orb.run']();
        assert.strictEqual(state.terminals.length, 1);
        assert.strictEqual(state.terminals[0].sent[0], `${orb} run ${good}`);

        // run with args (uses `--`)
        await vs._commands['orb.runWithArgs']();
        assert.strictEqual(state.terminals[0].sent[1], `${orb} run ${good} -- a b`);

        // build
        await vs._commands['orb.build']();
        assert.strictEqual(state.terminals[0].sent[2], `${orb} build ${good} -o ${path.join(dir, 'good')}`);

        // code lens above main
        const lenses = state.lens.provideCodeLenses(doc(good));
        assert.strictEqual(lenses.length, 2);
        assert.strictEqual(lenses[0].range.start.line, 0);

        // tasks
        const tasks = state.taskProvider.provideTasks();
        assert.strictEqual(tasks.length, 3);
        assert.strictEqual(tasks[0].execution.command, orb);
        assert.strictEqual(tasks[0].problemMatcher, '$orb');
    } finally {
        Module._load = origLoad;
    }
});
