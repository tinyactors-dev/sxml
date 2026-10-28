<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="restricted" from="/doc/part[1]">
    <sch:active pattern="local"/>
    <sch:active pattern="remote"/>
  </sch:phase>
  <sch:pattern id="local">
    <sch:rule context="item">
      <sch:report test="true()">Local <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="remote" documents="'phase-from-not-subordinate/other.xml'">
    <sch:rule context="item">
      <sch:report test="true()">Remote <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
