<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2" diagnostics="second first">Not two.</sch:assert>
      <sch:assert test="@n != 3" diagnostics="first">Not three.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="first">First for <sch:value-of select="@n"/>.</sch:diagnostic>
    <sch:diagnostic id="second">Second.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
