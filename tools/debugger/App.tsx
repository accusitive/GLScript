import { effect, List, mount, signal, Scroll, Text, View } from "nsh";
import { Input } from "nsh/base";
import type { ListRef } from "nsh";

import {
  DebugClient,
  errorMessage,
  type DisasmRow,
  type Frame,
  type FunctionInfo,
  type JsonObject,
  type Variable,
} from "./protocol";

const client = new DebugClient();

interface VariableRow extends Variable {
  depth: number;
  key: string;
}

function baseName(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash >= 0 ? path.slice(slash + 1) : path;
}

function numberField(message: JsonObject, name: string, fallback = 0): number {
  const value = message[name];
  return typeof value === "number" ? value : fallback;
}

function stringField(message: JsonObject, name: string, fallback = ""): string {
  const value = message[name];
  return typeof value === "string" ? value : fallback;
}

function App() {
  const host = signal("127.0.0.1");
  const port = signal("9000");

  const connected = signal(false);
  const paused = signal(false);
  const status = signal("disconnected");

  const files = signal<string[]>([]);
  const functions = signal<FunctionInfo[]>([]);
  const currentFile = signal("");
  const sourceText = signal("");
  const breakpoints = signal<Record<string, number[]>>({});
  const currentLine = signal(0);
  const output = signal("");
  let sourceList: ListRef | null = null;

  const selectedFunction = signal(0);
  const bytecode = signal<DisasmRow[]>([]);

  const frames = signal<Frame[]>([]);
  const selectedFrame = signal(0);
  const variables = signal<VariableRow[]>([]);
  const expression = signal("");
  const evalResult = signal("");

  const expanded = new Set<string>();

  const sourceLines = () => {
    const text = sourceText();
    if (text.length === 0) {
      return [];
    }
    const lines = text.split("\n");
    if (lines.length > 0 && lines[lines.length - 1] === "") {
      lines.pop();
    }
    return lines;
  };

  const hasBreakpoint = (line: number) => {
    const file = currentFile();
    return file.length > 0 && (breakpoints()[file] ?? []).includes(line);
  };

  async function loadSource(file: string): Promise<void> {
    if (file.length === 0 || !connected()) {
      return;
    }
    try {
      const response = await client.request("source", { file });
      sourceText.set(stringField(response, "content"));
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function loadDisassembly(index: number): Promise<void> {
    if (!connected()) {
      return;
    }
    selectedFunction.set(index);
    try {
      const response = await client.request("disassemble", {
        function_index: index,
      });
      const rows = response["rows"];
      bytecode.set(Array.isArray(rows) ? (rows as DisasmRow[]) : []);
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function selectFrame(index: number): Promise<void> {
    selectedFrame.set(index);
    expanded.clear();
    variables.set([]);
    try {
      const response = await client.request("scopes", { frame_index: index });
      const locals = response["locals"];
      if (Array.isArray(locals)) {
        variables.set(
          (locals as Variable[]).map((local, i) => ({
            ...local,
            depth: 0,
            key: `${index}/${i}/${local.name}`,
          })),
        );
      }
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function expandVariable(row: VariableRow, index: number): Promise<void> {
    if (row.ref === undefined || expanded.has(row.key)) {
      return;
    }
    expanded.add(row.key);
    try {
      const response = await client.request("variables", { ref: row.ref });
      const children = response["variables"];
      if (!Array.isArray(children)) {
        return;
      }
      const rows = (children as Variable[]).map((child, i) => ({
        ...child,
        depth: row.depth + 1,
        key: `${row.key}/${i}/${child.name}`,
      }));
      variables.update((current) => [
        ...current.slice(0, index + 1),
        ...rows,
        ...current.slice(index + 1),
      ]);
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function loadProgram(): Promise<void> {
    const response = await client.request("program");
    const rawFiles = response["files"];
    const rawFunctions = response["functions"];
    const fileList = Array.isArray(rawFiles)
      ? (rawFiles as unknown[]).map((file) => String(file))
      : [];
    files.set(fileList);
    functions.set(
      Array.isArray(rawFunctions) ? (rawFunctions as FunctionInfo[]) : [],
    );
    if (fileList.length > 0 && !fileList.includes(currentFile())) {
      currentFile.set(fileList[0]);
      await loadSource(fileList[0]);
    }
  }

  async function refreshStack(frameIndex: number): Promise<void> {
    try {
      const response = await client.request("stack_trace");
      const rawFrames = response["frames"];
      frames.set(Array.isArray(rawFrames) ? (rawFrames as Frame[]) : []);
      await selectFrame(frameIndex);
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function handleEvent(event: JsonObject): Promise<void> {
    const name = stringField(event, "event");
    if (name === "stopped") {
      paused.set(true);
      const line = numberField(event, "line");
      currentLine.set(line);
      const reason = stringField(event, "reason");
      const where = stringField(event, "function");
      status.set(`${reason} · ${where}:${line}`);

      const file = stringField(event, "file");
      if (file.length > 0 && file !== currentFile()) {
        currentFile.set(file);
        await loadSource(file);
      }

      const frameIndex = numberField(event, "frame_index");
      await refreshStack(frameIndex);
      await loadDisassembly(numberField(event, "function_index"));
      return;
    }
    if (name === "output") {
      output.update((text) => (text + stringField(event, "text")).slice(-8000));
      return;
    }
    if (name === "exited") {
      paused.set(false);
      connected.set(false);
      status.set(event["failed"] === true ? "script failed" : "script exited");
      return;
    }
    if (name === "disconnected") {
      paused.set(false);
      connected.set(false);
      if (!status().startsWith("script ")) {
        status.set("disconnected");
      }
    }
  }

  client.onEvent((event) => {
    void handleEvent(event);
  });

  async function connect(): Promise<void> {
    const target = Number(port());
    if (!Number.isFinite(target) || target <= 0 || target > 65535) {
      status.set("port must be 1..65535");
      return;
    }
    status.set("connecting…");
    try {
      await client.connect(host(), target);
      connected.set(true);
      status.set("connected");
      await loadProgram();
    } catch (error) {
      connected.set(false);
      status.set(errorMessage(error));
      client.close();
    }
  }

  async function command(name: string): Promise<void> {
    if (!connected()) {
      return;
    }
    paused.set(false);
    try {
      status.set(`${name}…`);
      await client.request(name);
    } catch (error) {
      status.set(errorMessage(error));
    }
  }

  async function evaluate(): Promise<void> {
    if (!connected() || !paused()) {
      return;
    }
    evalResult.set("…");
    try {
      const response = await client.request("evaluate", {
        frame_index: selectedFrame(),
        expression: expression(),
      });
      const error = response["error"];
      if (typeof error === "string") {
        evalResult.set(`error: ${error}`);
      } else {
        evalResult.set(
          `${String(response["value"])}  (${String(response["type"])})`,
        );
      }
    } catch (failure) {
      evalResult.set(errorMessage(failure));
    }
  }

  async function toggleBreakpoint(line: number): Promise<void> {
    const file = currentFile();
    if (file.length === 0) {
      return;
    }
    const next = { ...breakpoints() };
    const lines = new Set(next[file] ?? []);
    if (lines.has(line)) {
      lines.delete(line);
    } else {
      lines.add(line);
    }
    next[file] = [...lines].sort((a, b) => a - b);
    breakpoints.set(next);

    if (connected()) {
      try {
        await client.request("set_breakpoints", {
          file,
          lines: next[file],
        });
      } catch (error) {
        status.set(errorMessage(error));
      }
    }
  }

  async function chooseFile(file: string): Promise<void> {
    currentFile.set(file);
    await loadSource(file);
  }

  effect(() => {
    const line = currentLine();
    if (line > 0 && sourceList !== null) {
      sourceList.scrollToIndex({
        index: Math.max(0, line - 1),
        viewPosition: 0.3,
        animated: true,
      });
    }
  });

  return (
    <View class="flex-1 flex-col bg-slate-950 text-slate-100">
      <View class="flex-row flex-wrap items-center gap-2 border-b border-slate-800 bg-slate-900 px-3 py-2">
        <Text class="mr-1 text-sm font-semibold tracking-wide">glscript</Text>
        <Input
          value={host()}
          onChange={({ value }) => host.set(value)}
          placeholder="host"
          class="w-32 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-xs text-slate-100"
        />
        <Input
          value={port()}
          onChange={({ value }) => port.set(value)}
          placeholder="port"
          class="w-20 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-xs text-slate-100"
        />
        <View
          role="button"
          onPress={() => void connect()}
          class="rounded bg-sky-600 px-3 py-1 active:bg-sky-500"
        >
          <Text class="text-xs font-medium text-white">Connect</Text>
        </View>

        <View class="mx-1 h-5 w-px bg-slate-700" />

        <ToolbarButton
          label="Continue"
          disabled={!connected() || !paused()}
          onPress={() => void command("continue")}
        />
        <ToolbarButton
          label="Pause"
          disabled={!connected() || paused()}
          onPress={() => void command("pause")}
        />
        <ToolbarButton
          label="Step in"
          disabled={!connected() || !paused()}
          onPress={() => void command("step_in")}
        />
        <ToolbarButton
          label="Step over"
          disabled={!connected() || !paused()}
          onPress={() => void command("step_over")}
        />

        <Text class="ml-auto text-xs text-slate-400">{status()}</Text>
      </View>

      <View class="min-h-0 flex-1 flex-row">
        <View class="min-w-0 flex-1 flex-col border-r border-slate-800">
          <View class="flex-row items-center gap-1 border-b border-slate-800 bg-slate-900 px-2 py-1">
            <Text class="mr-2 text-xs font-semibold uppercase tracking-wider text-slate-400">
              Source
            </Text>
            <Scroll horizontal style={{ flexGrow: 0 }}>
              <View class="flex-row gap-1">
                <Each of={files()}>
                  {(file) => (
                    <View
                      role="button"
                      onPress={() => void chooseFile(file())}
                      class="rounded px-2 py-1"
                      classList={{ "bg-slate-700": file() === currentFile() }}
                    >
                      <Text
                        class="text-xs text-slate-200"
                        style={{ numberOfLines: 1 }}
                      >
                        {baseName(file())}
                      </Text>
                    </View>
                  )}
                </Each>
              </View>
            </Scroll>
          </View>

          <List
            style={{ flex: 1 }}
            each={sourceLines()}
            estimatedItemSize={20}
            contentStyle={{ paddingVertical: 4 }}
            ref={(node: ListRef) => {
              sourceList = node;
            }}
          >
            {(line, index) => (
              <View
                class="flex-row items-start px-1"
                classList={{
                  "bg-amber-400/20": index() + 1 === currentLine(),
                }}
                onPress={() => void toggleBreakpoint(index() + 1)}
              >
                <Text
                  class="w-10 text-right text-xs text-slate-500"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {index() + 1}
                </Text>
                <Text class="w-5 text-center text-xs text-rose-400">
                  {hasBreakpoint(index() + 1) ? "●" : " "}
                </Text>
                <Text
                  class="flex-1 text-xs text-slate-200"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {line()}
                </Text>
              </View>
            )}
          </List>
        </View>

        <View class="flex w-[440px] flex-col border-r border-slate-800">
          <View class="border-b border-slate-800 bg-slate-900 px-2 py-1">
            <Text class="text-xs font-semibold uppercase tracking-wider text-slate-400">
              Bytecode
            </Text>
          </View>
          <Scroll horizontal style={{ flexGrow: 0 }}>
            <View class="flex-row gap-1 border-b border-slate-800 px-2 py-1">
              <Each of={functions()}>
                {(fn) => (
                  <View
                    role="button"
                    onPress={() => void loadDisassembly(fn().index)}
                    class="rounded px-2 py-1"
                    classList={{
                      "bg-sky-700": fn().index === selectedFunction(),
                    }}
                  >
                    <Text
                      class="text-xs text-slate-200"
                      style={{ numberOfLines: 1 }}
                    >
                      {fn().name}
                    </Text>
                  </View>
                )}
              </Each>
            </View>
          </Scroll>
          <List
            style={{ flex: 1 }}
            each={bytecode()}
            estimatedItemSize={20}
            contentStyle={{ paddingVertical: 4 }}
          >
            {(instruction) => (
              <View
                class="flex-row items-start px-2"
                classList={{
                  "bg-sky-500/25": instruction().current,
                }}
              >
                <Text
                  class="w-10 text-right text-xs text-slate-500"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {instruction().ip}
                </Text>
                <Text
                  class="flex-1 pl-2 text-xs text-slate-200"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {instruction().text}
                </Text>
              </View>
            )}
          </List>
        </View>
      </View>

      <View class="flex h-64 flex-row border-t border-slate-800">
        <View class="flex w-64 flex-col border-r border-slate-800">
          <PanelTitle title="Stack" />
          <List
            style={{ flex: 1 }}
            each={frames()}
            estimatedItemSize={40}
            contentStyle={{ paddingVertical: 4 }}
          >
            {(frame) => (
              <View
                class="px-2 py-1"
                classList={{
                  "bg-slate-800": frame().index === selectedFrame(),
                }}
                onPress={() => void selectFrame(frame().index)}
              >
                <Text class="text-xs text-slate-200" style={{ numberOfLines: 1 }}>
                  {frame().function}
                </Text>
                <Text class="text-[10px] text-slate-500" style={{ numberOfLines: 1 }}>
                  {baseName(frame().file)}:{frame().line}
                  {frame().resumed ? " · resumed" : ""}
                </Text>
              </View>
            )}
          </List>
        </View>

        <View class="min-w-0 flex-1 flex-col border-r border-slate-800">
          <PanelTitle title="Variables" />
          <List
            style={{ flex: 1 }}
            each={variables()}
            estimatedItemSize={22}
            contentStyle={{ paddingVertical: 4 }}
            empty={
              <Text class="px-3 py-2 text-xs text-slate-500">
                Pause to inspect locals
              </Text>
            }
          >
            {(variable, index) => (
              <View
                class="flex-row items-start px-2 py-0.5"
                onPress={() => void expandVariable(variable(), index())}
              >
                <Text
                  class="text-xs text-slate-500"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {"  ".repeat(variable().depth)}
                  {variable().ref !== undefined ? "▸ " : "  "}
                  {variable().name}
                </Text>
                <Text
                  class="flex-1 pl-2 text-xs text-emerald-300"
                  style={{ fontFamily: "ui-monospace", numberOfLines: 1 }}
                >
                  {variable().value}
                </Text>
                <Text class="pl-2 text-[10px] text-slate-600">
                  {variable().type}
                </Text>
              </View>
            )}
          </List>
        </View>

        <View class="flex w-[380px] flex-col">
          <PanelTitle title="Evaluate" />
          <View class="flex-row gap-2 px-2 py-2">
            <Input
              value={expression()}
              onChange={({ value }) => expression.set(value)}
              onSubmit={() => void evaluate()}
              placeholder="expression, e.g. total * 2"
              class="flex-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-xs text-slate-100"
            />
            <View
              role="button"
              onPress={() => void evaluate()}
              class="rounded bg-emerald-600 px-3 py-1 active:bg-emerald-500"
              classList={{ "opacity-40": !paused() }}
            >
              <Text class="text-xs font-medium text-white">Run</Text>
            </View>
          </View>
          <Text
            class="px-3 text-xs text-amber-300"
            style={{ fontFamily: "ui-monospace", numberOfLines: 3 }}
          >
            {evalResult()}
          </Text>
          <PanelTitle title="Output" />
          <Scroll style={{ flex: 1 }} contentStyle={{ padding: 8 }}>
            <Text
              class="text-[11px] text-slate-400"
              style={{ fontFamily: "ui-monospace" }}
            >
              {output()}
            </Text>
          </Scroll>
        </View>
      </View>
    </View>
  );
}

function ToolbarButton(props: {
  label: string;
  disabled: boolean;
  onPress: () => void;
}) {
  return (
    <View
      role="button"
      disabled={props.disabled}
      onPress={props.onPress}
      class="rounded border border-slate-700 bg-slate-800 px-2 py-1 active:bg-slate-700"
      classList={{ "opacity-40": props.disabled }}
    >
      <Text class="text-xs text-slate-200">{props.label}</Text>
    </View>
  );
}

function PanelTitle(props: { title: string }) {
  return (
    <View class="border-b border-slate-800 bg-slate-900 px-2 py-1">
      <Text class="text-xs font-semibold uppercase tracking-wider text-slate-400">
        {props.title}
      </Text>
    </View>
  );
}

mount(App);
