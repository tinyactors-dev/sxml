// The audit module writes every kill to the audit log and tells the
// owners of the flag through their webhook.
import { query } from "./db.ts";

const webhook = "https://hooks.example.com/vane";

export async function record(flagKey: string, action: string): Promise<void> {
  await query("INSERT INTO audit_events (flag, action) VALUES ($1, $2)", [flagKey, action]);
  await fetch(webhook, {
    method: "POST",
    body: JSON.stringify({ flag: flagKey, action }),
  });
}
