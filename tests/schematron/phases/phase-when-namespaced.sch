<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:phase id="none" when="/doc/e:missing">
    <sch:active pattern="a"/>
  </sch:phase>
  <sch:phase id="parts" when="/doc/e:part">
    <sch:active pattern="b"/>
  </sch:phase>
  <sch:pattern id="a">
    <sch:rule context="e:item">
      <sch:report test="@n = 1">Pattern a.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="b">
    <sch:rule context="e:item">
      <sch:report test="@n = 2">Pattern b.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
