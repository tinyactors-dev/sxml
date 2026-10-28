<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="p" from="/doc/part[2]/@n">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="@n">
      <sch:report test="true()">Number <sch:value-of select="."/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
