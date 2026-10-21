// Three kinds of functions: methods, an arrow function assigned to a
// const, and an arrow function passed as an argument.
class Lock {
  open(code: string): boolean {
    return this.check(code) && log("open");
  }

  check(code: string): boolean {
    return /^[0-9]{6}$/.test(code);
  }
}

const openAll = (locks: Lock[], code: string): boolean[] =>
  locks.map((lock) => lock.open(code));

function log(text: string): boolean {
  console.log(text);
  return true;
}
