<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" schemaVersion="1.0" id="s1">
  <sch:title>Documentation test</sch:title>
  <sch:p>A paragraph about the schema.</sch:p>
  <sch:pattern>
    <sch:title>A pattern</sch:title>
    <sch:p class="note">A paragraph about the pattern.</sch:p>
    <sch:rule context="item">
      <sch:title>A rule</sch:title>
      <sch:p>A paragraph about the rule.</sch:p>
      <sch:report test="@n = 1">Item one.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:p>A closing paragraph.</sch:p>
</sch:schema>
