// Three kinds of functions: methods, an arrow function assigned to a
// const, and an arrow function passed as an argument.
class Flag {
  evaluate(user: string): boolean {
    return this.check(user) && log("evaluate");
  }

  check(user: string): boolean {
    return /^beta-/.test(user);
  }
}

const evaluateAll = (flags: Flag[], user: string): boolean[] =>
  flags.map((flag) => flag.evaluate(user));

function log(text: string): boolean {
  console.log(text);
  return true;
}
