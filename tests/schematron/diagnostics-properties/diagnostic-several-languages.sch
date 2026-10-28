<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n &lt; 3" diagnostics="d-en d-de">Below three.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="d-en" xml:lang="en">The value is too large.</sch:diagnostic>
    <sch:diagnostic id="d-de" xml:lang="de">Der Wert ist zu gross.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
