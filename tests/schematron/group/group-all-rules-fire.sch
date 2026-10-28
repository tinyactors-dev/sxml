<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="item[@n = 2]">
      <sch:report test="true()">Specific.</sch:report>
    </sch:rule>
    <sch:rule context="item">
      <sch:report test="@n &gt; 1">General <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>
