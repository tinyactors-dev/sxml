// The backend: the library's remote functions, written by hand
// against the generated interface.
import type { Implementation } from "./generated/server.ts";
import type { Book } from "./generated/types.ts";

const books: Book[] = [
  { id: "dune", title: "Dune", genre: "fiction", available: true },
  { id: "sicp", title: "Structure and Interpretation of Computer Programs", genre: "science", available: false },
  { id: "spqr", title: "SPQR", genre: "history", available: true },
];

// The example's clock is fixed, so its output is always the same.
const today = new Date("2026-10-20");

export const library: Implementation = {
  async getBook({ id }) {
    const book = books.find((book) => book.id === id);
    if (book === undefined) throw new Error(`no book has the id ${id}`);
    return book;
  },
  async searchBooks({ genre, availableOnly }) {
    return {
      books: books.filter((book) =>
        (genre === undefined || book.genre === genre) && (!availableOnly || book.available)),
    };
  },
  async borrowBook({ book, member, days }) {
    const due = new Date(today.getTime() + days * 24 * 60 * 60 * 1000);
    return { book, member, due: due.toISOString().slice(0, 10) };
  },
};
