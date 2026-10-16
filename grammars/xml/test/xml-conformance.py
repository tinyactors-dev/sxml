#!/usr/bin/env python3
"""Runs the sxml XML grammar over the W3C XML conformance suite (standalone tests).

Usage: python3 -I grammars/xml/test/xml-conformance.py XMLCONF_DIR WORK_DIR TREE_SITTER_CLI GRAMMAR_DIR
XMLCONF_DIR is the xmlconf/ directory of https://www.w3.org/XML/Test/xmlts20130923.tar.gz.
Prints counts per test type; writes WORK_DIR/results.json. See grammars/xml/README.md.
"""
import codecs, json, os, re, subprocess, sys
import xml.etree.ElementTree as ET

suite = sys.argv[1]          # .../xmlconf
work = sys.argv[2]           # output dir
cli = sys.argv[3]
grammar = sys.argv[4]
os.makedirs(work, exist_ok=True)

top = open(os.path.join(suite, 'xmlconf.xml'), encoding='utf-8').read()
catalogs = re.findall(r'<!ENTITY\s+(\S+)\s+SYSTEM\s+"([^"]+)"', top)
tests = []
for name, rel in catalogs:
    path = os.path.join(suite, rel)
    base = os.path.dirname(path)
    raw = open(path, 'rb').read().decode('utf-8', 'replace')
    raw = re.sub(r'<\?xml[^>]*\?>', '', raw, count=1)
    raw = re.sub(r'<!DOCTYPE[^>\[]*(\[[^\]]*\])?\s*>', '', raw)
    root = ET.fromstring('<wrap>' + raw + '</wrap>')
    for test in root.iter('TEST'):
        attributes = test.attrib
        if attributes.get('ENTITIES', 'none') != 'none':
            continue
        if attributes.get('RECOMMENDATION', 'XML1.0') not in ('XML1.0', 'NS1.0', 'XML1.0-errata2e', 'XML1.0-errata3e', 'XML1.0-errata4e'):
            continue
        if attributes.get('VERSION', '1.0') != '1.0' and '1.0' not in attributes.get('VERSION', ''):
            continue
        edition = attributes.get('EDITION')
        if edition and '5' not in edition.split():
            continue
        tests.append(dict(catalog=name, id=attributes['ID'], type=attributes['TYPE'],
                          namespace=attributes.get('NAMESPACE', 'yes'),
                          path=os.path.join(base, attributes['URI']),
                          sections=attributes.get('SECTIONS', ''),
                          description=' '.join((test.text or '').split())))

def to_utf8(data):
    if data.startswith(codecs.BOM_UTF16_LE) or data.startswith(codecs.BOM_UTF16_BE):
        return '﻿' + data.decode('utf-16')[0:].lstrip('﻿'), 'utf-16'
    if data.startswith(b'<\x00?\x00'):
        return data.decode('utf-16-le'), 'utf-16-le'
    if data.startswith(b'\x00<\x00?'):
        return data.decode('utf-16-be'), 'utf-16-be'
    match = re.match(rb'(?:\xef\xbb\xbf)?<\?xml[^>]*encoding\s*=\s*["\']([A-Za-z0-9._-]+)', data)
    encoding = match.group(1).decode().lower() if match else 'utf-8'
    if encoding in ('utf-8', 'utf8', 'us-ascii', 'ascii', 'utf-16'):
        return None, encoding
    return data.decode(encoding), encoding

paths = []
for index, test in enumerate(tests):
    data = open(test['path'], 'rb').read()
    try:
        text, encoding = to_utf8(data)
    except (LookupError, UnicodeDecodeError) as problem:
        test['transcode'] = 'failed: %s' % problem
        text = None
    if text is None:
        test['input'] = test['path']
    else:
        test['transcode'] = encoding
        test['input'] = os.path.join(work, 'transcoded-%d.xml' % index)
        open(test['input'], 'wb').write(text.encode('utf-8', 'surrogatepass'))
    paths.append(test['input'])

paths_file = os.path.join(work, 'paths.txt')
open(paths_file, 'w').write('\n'.join(paths) + '\n')
result = subprocess.run([cli, 'parse', '-p', grammar, '-q', '--paths', paths_file],
                        capture_output=True, text=True, errors='replace')
has_error = {}
for line in result.stdout.splitlines():
    if 'Parse:' not in line:
        continue
    path = line.split('\t')[0].strip()
    has_error[path] = ('ERROR' in line or 'MISSING' in line)

summary = {}
records = []
for test in tests:
    outcome = 'error' if has_error.get(test['input'], False) else 'clean'
    test['outcome'] = outcome
    key = test['type']
    summary.setdefault(key, {}).setdefault(outcome, 0)
    summary[key][outcome] += 1
    records.append(test)
json.dump(records, open(os.path.join(work, 'results.json'), 'w'), indent=1)
print(json.dumps(summary, indent=1))
