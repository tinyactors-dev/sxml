"""The subset of Markdown that docs/ uses, read into the spec vocabulary.

Used once per import by tools/spec/import.py, while the Markdown files are
still the source (spec/README.md, "The shadow period"). Blocks: paragraphs,
headings, bullet and ordered lists (task boxes too), fenced code, pipe
tables, block quotes, thematic breaks and HTML comments. Inlines: code
spans, links, strong, emphasis and backslash escapes; decision numbers,
packet identifiers and glossary terms become references.
"""
import re
import xml.etree.ElementTree as ElementTree

LIST_MARKER = re.compile(r"^( *)([-*+]|\d+[.)])( +)(.*)$")
TASK_BOX = re.compile(r"^\[([ xX])\] (.*)$")
FENCE = re.compile(r"^( *)(```+|~~~+)(.*)$")
HEADING = re.compile(r"^(#{1,6}) +(.*?) *#*$")
RULE = re.compile(r"^ {0,3}(-{3,}|\*{3,}|_{3,}) *$")
DECISION = re.compile(r"\bD([1-9][0-9]*)\b")


class References:
    """What text may refer to: known packet identifiers and glossary names."""

    def __init__(self, packets=(), terms=()):
        self.packets = set(packets)
        self.terms = set(terms)


def indentation(line):
    return len(line) - len(line.lstrip(" "))


def starts_block(line, list_indent=None):
    """True when LINE begins a block that interrupts a paragraph."""
    stripped = line.lstrip(" ")
    if not stripped:
        return True
    if FENCE.match(line) or HEADING.match(stripped) or RULE.match(line):
        return True
    if stripped.startswith(">") or stripped.startswith("<!--"):
        return True
    marker = LIST_MARKER.match(line)
    if marker:
        if list_indent is not None and indentation(line) > list_indent:
            return True
        bullet = marker.group(2)
        return bullet in "-*+" or bullet in ("1.", "1)")
    return False


DELIMITER_ROW = re.compile(r"^ *\|?( *:?-+:? *\|)* *:?-+:? *\|? *$")


def is_table(lines, position):
    """A pipe table: a row starting with "|" and a delimiter row under it."""
    return (lines[position].lstrip().startswith("|") and position + 1 < len(lines)
            and DELIMITER_ROW.match(lines[position + 1]) is not None)


def parse_blocks(lines, references, parent):
    """Append the blocks of LINES (already dedented) to PARENT."""
    position = 0
    while position < len(lines):
        line = lines[position]
        if not line.strip():
            position += 1
            continue
        fence = FENCE.match(line)
        if fence:
            position = parse_fence(lines, position, fence, parent)
            continue
        heading = HEADING.match(line)
        if heading:
            element = ElementTree.SubElement(parent, "heading", level=str(len(heading.group(1))))
            parse_inline(heading.group(2), references, element)
            position += 1
            continue
        if RULE.match(line):
            ElementTree.SubElement(parent, "rule")
            position += 1
            continue
        if line.lstrip().startswith("<!--"):
            end = position
            while "-->" not in lines[end]:
                end += 1
            text = "\n".join(lines[position:end + 1]).strip()
            ElementTree.SubElement(parent, "comment").text = text[4:-3].strip()
            position = end + 1
            continue
        if is_table(lines, position):
            end = position
            while end < len(lines) and lines[end].lstrip().startswith("|"):
                end += 1
            parse_table(lines[position:end], references, parent)
            position = end
            continue
        if line.lstrip().startswith(">"):
            end = position
            while end < len(lines) and lines[end].lstrip().startswith(">"):
                end += 1
            quote = ElementTree.SubElement(parent, "quote")
            inner = [re.sub(r"^ *> ?", "", text) for text in lines[position:end]]
            parse_blocks(inner, references, quote)
            position = end
            continue
        if LIST_MARKER.match(line):
            position = parse_list(lines, position, references, parent)
            continue
        end = position + 1
        while end < len(lines) and lines[end].strip() and not starts_block(lines[end]) and not is_table(lines, end):
            end += 1
        paragraph = ElementTree.SubElement(parent, "p")
        parse_inline(" ".join(text.strip() for text in lines[position:end]), references, paragraph)
        position = end


def parse_fence(lines, position, fence, parent):
    indent = len(fence.group(1))
    marker = fence.group(2)
    end = position + 1
    while end < len(lines) and not lines[end].strip().startswith(marker):
        end += 1
    body = [text[indent:] if text[:indent].strip() == "" else text.lstrip() for text in lines[position + 1:end]]
    code = ElementTree.SubElement(parent, "pre")
    if fence.group(3).strip():
        code.set("language", fence.group(3).strip())
    code.text = "\n".join(body)
    return end + 1


