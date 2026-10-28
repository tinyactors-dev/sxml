<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n &lt; 3" diagnostics="used">Below three.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="used">Used.</sch:diagnostic>
    <sch:diagnostic id="unused">Unused.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
