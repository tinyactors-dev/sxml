// Starts the service, and calls it the way the SDKs and the console do.
import { createServer } from "node:http";
import { createHandler, type Implementation } from "./generated/server.ts";
import { evaluateFlags } from "./generated/browser.ts";
import { getFlags } from "./generated/node.ts";
import { killFlag, rollOutFlag } from "./generated/console.ts";
import type { EvaluationContext } from "./generated/types.ts";
import { settings } from "./runtime.ts";
import { flagStatus } from "./sdk.ts";
import { vane } from "./vane.ts";

// The service, with a count of the status requests it answers.
let statusRequests = 0;
const service: Implementation = {
  ...vane,
  getStatus(input) {
    statusRequests += 1;
    return vane.getStatus(input);
  },
};

const server = createServer(createHandler(service));
server.listen(0);
await new Promise((resolve) => server.once("listening", resolve));
const address = server.address();
settings.endpoint = `http://localhost:${typeof address === "object" ? address?.port : 0}/api`;

async function show(label: string, result: Promise<unknown>) {
  try {
    console.log(label, JSON.stringify(await result));
  } catch (error) {
    console.log(label, "failed:", (error as Error).message);
  }
}

// The backend SDK downloads every flag; the frontend SDK asks about one user.
const user: EvaluationContext = { environment: "staging", user: "user-7", attributes: [{ name: "country", value: "nl" }] };
await show("getFlags", getFlags({ environment: "staging" }));
await show("evaluateFlags", evaluateFlags(user));

// The backend SDK asks for a status twice; the service answers once.
await show("flagStatus", flagStatus("beta-banner"));
await show("flagStatus", flagStatus("beta-banner"));
console.log("status requests:", statusRequests);

// The console pulls the kill switch, and the flag is off for everyone.
await show("killFlag", killFlag({ flag: "new-checkout", owner: "ada@example.com" }));
await show("evaluateFlags", evaluateFlags(user));

// Values the types allow but the schema doesn't: the client refuses them.
await show("rollOutFlag", rollOutFlag({ flag: "new-checkout", percentage: 150, owner: "ada" }));

// A request that skips the client: the service refuses it.
const response = await fetch(`${settings.endpoint}/killFlag`, {
  method: "POST",
  body: JSON.stringify({ flag: "New Checkout", owner: "ada@example.com", force: true }),
});
console.log("fetch", response.status, await response.text());

server.close();
