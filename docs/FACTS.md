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
| `qt4-catalog/assertions/all-of` | Occurrences of all-of in the results of the QT4 catalog's test cases | 1,770 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/any-of` | Occurrences of any-of in the results of the QT4 catalog's test cases | 2,163 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert` | Occurrences of assert in the results of the QT4 catalog's test cases | 2,931 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-deep-eq` | Occurrences of assert-deep-eq in the results of the QT4 catalog's test cases | 5,013 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-empty` | Occurrences of assert-empty in the results of the QT4 catalog's test cases | 927 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-eq` | Occurrences of assert-eq in the results of the QT4 catalog's test cases | 6,500 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-false` | Occurrences of assert-false in the results of the QT4 catalog's test cases | 3,990 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-string-value` | Occurrences of assert-string-value in the results of the QT4 catalog's test cases | 5,989 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-true` | Occurrences of assert-true in the results of the QT4 catalog's test cases | 11,638 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-type` | Occurrences of assert-type in the results of the QT4 catalog's test cases | 1,763 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/assert-xml` | Occurrences of assert-xml in the results of the QT4 catalog's test cases | 2,981 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/error` | Occurrences of error in the results of the QT4 catalog's test cases | 10,063 | `third_party/qt4tests/catalog.xml` |
| `qt4-catalog/assertions/serialization-matches` | Occurrences of serialization-matches in the results of the QT4 catalog's test cases | 794 | `third_party/qt4tests/catalog.xml` |
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
| `xsd-tests/versions/1.0` | Occurrences of the version token 1.0 in the XSD test suite | 144 | `third_party/xsdtests/suite.xml` |
| `xsd-tests/versions/1.1` | Occurrences of the version token 1.1 in the XSD test suite | 1,042 | `third_party/xsdtests/suite.xml` |
| `xsd-tests/versions/full-xpath-in-cta` | Occurrences of the version token full-xpath-in-CTA in the XSD test suite | 30 | `third_party/xsdtests/suite.xml` |
| `xsd-tests/versions/restricted-xpath-in-cta` | Occurrences of the version token restricted-xpath-in-CTA in the XSD test suite | 10 | `third_party/xsdtests/suite.xml` |
| `xsd-tests/versions/unicode-4.0.0` | Occurrences of the version token Unicode_4.0.0 in the XSD test suite | 9 | `third_party/xsdtests/suite.xml` |
| `xsd-tests/versions/unicode-6.0.0` | Occurrences of the version token Unicode_6.0.0 in the XSD test suite | 8 | `third_party/xsdtests/suite.xml` |
| `xslt40-test/assertions/all-of` | Occurrences of all-of in the results of xslt40-test's test cases | 1,273 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert` | Occurrences of assert in the results of xslt40-test's test cases | 8,998 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-message` | Occurrences of assert-message in the results of xslt40-test's test cases | 92 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-message/test-cases` | Test cases of xslt40-test with assert-message in the result | 50 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-result-document` | Occurrences of assert-result-document in the results of xslt40-test's test cases | 156 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-result-document/test-cases` | Test cases of xslt40-test with assert-result-document in the result | 74 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-serialization` | Occurrences of assert-serialization in the results of xslt40-test's test cases | 112 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-string-value` | Occurrences of assert-string-value in the results of xslt40-test's test cases | 350 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-warning` | Occurrences of assert-warning in the results of xslt40-test's test cases | 6 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-xml` | Occurrences of assert-xml in the results of xslt40-test's test cases | 5,215 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert-xml/test-cases` | Test cases of xslt40-test with assert-xml in the result | 5,178 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/assert/test-cases` | Test cases of xslt40-test with assert in the result | 5,345 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/error` | Occurrences of error in the results of xslt40-test's test cases | 2,828 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/error/test-cases` | Test cases of xslt40-test with error in the result | 2,706 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/serialization-matches` | Occurrences of serialization-matches in the results of xslt40-test's test cases | 890 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/assertions/serialization-matches/test-cases` | Test cases of xslt40-test with serialization-matches in the result | 369 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/commented-test-sets` | Test-set entries commented out in the xslt40-test catalog | 28 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/defines-stylesheet` | Test cases of xslt40-test whose source names the stylesheet | 16 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/attr` | Test cases of xslt40-test in the group attr | 1,634 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/decl` | Test cases of xslt40-test in the group decl | 1,497 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/expr` | Test cases of xslt40-test in the group expr | 739 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/fn` | Test cases of xslt40-test in the group fn | 1,339 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/insn` | Test cases of xslt40-test in the group insn | 1,996 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/misc` | Test cases of xslt40-test in the group misc | 3,123 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/strm` | Test cases of xslt40-test in the group strm | 2,564 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/groups/type` | Test cases of xslt40-test in the group type | 975 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/initial-function` | Test cases of xslt40-test with initial-function | 38 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/initial-mode` | Test cases of xslt40-test with initial-mode | 156 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/initial-template` | Test cases of xslt40-test with initial-template | 6,312 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/no-spec` | Test cases of xslt40-test with no spec dependency on the case or its set | 12 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/packages` | Test cases of xslt40-test that use a package, in the test or the environment | 434 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/principal-in-environment` | Test cases of xslt40-test with a principal module only in the environment | 4,708 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/principal-in-test` | Test cases of xslt40-test with a principal module in the test | 9,143 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/scope` | Test cases of xslt40-test in the XSLT 4.0 gate scope | 8,728 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/test-cases` | Test cases in the xslt40-test catalog | 13,867 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/test-sets` | Active test-set entries in the xslt40-test catalog | 250 | `third_party/xslt40-test/catalog.xml` |
| `xslt40-test/xslt40/test-cases` | Test cases of xslt40-test that apply to XSLT 4.0 | 12,639 | `third_party/xslt40-test/catalog.xml` |
