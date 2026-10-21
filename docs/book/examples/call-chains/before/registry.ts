// The registry module keeps every flag, its targeting rules and its
// percentage in PostgreSQL.
import { query, type Flag } from "./db.ts";

const columns = "key, name, state, percentage, rules, owners";

export async function getFlags(): Promise<Flag[]> {
  return (await query(`SELECT ${columns} FROM flags`)) as Flag[];
}

export async function getFlag(key: string): Promise<Flag> {
  const rows = await query(`SELECT ${columns} FROM flags WHERE key = $1`, [key]);
  return rows[0] as Flag;
}

export async function setPercentage(key: string, percentage: number): Promise<void> {
  await query("UPDATE flags SET percentage = $2 WHERE key = $1", [key, percentage]);
}
