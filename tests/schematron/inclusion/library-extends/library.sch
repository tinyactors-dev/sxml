<?xml version="1.0" encoding="UTF-8"?>
<sch:library xmlns:sch="http://purl.oclc.org/dsdl/schematron">
  <sch:rules>
    <sch:rule abstract="true" id="has-n">
      <sch:assert test="@n" diagnostics="add-n">Library: has n.</sch:assert>
    </sch:rule>
  </sch:rules>
  <sch:pattern id="library-pattern">
    <sch:rule context="item">
      <sch:report test="@n = 3">Library pattern: three.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="add-n">Add an n to this <sch:name/>.</sch:diagnostic>
  </sch:diagnostics>
</sch:library>
