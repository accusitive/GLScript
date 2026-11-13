import { expect, open, test } from "nsh/test";

test("shows the debugger shell disconnected", async () => {
  const app = await open("./App.tsx");
  expect(app.texts()).toContain("disconnected");
  expect(app.texts()).toContain("Connect");
  expect(app.texts()).toContain("Continue");
  expect(app.texts()).toContain("SOURCE");
  expect(app.texts()).toContain("BYTECODE");
});
