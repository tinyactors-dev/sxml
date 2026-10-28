<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[@n = 1]">
      <sch:report test="true()">This is <sch:emph>very</sch:emph> <sch:dir value="ltr">much</sch:dir> <sch:span class="x">item <sch:value-of select="@n"/></sch:span>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
