<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="first" from="/doc/part[1]">
    <sch:active pattern="p"/>
  </sch:phase>
  <sch:pattern id="p">
    <sch:rule context="list" visit-each="//entry">
      <sch:report test="true()">Entry <sch:value-of select="@v"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
