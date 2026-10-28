<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n" diagnostics="d1">An item has an n attribute.</sch:assert>
    </sch:rule>
    <sch:diagnostics><sch:diagnostic id="d1">Add n.</sch:diagnostic></sch:diagnostics>
  </sch:pattern>
</sch:schema>
