// The flag registry knows every flag, its rules and its percentage.
import { request, type Flag } from "./http.ts";

export async function getFlags(): Promise<Flag[]> {
  return (await request("flag-registry", "/flags")) as Flag[];
}

export async function getFlag(key: string): Promise<Flag> {
  return (await request("flag-registry", `/flags/${key}`)) as Flag;
}

export async function setPercentage(key: string, percentage: number): Promise<void> {
  await request("flag-registry", `/flags/${key}/percentage`, {
    method: "PUT",
    body: JSON.stringify({ percentage }),
  });
}
