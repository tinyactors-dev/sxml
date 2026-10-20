// Starts the server, and calls it the way a browser page would.
import { createServer } from "node:http";
import { createHandler } from "./generated/server.ts";
import { borrowBook, getBook, searchBooks } from "./generated/client.ts";
import { settings } from "./runtime.ts";
import { library } from "./library.ts";

const server = createServer(createHandler(library));
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

await show("getBook", getBook({ id: "dune" }));
await show("searchBooks", searchBooks({ genre: "science" }));
await show("borrowBook", borrowBook({ book: "spqr", member: "m0042", days: 14 }));

// A value the types allow but the schema doesn't: the client refuses it.
await show("borrowBook", borrowBook({ book: "spqr", member: "ada", days: 60 }));

// A request that skips the client: the server refuses it.
const response = await fetch(`${settings.endpoint}/borrowBook`, {
  method: "POST",
  body: JSON.stringify({ book: "SPQR", member: "m0042", days: 7, renew: true }),
});
console.log("fetch", response.status, await response.text());

server.close();
