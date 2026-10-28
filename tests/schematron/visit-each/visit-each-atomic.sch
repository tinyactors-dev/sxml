<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="codes" visit-each="tokenize(normalize-space(.), ' ')">
      <sch:report test="string-length(.) != 3">Code "<sch:value-of select="."/>" is not three characters long.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
