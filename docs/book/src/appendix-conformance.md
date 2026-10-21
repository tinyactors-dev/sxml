# Appendix B: Conformance

sxml is tested against the test suites of the W3C and others, which
check each specification rule by rule. These are the counts of a
release build on 20 October 2026. A test case counts as *run* unless it
needs a feature sxml does not claim, such as static typing or a
network connection. A test case that expects an error and gets a
different error code counts as passed, as the QT4 catalog allows.

| Test suite | Run | Passed | Rate |
| --- | --: | --: | --: |
| QT4: XPath 4.0 and F&O 4.0 | 32,841 | 32,815 | 99.92 % |
| QT4: XQuery 4.0, with Update and Full Text | 46,232 | 46,158 | 99.84 % |
| XSLT 4.0 | 12,518 | 12,268 | 98.00 % |
| XSD 1.1 | 41,610 | 41,599 | 99.97 % |

| Test suite | Passed |
| --- | --: |
| W3C XML Conformance Test Suite, XML 1.0 and 1.1 | 2,237 of 2,237 |
| html5lib: tokenizer | 7,045 of 7,045 |
| html5lib: tree construction | 1,749 of 1,753 |
| html5lib: encoding | 82 of 82 |
| Invisible XML community test suite | 886 of 886 run |
| tree-sitter grammars' own tests: parse trees and highlights | 1,410 of 1,419 run |

The repository's README has the current counts, and
[`docs/CONFORMANCE.md`](https://github.com/tinyactors-dev/sxml/blob/main/docs/CONFORMANCE.md)
has the results by test set.

## The specifications

sxml follows these specifications. The 4.0 specifications are drafts of
the W3C's QT4 community group, which sxml tracks as they change.

| Area | Specification |
| --- | --- |
| Documents | XML 1.0 (Fifth Edition), XML 1.1, Namespaces in XML 1.0 and 1.1 |
| Data model | XQuery and XPath Data Model (XDM) 4.0 |
| Expressions | XPath 4.0; XPath and XQuery Functions and Operators (F&O) 4.0 |
| Queries | XQuery 4.0; XQuery Update Facility 3.0; XQuery and XPath Full Text 3.0 |
| Transformations | XSLT 4.0, which runs XSLT 1.0, 2.0 and 3.0 stylesheets; EXSLT Common |
| Schemas | W3C XML Schema Definition Language (XSD) 1.1, Parts 1 and 2; DTDs |
| Output | XSLT and XQuery Serialization 4.0 |
| Other formats | The HTML Living Standard (parsing) and the Encoding Standard; Invisible XML 1.0; JSON (RFC 8259); CSV (RFC 4180) |
| Extensions | EXPath Binary 4.0; EXPath File 4.0 |
| Text | Unicode 16.0; the Unicode Collation Algorithm and the Unicode CLDR; the IANA Time Zone Database |
