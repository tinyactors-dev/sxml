#!/usr/bin/env python3
"""Import docs/ into spec/articles: one XML file per article.

    tools/spec/import.py [--sxml PROGRAM]

While the Markdown files under docs/ are the source (spec/README.md, "The
shadow period"), this script makes the shadow copy from them again: it
replaces spec/articles/ entirely and writes each article in canonical form
(spec/queries/canonicalize.xq). `tools/spec/spec roundtrip` then checks that
the articles assemble back to the same text.

Articles: a decision per "### Dn." of DECISIONS.md, a proposal per item of
its "Proposed" list, a term per glossary row, a packet per "### <packet>"
of docs/tasks/, a log entry per "##" section of LOG.md, and a document for
each Markdown file, which includes the articles taken out of it.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import markdown  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARTICLES = os.path.join(ROOT, "spec", "articles")
DOCUMENTS = (
    ["docs/" + name for name in sorted(os.listdir(os.path.join(ROOT, "docs"))) if name.endswith(".md")]
    + ["docs/tasks/" + name for name in sorted(os.listdir(os.path.join(ROOT, "docs/tasks"))) if name.endswith(".md")]
    + ["docs/agents/" + name for name in sorted(os.listdir(os.path.join(ROOT, "docs/agents"))) if name.endswith(".md")]
)
PACKET_HEADING = re.compile(r"^(" + markdown.PACKET_IDENTIFIER.pattern + r") (.+)$")
DECISION_HEADING = re.compile(r"^D([0-9]+)\. (.+)$")
STATUS_WORDS = {"Done", "Paused", "Design", "Designed", "Revised"}


def document_identifier(path):
    return path[len("docs/"):-len(".md")]


def slug(text, taken):
    base = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "section"
    candidate, number = base, 2
    while candidate in taken:
        candidate, number = "%s-%d" % (base, number), number + 1
    taken.add(candidate)
    return candidate


def read_lines(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as source:
        return source.read().split("\n")


def heading_text(line):
    match = markdown.HEADING.match(line)
    return (len(match.group(1)), match.group(2)) if match else None


def split_sections(lines):
    """[(level, heading, lines)] with level 0 for the text before any heading."""
    sections, current, in_fence = [], (0, None, []), False
    for line in lines:
        if markdown.FENCE.match(line):
            in_fence = not in_fence
        heading = None if in_fence else heading_text(line)
        if heading:
            sections.append(current)
            current = (heading[0], heading[1], [])
        else:
            current[2].append(line)
    sections.append(current)
    return sections


def first_pass():
    """The packet identifiers and glossary names that text may refer to."""
    packets, terms = [], []
    for path in DOCUMENTS:
        if path.startswith("docs/tasks/"):
            for level, heading, _ in split_sections(read_lines(path)):
                match = heading and level == 3 and PACKET_HEADING.match(heading)
                if match:
                    packets.append(match.group(1))
    for line in read_lines("docs/GLOSSARY.md"):
        if line.startswith("| `"):
            terms.extend(re.findall(r"`([^`]+)`", markdown.split_row(line)[0]))
    return markdown.References(packets, terms)


class Importer:
    def __init__(self, references):
        self.references = references
        self.articles = {}  # relative file name -> root element

    def add(self, kind, identifier, element):
        directory = {"decision": "decisions", "proposal": "proposals", "term": "terms",
                     "packet": "packets", "log-entry": "log", "document": "documents"}[kind]
        name = os.path.join(directory, identifier.replace("/", os.sep) + ".xml")
        if name in self.articles:
            raise SystemExit("import: two articles named %s" % name)
        self.articles[name] = element

    def blocks(self, lines, parent):
        markdown.parse_blocks(lines, self.references, parent)

    def title(self, text, parent):
        markdown.parse_inline(text, self.references, ElementTree.SubElement(parent, "title"))

    def document(self, path):
        identifier = document_identifier(path)
        root = ElementTree.Element("document", id=identifier, source=path)
        sections = split_sections(read_lines(path))
        stack = [(0, root)]
        taken = set()
        for level, heading, lines in sections:
            if heading is None:
                self.blocks(lines, root)
                continue
            if level == 1 and stack == [(0, root)] and root.find("title") is None:
                self.title(heading, root)
                root.insert(0, root[-1])
                del root[-1]
                self.document_body(path, None, lines, root)
                continue
            while stack[-1][0] >= level:
                stack.pop()
            parent = stack[-1][1]
            if self.take_article(path, level, heading, lines, parent):
                continue
            section = ElementTree.SubElement(parent, "section", id=slug(heading, taken))
            if level != (2 if parent is root else stack[-1][0] + 1):
                section.set("level", str(level))
            self.title(heading, section)
            self.document_body(path, heading, lines, section)
            stack.append((level, section))
        self.add("document", identifier, root)

    def document_body(self, path, heading, lines, section):
        if path == "docs/GLOSSARY.md":
            return self.glossary(lines, section)
        if path == "docs/DECISIONS.md" and heading == "Proposed":
            return self.proposals(lines, section)
        self.blocks(lines, section)

    def take_article(self, path, level, heading, lines, parent):
        """Make an article of a section that is one; leave an include."""
        if path == "docs/DECISIONS.md" and level == 3:
            match = DECISION_HEADING.match(heading)
            if match:
                identifier = "D" + match.group(1)
                article = ElementTree.Element("decision", id=identifier, status="accepted", source=path)
                self.title(match.group(2), article)
                body = ElementTree.SubElement(article, "body")
                self.blocks(lines, body)
                mark_recorded(body)
                self.add("decision", identifier, article)
                ElementTree.SubElement(parent, "include", kind="decision", to=identifier)
                return True
        if path.startswith("docs/tasks/") and level == 3:
            match = PACKET_HEADING.match(heading)
            if match:
                identifier = match.group(1)
                article = ElementTree.Element("packet", id=identifier, source=path)
                self.title(match.group(2), article)
                body = ElementTree.SubElement(article, "body")
                self.blocks(lines, body)
                mark_status(article, body)
                self.add("packet", identifier, article)
                ElementTree.SubElement(parent, "include", kind="packet", to=identifier)
                return True
        if path == "docs/LOG.md" and level == 2:
            identifier = slug(heading, set())
            article = ElementTree.Element("log-entry", id=identifier, source=path)
            self.title(heading, article)
            self.blocks(lines, ElementTree.SubElement(article, "body"))
            self.add("log-entry", identifier, article)
            ElementTree.SubElement(parent, "include", kind="log-entry", to=identifier)
            return True
        return False

    def glossary(self, lines, section):
        """Each row of the glossary's table is a term article."""
        self.blocks(lines, section)
        definitions = {}
        for table in section.iter("table"):
            for position, row in enumerate(list(table)):
                if row.get("header"):
                    continue
                cells = list(row)
                names = [term.get("name") for term in cells[0] if term.tag == "term"]
                if not names:
                    continue
                first = names[0]
                if first not in definitions:
                    article = ElementTree.Element("term", id=first, source="docs/GLOSSARY.md")
                    for name in names:
                        ElementTree.SubElement(article, "name").text = name
                    definitions[first] = article
                    self.add("term", first, article)
                article = definitions[first]
                definition = cells[1]
                definition.tag = "definition"
                article.append(definition)
                number = len(article.findall("definition"))
                include = ElementTree.Element("include", kind="term", to=first)
                if number > 1:
                    include.set("definition", str(number))
                table.remove(row)
                table.insert(position, include)

    def proposals(self, lines, section):
        """Each item of the Proposed list that starts in bold is a proposal."""
        self.blocks(lines, section)
        taken = set()
        for listing in section.findall("list"):
            for position, item in enumerate(list(listing)):
                paragraph = item.find("p")
                if paragraph is None or paragraph.text or not len(paragraph) or paragraph[0].tag != "strong":
                    continue
                strong = paragraph[0]
                identifier = slug(markdown.inline_text(strong), taken)
                article = ElementTree.Element("proposal", id=identifier, source="docs/DECISIONS.md")
                title = ElementTree.SubElement(article, "title")
                title.text, title[:] = strong.text, list(strong)
                paragraph.text = strong.tail
                paragraph.remove(strong)
                body = ElementTree.SubElement(article, "body")
                body[:] = list(item)
                self.add("proposal", identifier, article)
                include = ElementTree.Element("include", kind="proposal", to=identifier)
                listing.remove(item)
                listing.insert(position, include)


