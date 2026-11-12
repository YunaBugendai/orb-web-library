/// <reference lib="dom" />

export type OrbSource =
  | string
  | URL
  | ArrayBuffer
  | ArrayBufferView
  | WebAssembly.Module;

export type OrbLogLevel = "log" | "info" | "warn" | "error" | "debug";

export interface OrbWebOptions {
  /** Document used by DOM host functions. Defaults to globalThis.document. */
  document?: Document;
  /** Window-like host used for browser APIs. Defaults to globalThis.window. */
  window?: Window & typeof globalThis;
  /** Override fetch, useful for tests or a controlled host environment. */
  fetch?: typeof fetch;
  /** Receive stdout instead of the default console output. */
  stdout?: (text: string) => void;
  /** Receive stderr instead of the default console error output. */
  stderr?: (text: string) => void;
  /** Receive runtime log messages. */
  log?: (level: OrbLogLevel, text: string) => void;
  /** Called when the Orb! program exits or panics. */
  onexit?: (code: number) => void;
  /** Called when a browser callback traps. */
  onerror?: (error: unknown) => void;
  /** Additional WebAssembly imports, merged with Orb!'s built-in imports. */
  imports?: WebAssembly.Imports;
}

export class OrbExit extends Error {
  readonly code: number;
  constructor(code: number);
}

export class OrbWeb {
  constructor(options?: OrbWebOptions);
  readonly instance: WebAssembly.Instance;
  readonly exports: WebAssembly.Exports | null;
  readonly memory: WebAssembly.Memory | null;
  readonly exited: number | null;
  load(source: OrbSource): Promise<void>;
  run(): void;
}

/** Load and start an Orb! WebAssembly application. */
export function start(source: OrbSource, options?: OrbWebOptions): Promise<OrbWeb>;
