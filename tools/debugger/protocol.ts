// NDJSON client for glc's debug server (docs/debugger-plan.md §3).
//
// Requests carry a monotonic id; the server echoes it on responses while
// events (stopped/output/exited) have only "event". Sockets are capability
// gated in nsh.json.

import net from "net";
import type { Socket } from "net";

export type JsonObject = { [key: string]: unknown };

export interface Frame {
  index: number;
  function: string;
  function_index: number;
  file: string;
  line: number;
  ip: number;
  resumed: boolean;
}

export interface FunctionInfo {
  index: number;
  name: string;
  qualified_name: string;
  param_count: number;
  local_count: number;
}

export interface DisasmRow {
  ip: number;
  text: string;
  line: number;
  current: boolean;
}

export interface Variable {
  name: string;
  type: string;
  value: string;
  ref?: number;
}

interface Pending {
  resolve: (value: JsonObject) => void;
  reject: (error: Error) => void;
  timer: number;
}

export class DebugClient {
  private socket: Socket | null = null;
  private buffer = "";
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private listener: ((event: JsonObject) => void) | null = null;

  onEvent(listener: (event: JsonObject) => void): void {
    this.listener = listener;
  }

  get connected(): boolean {
    return this.socket !== null;
  }

  async connect(host: string, port: number): Promise<void> {
    this.close();
    const socket = await net.connect({ host, port });
    this.socket = socket;
    this.buffer = "";
    void this.pump(socket);
  }

  close(): void {
    const socket = this.socket;
    this.socket = null;
    this.buffer = "";
    this.failPending(new Error("debugger disconnected"));
    if (socket !== null) {
      socket.close();
    }
  }

  request<T extends JsonObject = JsonObject>(
    command: string,
    payload: JsonObject = {},
  ): Promise<T> {
    const socket = this.socket;
    if (socket === null) {
      return Promise.reject(new Error("not connected to a debug server"));
    }
    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`debugger request timed out: ${command}`));
      }, 10_000);
      this.pending.set(id, {
        resolve: resolve as (value: JsonObject) => void,
        reject,
        timer,
      });
      const line = JSON.stringify({ id, command, ...payload }) + "\n";
      socket.write(line).catch((error: unknown) => {
        const pending = this.pending.get(id);
        if (pending !== undefined) {
          clearTimeout(pending.timer);
          this.pending.delete(id);
        }
        reject(error instanceof Error ? error : new Error(String(error)));
      });
    });
  }

  private async pump(socket: Socket): Promise<void> {
    const decoder = new TextDecoder();
    try {
      while (this.socket === socket) {
        const chunk = await socket.read();
        if (chunk === null) {
          break;
        }
        this.buffer += decoder.decode(chunk, { stream: true });
        let newline = this.buffer.indexOf("\n");
        while (newline >= 0) {
          const line = this.buffer.slice(0, newline);
          this.buffer = this.buffer.slice(newline + 1);
          if (line.trim().length > 0) {
            this.dispatch(line);
          }
          newline = this.buffer.indexOf("\n");
        }
      }
    } catch (error) {
      if (this.socket === socket) {
        this.failPending(
          error instanceof Error ? error : new Error(String(error)),
        );
      }
    }
    if (this.socket === socket) {
      this.socket = null;
      this.failPending(new Error("debugger connection closed"));
      this.listener?.({ event: "disconnected" });
    }
  }

  private dispatch(line: string): void {
    let message: JsonObject;
    try {
      message = JSON.parse(line) as JsonObject;
    } catch {
      return;
    }

    const id = message["id"];
    if (typeof id === "number") {
      const pending = this.pending.get(id);
      if (pending !== undefined) {
        clearTimeout(pending.timer);
        this.pending.delete(id);
        pending.resolve(message);
      }
      return;
    }

    if (typeof message["event"] === "string") {
      this.listener?.(message);
    }
  }

  private failPending(error: Error): void {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(error);
    }
    this.pending.clear();
  }
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
