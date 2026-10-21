// Every kill is recorded by the audit service.
import { request } from "./http.ts";

export async function record(flagKey: string, action: string): Promise<void> {
  await request("audit-service", "/events", {
    method: "POST",
    body: JSON.stringify({ flag: flagKey, action, at: new Date().toISOString() }),
  });
}