def mark_recorded(body):
    """"Recorded YYYY-MM-DD" at the start of a decision becomes <recorded>."""
    paragraph = body[0] if len(body) and body[0].tag == "p" else None
    match = paragraph is not None and re.match(r"Recorded (\d{4}-\d{2}-\d{2})", paragraph.text or "")
    if match:
        recorded = ElementTree.Element("recorded", date=match.group(1))
        recorded.tail = paragraph.text[match.end():]
        paragraph.text = None
        paragraph.insert(0, recorded)


def mark_status(article, body):
    """"**Done** YYYY-MM-DD" at the start of a packet becomes <status>."""
    paragraph = body[0] if len(body) and body[0].tag == "p" else None
    if paragraph is None or paragraph.text or not len(paragraph) or paragraph[0].tag != "strong":
        return
    strong = paragraph[0]
    if len(strong) or strong.text not in STATUS_WORDS:
        return
    status = ElementTree.Element("status", value=strong.text.lower())
    match = re.match(r" (\d{4}-\d{2}-\d{2})", strong.tail or "")
    rest = strong.tail or ""
    if match:
        status.set("date", match.group(1))
        rest = rest[match.end():]
    status.tail = rest
    paragraph.remove(strong)
    paragraph.insert(0, status)


def canonicalize(program):
    result = subprocess.run([program, "xquery", "--allow-put", "--force", "spec/queries/canonicalize.xq"],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("import: " + result.stderr.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--sxml", default=os.environ.get("SXML", os.path.join(ROOT, "build/release/apps/sxml")))
    arguments = parser.parse_args()
    importer = Importer(first_pass())
    for path in DOCUMENTS:
        importer.document(path)
    if os.path.isdir(ARTICLES):
        shutil.rmtree(ARTICLES)
    written = []
    for name, element in sorted(importer.articles.items()):
        path = os.path.join(ARTICLES, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        ElementTree.ElementTree(element).write(path, encoding="unicode")
        written.append(path)
    canonicalize(arguments.sxml)
    counts = {}
    for name in importer.articles:
        counts[name.split(os.sep)[0]] = counts.get(name.split(os.sep)[0], 0) + 1
    print("import: %d articles (%s)" % (len(written), ", ".join("%s %d" % item for item in sorted(counts.items()))))


if __name__ == "__main__":
    main()