def parse_list(lines, position, references, parent):
    first = LIST_MARKER.match(lines[position])
    list_indent = len(first.group(1))
    ordered = first.group(2)[0].isdigit()
    element = ElementTree.SubElement(parent, "list")
    if ordered:
        element.set("ordered", "true")
        start = int(first.group(2)[:-1])
        if start != 1:
            element.set("start", str(start))
    while position < len(lines):
        marker = LIST_MARKER.match(lines[position])
        if not marker or len(marker.group(1)) != list_indent or marker.group(2)[0].isdigit() != ordered:
            break
        content_indent = len(marker.group(1)) + len(marker.group(2)) + len(marker.group(3))
        item_lines = [marker.group(4)]
        position += 1
        while position < len(lines):
            line = lines[position]
            if not line.strip():
                following = position + 1
                while following < len(lines) and not lines[following].strip():
                    following += 1
                if following < len(lines) and indentation(lines[following]) >= content_indent:
                    item_lines.extend([""] * (following - position))
                    position = following
                    continue
                break
            if LIST_MARKER.match(line) and indentation(line) <= list_indent:
                break
            if indentation(line) >= content_indent:
                item_lines.append(line[content_indent:])
            elif indentation(line) > list_indent and not LIST_MARKER.match(line):
                item_lines.append(line.lstrip(" "))
            elif not starts_block(line, list_indent) and item_lines[-1].strip():
                item_lines.append(line.lstrip(" "))
            else:
                break
            position += 1
        item = ElementTree.SubElement(element, "item")
        box = TASK_BOX.match(item_lines[0])
        if box:
            item.set("checked", "false" if box.group(1) == " " else "true")
            item_lines[0] = box.group(2)
        parse_blocks(item_lines, references, item)
        while position < len(lines) and not lines[position].strip():
            following = position + 1
            while following < len(lines) and not lines[following].strip():
                following += 1
            next_marker = following < len(lines) and LIST_MARKER.match(lines[following])
            if next_marker and len(next_marker.group(1)) == list_indent:
                element.set("loose", "true")
                position = following
            break
    return position


def split_row(line):
    text = line.strip()
    if text.startswith("|"):
        text = text[1:]
    if text.endswith("|") and not text.endswith("\\|"):
        text = text[:-1]
    # A "|" in a code span does not end the cell. GitHub splits there, but
    # the author meant the code; the assembled table escapes it.
    cells, current, position = [], "", 0
    while position < len(text):
        character = text[position]
        if character == "\\" and position + 1 < len(text):
            current += text[position:position + 2] if text[position + 1] != "|" else "|"
            position += 2
        elif character == "`" and code_span_end(text, position) is not None:
            end = code_span_end(text, position)
            current += text[position:end].replace("\\|", "|")
            position = end
        elif character == "|":
            cells.append(current.strip())
            current = ""
            position += 1
        else:
            current += character
            position += 1
    cells.append(current.strip())
    return cells


def parse_table(lines, references, parent):
    table = ElementTree.SubElement(parent, "table")
    rows = [split_row(line) for line in lines]
    if len(rows) > 1 and all(re.fullmatch(r":?-+:?", cell) for cell in rows[1]):
        alignments = []
        for cell in rows[1]:
            alignments.append({(True, True): "center", (False, True): "right"}.get(
                (cell.startswith(":"), cell.endswith(":")), "left" if cell.startswith(":") else ""))
        if any(alignments):
            table.set("align", " ".join(alignment or "default" for alignment in alignments))
        rows = [rows[0]] + rows[2:]
        header = True
    else:
        header = False
    for number, cells in enumerate(rows):
        row = ElementTree.SubElement(table, "row")
        if header and number == 0:
            row.set("header", "true")
        for text in cells:
            parse_inline(text, references, ElementTree.SubElement(row, "cell"))


def append_text(parent, text):
    if not text:
        return
    if len(parent):
        last = parent[-1]
        last.tail = (last.tail or "") + text
    else:
        parent.text = (parent.text or "") + text


