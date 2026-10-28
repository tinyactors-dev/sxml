<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p" from="/doc/part[1]">
    <sch:active pattern="g"/>
  </sch:phase>
  <sch:group id="g">
    <sch:rule context="item">
      <sch:report test="true()">A Item <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">B Item <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
