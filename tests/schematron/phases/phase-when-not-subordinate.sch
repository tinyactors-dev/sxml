<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="one-item" when="count(//item) = 1"><sch:active pattern="remote"/></sch:phase>
  <sch:phase id="three-items" when="count(//item) = 3"><sch:active pattern="local"/></sch:phase>
  <sch:pattern id="local">
    <sch:rule context="item">
      <sch:report test="@n = 3">Local three.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="remote" documents="'phase-when-not-subordinate/one.xml'">
    <sch:rule context="item">
      <sch:report test="true()">Remote.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
