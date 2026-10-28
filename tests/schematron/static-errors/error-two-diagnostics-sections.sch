<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n">An item has an n attribute.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics><sch:diagnostic id="d1">One.</sch:diagnostic></sch:diagnostics>
  <sch:diagnostics><sch:diagnostic id="d2">Two.</sch:diagnostic></sch:diagnostics>
</sch:schema>
