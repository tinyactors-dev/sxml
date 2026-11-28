# Every axis, on real documents: the XDMB tree must answer as the parsed one.
import sqlite3, sys, glob, random
db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
db.load_extension("./xml.so", entrypoint="sqlite3_xml_init")
expressions = [
  "count(//node())", "count(//@*)", "count(//namespace::*)",
  "string-join(//*/name(), ' ')",
  "string-join(for $n in //node() return count($n/preceding-sibling::node()), ',')",
  "string-join(for $n in //node() return count($n/following-sibling::node()), ',')",
  "string-join(for $n in //node() return count($n/ancestor::node()), ',')",
  "string-join(for $n in (//node())[position() mod 7 = 1] return count($n/preceding::node()), ',')",
  "string-join(for $n in (//node())[position() mod 7 = 1] return count($n/following::node()), ',')",
  "string-join(for $e in //* return ($e/*[1]/name(), $e/*[last()]/name(), $e/../name()), ',')",
  "string-join(for $e in //* return in-scope-prefixes($e) => sort() => string-join(':'), ',')",
  "string-join(//@* ! (name() || '=' || .), ',')",
  "string-join(//text() ! string-length(), ',')",
  "string-join(//comment() ! string(), '|')",
  "string-join(//processing-instruction() ! (name() || ' ' || .), '|')",
  "string(/)",
  # serialize(/) differs only in the order of namespace declarations,
  # which XDMB sorts by prefix; deep-equal checks the rest.
  "deep-equal(/, parse-xmlserialize(serialize(/)))",
]
files = sorted(glob.glob(sys.argv[1] + "/**/*.xml", recursive=True))
random.seed(1); random.shuffle(files)
documents = checked = skipped = failures = 0
for path in files[: int(sys.argv[2])]:
    raw = open(path, "rb").read()
    if len(raw) > 200_000: continue
    try:
        text = raw.decode("utf-8")
        blob = db.execute("select xmlparse(?)", (text,)).fetchone()[0]
    except Exception:
        skipped += 1; continue
    documents += 1
    if db.execute("select xmlparse(?) = ?", (blob, blob)).fetchone()[0] != 1:
        failures += 1; print("NOT STABLE", path)
    for e in expressions:
        try: a = db.execute("select xmlquery(?2, ?1)", (text, e)).fetchone()[0]
        except Exception as x: a = "ERR " + str(x)[:60]
        try: b = db.execute("select xmlquery(?2, ?1)", (blob, e)).fetchone()[0]
        except Exception as x: b = "ERR " + str(x)[:60]
        checked += 1
        if a != b:
            failures += 1
            if failures < 15: print("DIFF", path, e, "\n  text:", str(a)[:150], "\n  blob:", str(b)[:150])
print(f"{documents} documents, {skipped} not parsed (not UTF-8 or not well-formed), {checked} comparisons, {failures} failures")
