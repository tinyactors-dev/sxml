<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="maximum" value="5"/>
  <sch:pattern>
    <sch:rule context="list" visit-each="entry[@v &gt; $maximum]">
      <sch:report test="true()">Over the maximum.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
