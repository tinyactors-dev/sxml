// Generated from the API description by program.xq and print.xsl. Do not edit.
import { call, expect } from "../runtime.ts";
import type { BookRequest, Book, BookSearch, BookList, LoanRequest, Loan } from "./types.ts";
import { checkBookRequest, checkBook, checkBookSearch, checkBookList, checkLoanRequest, checkLoan } from "./types.ts";

/** Finds one book by its id. */
export async function getBook(input: BookRequest): Promise<Book> {
  expect<BookRequest>(input, checkBookRequest);
  const output = await call("getBook", "query", input);
  return expect<Book>(output, checkBook);
}

/** Lists the books of a genre, or all books. */
export async function searchBooks(input: BookSearch): Promise<BookList> {
  expect<BookSearch>(input, checkBookSearch);
  const output = await call("searchBooks", "query", input);
  return expect<BookList>(output, checkBookList);
}

/** Lends a book to a member for some days. */
export async function borrowBook(input: LoanRequest): Promise<Loan> {
  expect<LoanRequest>(input, checkLoanRequest);
  const output = await call("borrowBook", "command", input);
  return expect<Loan>(output, checkLoan);
}
