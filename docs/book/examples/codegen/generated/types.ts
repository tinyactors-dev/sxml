// Generated from the XSD schema by generate.xq. Do not edit.
import { characterCount, checkBoolean, checkDate, checkDecimal, checkInteger, checkObject, checkString } from "../runtime.ts";

export type BookId = string;

export function checkBookId(value: unknown, path: string, problems: string[]): value is BookId {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!/^(?:[a-z][a-z0-9-]*)$/u.test(value)) problems.push(path + ": must match [a-z][a-z0-9-]*");
  if (characterCount(value) > 40) problems.push(path + ": must have at most 40 characters");
  return problems.length === count;
}

export type MemberId = string;

export function checkMemberId(value: unknown, path: string, problems: string[]): value is MemberId {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!/^(?:m[0-9]{4})$/u.test(value)) problems.push(path + ": must match m[0-9]{4}");
  return problems.length === count;
}

export type Genre = "fiction" | "science" | "history";

export function checkGenre(value: unknown, path: string, problems: string[]): value is Genre {
  if (!checkString(value, path, problems)) return false;
  const count = problems.length;
  if (!["fiction", "science", "history"].includes(value)) problems.push(path + ": must be one of fiction, science, history");
  return problems.length === count;
}

export type LoanDays = number;

export function checkLoanDays(value: unknown, path: string, problems: string[]): value is LoanDays {
  if (!checkInteger(value, path, problems)) return false;
  const count = problems.length;
  if (value < 1) problems.push(path + ": must be at least 1");
  if (value > 28) problems.push(path + ": must be at most 28");
  return problems.length === count;
}

export interface Book {
  id: BookId;
  title: string;
  genre: Genre;
  available: boolean;
}

export function checkBook(value: unknown, path: string, problems: string[]): value is Book {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.id, path + ".id", problems);
  checkString(value.title, path + ".title", problems);
  checkGenre(value.genre, path + ".genre", problems);
  checkBoolean(value.available, path + ".available", problems);
  for (const key of Object.keys(value)) {
    if (!["id", "title", "genre", "available"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}

export interface BookRequest {
  id: BookId;
}

export function checkBookRequest(value: unknown, path: string, problems: string[]): value is BookRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.id, path + ".id", problems);
  for (const key of Object.keys(value)) {
    if (!["id"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}

export interface BookSearch {
  genre?: Genre;
  availableOnly?: boolean;
}

export function checkBookSearch(value: unknown, path: string, problems: string[]): value is BookSearch {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (value.genre !== undefined) checkGenre(value.genre, path + ".genre", problems);
  if (value.availableOnly !== undefined) checkBoolean(value.availableOnly, path + ".availableOnly", problems);
  for (const key of Object.keys(value)) {
    if (!["genre", "availableOnly"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}

export interface BookList {
  books: Book[];
}

export function checkBookList(value: unknown, path: string, problems: string[]): value is BookList {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  if (Array.isArray(value.books)) {
    value.books.forEach((item, index) => checkBook(item, path + ".books[" + index + "]", problems));
  } else {
    problems.push(path + ".books" + ": expected an array");
  }
  for (const key of Object.keys(value)) {
    if (!["books"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}

export interface LoanRequest {
  book: BookId;
  member: MemberId;
  days: LoanDays;
}

export function checkLoanRequest(value: unknown, path: string, problems: string[]): value is LoanRequest {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.book, path + ".book", problems);
  checkMemberId(value.member, path + ".member", problems);
  checkLoanDays(value.days, path + ".days", problems);
  for (const key of Object.keys(value)) {
    if (!["book", "member", "days"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}

export interface Loan {
  book: BookId;
  member: MemberId;
  due: string;
}

export function checkLoan(value: unknown, path: string, problems: string[]): value is Loan {
  if (!checkObject(value, path, problems)) return false;
  const count = problems.length;
  checkBookId(value.book, path + ".book", problems);
  checkMemberId(value.member, path + ".member", problems);
  checkDate(value.due, path + ".due", problems);
  for (const key of Object.keys(value)) {
    if (!["book", "member", "due"].includes(key)) {
      problems.push(path + "." + key + ": is not allowed");
    }
  }
  return problems.length === count;
}
