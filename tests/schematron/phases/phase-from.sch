<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:phase id="second-part" from="/doc/part[@name = 'second']">
    <sch:active pattern="all"/>
  </sch:phase>
  <sch:pattern id="all">
    <sch:rule context="item">
      <sch:report test="true()">Item <sch:value-of select="@n"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