def parse_inline(text, references, parent):
    """Append TEXT's inline content to PARENT."""
    position = 0
    plain = ""

    def flush():
        nonlocal plain
        if plain:
            append_references(plain, references, parent)
            plain = ""

    while position < len(text):
        character = text[position]
        if character == "\\" and position + 1 < len(text) and not text[position + 1].isalnum() and text[position + 1] != " ":
            plain += text[position + 1]
            position += 2
            continue
        if character == "`":
            run = re.match(r"`+", text[position:]).end()
            closing = text.find("`" * run, position + run)
            while closing != -1 and closing + run < len(text) and text[closing + run] == "`":
                closing = text.find("`" * run, closing + run + 1)
            if closing == -1:
                plain += "`" * run
                position += run
                continue
            if closing != -1:
                flush()
                content = text[position + run:closing]
                if len(content) > 1 and content.startswith(" ") and content.endswith(" ") and content.strip():
                    content = content[1:-1]
                if content in references.terms:
                    ElementTree.SubElement(parent, "term", name=content)
                else:
                    ElementTree.SubElement(parent, "code").text = content
                position = closing + run
                continue
        if character == "[":
            link = match_link(text, position)
            if link:
                label, target, end = link
                flush()
                element = ElementTree.SubElement(parent, "link", href=target)
                parse_inline(label, references, element)
                position = end
                continue
        if text.startswith("**", position):
            closing = find_outside_code(text, "**", position + 2)
            if closing > position + 2 and not text[position + 2].isspace() and not text[closing - 1].isspace():
                flush()
                parse_inline(text[position + 2:closing], references, ElementTree.SubElement(parent, "strong"))
                position = closing + 2
                continue
        if character == "*" and position + 1 < len(text) and not text[position + 1].isspace() and text[position + 1] != "*":
            closing = position + 1
            while True:
                closing = find_outside_code(text, "*", closing)
                if closing == -1 or not text[closing - 1].isspace():
                    break
                closing += 1
            if closing != -1 and closing > position + 1 and text[closing:closing + 2] != "**":
                flush()
                parse_inline(text[position + 1:closing], references, ElementTree.SubElement(parent, "em"))
                position = closing + 1
                continue
        plain += character
        position += 1
    flush()


def code_span_end(text, position):
    """The end of the code span opening at POSITION, or None."""
    run = re.match(r"`+", text[position:]).end()
    closing = text.find("`" * run, position + run)
    while closing != -1 and closing + run < len(text) and text[closing + run] == "`":
        closing = text.find("`" * run, closing + run + 1)
    return None if closing == -1 else closing + run


def find_outside_code(text, needle, start):
    """Like str.find, but not inside a code span: code binds tighter."""
    position = start
    while position < len(text):
        if text[position] == "`":
            end = code_span_end(text, position)
            if end is not None:
                position = end
                continue
            position += re.match(r"`+", text[position:]).end()
            continue
        if text.startswith(needle, position):
            return position
        position += 1
    return -1


def match_link(text, position):
    depth, end = 0, position
    while end < len(text):
        if text[end] == "[":
            depth += 1
        elif text[end] == "]":
            depth -= 1
            if depth == 0:
                break
        end += 1
    if end >= len(text) - 1 or text[end + 1] != "(":
        return None
    closing = text.find(")", end + 2)
    if closing == -1:
        return None
    return text[position + 1:end], text[end + 2:closing], closing + 1


PACKET_MENTION = re.compile(r"\b(packets?|Packets?|[Aa]fter) ")
PACKET_IDENTIFIER = re.compile(r"[0-9]+\.[0-9]+[a-z]?|[a-z]+(?:-[a-z]+)*-[0-9]+|xml-mem")


def append_references(text, references, parent):
    """Append plain TEXT, with decision numbers and packet mentions as refs."""
    pieces = []
    for match in DECISION.finditer(text):
        pieces.append((match.start(), match.end(), "decision", "D" + match.group(1)))
    for mention in PACKET_MENTION.finditer(text):
        position = mention.end()
        while True:
            identifier = PACKET_IDENTIFIER.match(text, position)
            if not identifier or identifier.group(0) not in references.packets:
                break
            pieces.append((identifier.start(), identifier.end(), "packet", identifier.group(0)))
            separator = re.match(r",? (?:and |or |to )?|, |–|-(?=[0-9])", text[identifier.end():])
            if not separator:
                break
            position = identifier.end() + separator.end()
    pieces.sort()
    position = 0
    for start, end, kind, target in pieces:
        if start < position:
            continue
        append_text(parent, text[position:start])
        ElementTree.SubElement(parent, "ref", kind=kind, to=target)
        position = end
    append_text(parent, text[position:])


def inline_text(element):
    """The plain text of an inline element, as the reader sees it."""
    return "".join(element.itertext())
