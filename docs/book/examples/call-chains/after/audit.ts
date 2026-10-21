// Every unlock is recorded by the audit service.
import { request } from "./http.ts";

export async function record(doorId: string, action: string): Promise<void> {
  await request("audit-service", "/events", {
    method: "POST",
    body: JSON.stringify({ door: doorId, action, at: new Date().toISOString() }),
  });
}
