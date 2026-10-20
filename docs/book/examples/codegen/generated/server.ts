// Generated from the API description by generate.xq. Do not edit.
import { serve } from "../runtime.ts";
import type { BookRequest, Book, BookSearch, BookList, LoanRequest, Loan } from "./types.ts";
import { checkBookRequest, checkBook, checkBookSearch, checkBookList, checkLoanRequest, checkLoan } from "./types.ts";

/** What the server does: one method for each remote function. */
export interface Implementation {
  /** Finds one book by its id. */
  getBook(input: BookRequest): Promise<Book>;
  /** Lists the books of a genre, or all books. */
  searchBooks(input: BookSearch): Promise<BookList>;
  /** Lends a book to a member for some days. */
  borrowBook(input: LoanRequest): Promise<Loan>;
}

/** A Node.js request listener that checks every input and output. */
export function createHandler(implementation: Implementation) {
  return serve({
    getBook: {
      kind: "query",
      checkInput: checkBookRequest,
      checkOutput: checkBook,
      run: (input) => implementation.getBook(input),
    },
    searchBooks: {
      kind: "query",
      checkInput: checkBookSearch,
      checkOutput: checkBookList,
      run: (input) => implementation.searchBooks(input),
    },
    borrowBook: {
      kind: "command",
      checkInput: checkLoanRequest,
      checkOutput: checkLoan,
      run: (input) => implementation.borrowBook(input),
    },
  });
}
