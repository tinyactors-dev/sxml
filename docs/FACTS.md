<!-- Generated from spec/articles by tools/spec/spec generate. Do not edit: change it with tools/spec/spec (spec/README.md). -->

# Facts

Each fact is a value the spec states and does not store: an XQuery expression
over the files that hold it, computed whenever a page or a command shows it, so
text that cites a fact (in source form, {{ID}}) always shows what its source
says now. Add one with tools/spec/spec add fact.

| Fact | What | Value | Read from |
| --- | --- | --- | --- |
| `budget/cli/bytes` | Size budget of the sxml command's WebAssembly module, bytes stripped (javascript/size_budget) | 15,500,000 | `bindings/javascript/CMakeLists.txt` |
| `budget/cli/gzipped` | Size budget of the sxml command's WebAssembly module, bytes gzipped (javascript/size_budget) | 2,900,000 | `bindings/javascript/CMakeLists.txt` |
| `budget/javascript/bytes` | Size budget of the JavaScript package's sxml.wasm, bytes stripped (javascript/size_budget) | 14,000,000 | `bindings/javascript/CMakeLists.txt` |
| `budget/javascript/gzipped` | Size budget of the JavaScript package's sxml.wasm, bytes gzipped (javascript/size_budget) | 2,800,000 | `bindings/javascript/CMakeLists.txt` |
| `conformance/commit` | The commit the conformance report measured | e74d4ad475b6 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/date` | The date of the commit the conformance report measured | 2026-10-20 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/torture-slice` | The torture slice's result in the conformance report | 198/198 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/cases` | XP40 test cases | 47,313 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/errors` | XP40 test cases ending in an error | 0 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/failed` | XP40 test cases failed | 26 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/gate-rate` | XP40 gate rate (D95) | 99.8 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/not-run` | XP40 test cases not run | 14,472 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/passed` | XP40 test cases passed | 32,815 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/rate-of-run` | XP40 passed of run | 99.9 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XP40/run` | XP40 test cases run | 32,841 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/cases` | XQ40 test cases | 47,313 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/errors` | XQ40 test cases ending in an error | 0 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/failed` | XQ40 test cases failed | 74 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/gate-rate` | XQ40 gate rate (D95) | 99.8 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/not-run` | XQ40 test cases not run | 1,081 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/passed` | XQ40 test cases passed | 46,158 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/rate-of-run` | XQ40 passed of run | 99.8 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XQ40/run` | XQ40 test cases run | 46,232 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/cases` | XSD11 test cases | 41,735 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/errors` | XSD11 test cases ending in an error | 0 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/failed` | XSD11 test cases failed | 11 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/gate-rate` | XSD11 gate rate (D95) | 100.0 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/not-run` | XSD11 test cases not run | 125 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/passed` | XSD11 test cases passed | 41,599 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/rate-of-run` | XSD11 passed of run | 100.0 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XSD11/run` | XSD11 test cases run | 41,610 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/cases` | XT40 test cases | 13,867 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/errors` | XT40 test cases ending in an error | 0 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/failed` | XT40 test cases failed | 250 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/gate-rate` | XT40 gate rate (D95) | 96.7 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/not-run` | XT40 test cases not run | 1,349 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/passed` | XT40 test cases passed | 12,268 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/rate-of-run` | XT40 passed of run | 98.0 % | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `conformance/XT40/run` | XT40 test cases run | 12,518 | `docs/CONFORMANCE.md` (tools/conformance-report.py) |
| `qt4-catalog/assertions/assert-true` | Occurrences of assert-true in the results of the QT4 catalog's test cases | 11,638 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/error` | Occurrences of error in the results of the QT4 catalog's test cases | 10,063 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/both` | Test cases of the QT4 catalog that apply to both XP40 and XQ40 | 32,822 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/neither` | Test cases of the QT4 catalog that apply to neither XP40 nor XQ40 | 763 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/no-spec` | Test cases of the QT4 catalog with no spec dependency on the case or its set | 15,593 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/test-cases` | Test cases in the QT4 catalog | 47,313 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/test-sets` | Test sets in the QT4 catalog | 737 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xp40-only` | Test cases of the QT4 catalog that apply to XP40 and not to XQ40 | 106 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xp40/test-cases` | Test cases of the QT4 catalog that apply to XP40 | 32,928 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xp40/test-sets` | Test sets of the QT4 catalog with a test case that applies to XP40 | 540 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xq40-only` | Test cases of the QT4 catalog that apply to XQ40 and not to XP40 | 13,622 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xq40/test-cases` | Test cases of the QT4 catalog that apply to XQ40 | 46,444 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/xq40/test-sets` | Test sets of the QT4 catalog with a test case that applies to XQ40 | 737 | `third_party/qt4tests/catalog.xml` |
| `slice/xpath/cases` | Test cases in the XPath (torture) slice | 198 | `tests/qt4/torture-slice.txt` |
| `slice/xquery/cases` | Test cases in the XQuery slice | 186 | `tests/qt4/xquery-slice.txt` |
| `slice/xsd/cases` | Test cases in the XSD slice | 147 | `tests/xsd/slice.txt` |
| `slice/xslt/cases` | Test cases in the XSLT slice | 154 | `tests/xslt/slice.txt` |
