// The PostgreSQL pool of the Vane service, and a flag as its tables keep it.
import pg from "pg";

export interface Rule {
  attribute: string;
  values: string[];
  percentage: number;
}

export interface Flag {
  key: string;
  name: string;
  state: "stable" | "rolling-out" | "paused";
  percentage: number;
  rules: Rule[];
  owners: string[];
}

const pool = new pg.Pool({ connectionString: process.env.DATABASE_URL });

export async function query(text: string, values: unknown[] = []): Promise<unknown[]> {
  const result = await pool.query(text, values);
  return result.rows;
}
