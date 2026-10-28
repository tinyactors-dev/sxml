<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule abstract="true" id="below-five">
      <sch:assert test="@v &lt; 5">Entry <sch:value-of select="@v"/> below five.</sch:assert>
    </sch:rule>
    <sch:rule context="list" visit-each="entry">
      <sch:extends rule="below-five"/>
    </sch:rule>
  </sch:pattern>
</sch:schema>
